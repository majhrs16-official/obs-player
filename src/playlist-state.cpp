#include "playlist-state.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <random>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

#include <obs-module.h>

namespace {

constexpr uint32_t OUTPUT_SAMPLE_RATE = 48000;
constexpr uint32_t OUTPUT_CHANNELS = 2;
constexpr uint32_t PLAYLIST_AUDIO_FRAMES = 1024;

std::mutex registry_mutex;
std::shared_ptr<PlaylistState> global_state;

std::string strip_extension(const std::string &filename)
{
    std::filesystem::path p(filename);
    return p.stem().string();
}

bool is_audio_file(const std::filesystem::path &path)
{
    if (!path.has_extension())
        return false;

    std::string ext = path.extension().string();

    std::transform(
        ext.begin(),
        ext.end(),
        ext.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

    return ext == ".mp3" ||
           ext == ".ogg" ||
           ext == ".opus" ||
           ext == ".wav" ||
           ext == ".flac" ||
           ext == ".aac" ||
           ext == ".m4a" ||
           ext == ".webm";
}

std::string normalize_directory(const std::string &directory)
{
    if (directory.empty())
        return {};

    try {
        return std::filesystem::weakly_canonical(
                   std::filesystem::path(directory))
            .string();
    } catch (...) {
        return directory;
    }
}

class Decoder {
public:
    ~Decoder()
    {
        close();
    }

    bool open(const std::string &filename)
    {
        close();

        if (avformat_open_input(
                &format_ctx,
                filename.c_str(),
                nullptr,
                nullptr) < 0) {
            return false;
        }

        if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
            close();
            return false;
        }

        audio_stream = -1;

        for (unsigned int i = 0; i < format_ctx->nb_streams; ++i) {
            if (format_ctx->streams[i]->codecpar->codec_type ==
                AVMEDIA_TYPE_AUDIO) {
                audio_stream = static_cast<int>(i);
                break;
            }
        }

        if (audio_stream < 0) {
            close();
            return false;
        }

        AVStream *stream = format_ctx->streams[audio_stream];

        const AVCodec *codec =
            avcodec_find_decoder(stream->codecpar->codec_id);

        if (!codec) {
            close();
            return false;
        }

        codec_ctx = avcodec_alloc_context3(codec);

        if (!codec_ctx) {
            close();
            return false;
        }

        if (avcodec_parameters_to_context(
                codec_ctx,
                stream->codecpar) < 0) {
            close();
            return false;
        }

        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            close();
            return false;
        }

        AVChannelLayout output_layout =
    AV_CHANNEL_LAYOUT_STEREO;

	const int swr_result =
		swr_alloc_set_opts2(
			&swr,
			&output_layout,
			AV_SAMPLE_FMT_FLT,
			OUTPUT_SAMPLE_RATE,
			&codec_ctx->ch_layout,
			codec_ctx->sample_fmt,
			codec_ctx->sample_rate,
			0,
			nullptr);

	if (swr_result < 0 || !swr) {
		close();
		return false;
	}

	if (swr_init(swr) < 0) {
		close();
		return false;
	}

        return true;
    }

    void close()
    {
        if (swr) {
            swr_free(&swr);
        }

        if (codec_ctx) {
            avcodec_free_context(&codec_ctx);
        }

        if (format_ctx) {
            avformat_close_input(&format_ctx);
        }

        audio_stream = -1;
    }

    int64_t duration_ms() const
    {
        if (!format_ctx)
            return 0;

        if (format_ctx->duration == AV_NOPTS_VALUE)
            return 0;

        return format_ctx->duration * 1000 / AV_TIME_BASE;
    }

    bool seek_ms(int64_t ms)
    {
        if (!format_ctx || audio_stream < 0)
            return false;

        AVStream *stream = format_ctx->streams[audio_stream];

        int64_t timestamp =
            av_rescale_q(
                ms,
                AVRational{1, 1000},
                stream->time_base);

        if (avformat_seek_file(
                format_ctx,
                audio_stream,
                INT64_MIN,
                timestamp,
                INT64_MAX,
                AVSEEK_FLAG_BACKWARD) < 0) {
            return false;
        }

        avcodec_flush_buffers(codec_ctx);
        swr_convert(swr, nullptr, 0, nullptr, 0);

        return true;
    }

    bool decode(std::deque<float> &output)
    {
        if (!format_ctx || !codec_ctx || !swr)
            return false;

        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();

        if (!packet || !frame) {
            av_packet_free(&packet);
            av_frame_free(&frame);
            return false;
        }

        bool produced = false;

        while (true) {
            int ret = av_read_frame(format_ctx, packet);

            if (ret < 0) {
                av_packet_unref(packet);

                avcodec_send_packet(codec_ctx, nullptr);

                while (true) {
                    ret = avcodec_receive_frame(codec_ctx, frame);

                    if (ret == AVERROR_EOF ||
                        ret == AVERROR(EAGAIN)) {
                        break;
                    }

                    if (ret < 0)
                        break;

                    convert_frame(frame, output);
                    produced = true;
                }

                break;
            }

            if (packet->stream_index != audio_stream) {
                av_packet_unref(packet);
                continue;
            }

            ret = avcodec_send_packet(codec_ctx, packet);

            av_packet_unref(packet);

            if (ret < 0)
                continue;

            while (true) {
                ret = avcodec_receive_frame(codec_ctx, frame);

                if (ret == AVERROR(EAGAIN) ||
                    ret == AVERROR_EOF) {
                    break;
                }

                if (ret < 0)
                    break;

                convert_frame(frame, output);
                produced = true;
            }

            if (produced)
                break;
        }

        av_frame_free(&frame);
        av_packet_free(&packet);

        return produced;
    }

private:
    void convert_frame(
        AVFrame *frame,
        std::deque<float> &output)
    {
        const int64_t delay =
            swr_get_delay(swr, codec_ctx->sample_rate);

        const int out_samples =
            static_cast<int>(
                av_rescale_rnd(
                    delay + frame->nb_samples,
                    OUTPUT_SAMPLE_RATE,
                    codec_ctx->sample_rate,
                    AV_ROUND_UP));

        if (out_samples <= 0)
            return;

        std::vector<float> converted(
            static_cast<std::size_t>(out_samples) *
            OUTPUT_CHANNELS);

        uint8_t *out_data =
            reinterpret_cast<uint8_t *>(converted.data());

        const int produced =
            swr_convert(
                swr,
                &out_data,
                out_samples,
                const_cast<const uint8_t **>(frame->extended_data),
                frame->nb_samples);

        if (produced <= 0)
            return;

        const std::size_t samples =
            static_cast<std::size_t>(produced) *
            OUTPUT_CHANNELS;

        output.insert(
            output.end(),
            converted.begin(),
            converted.begin() + samples);
    }

private:
    AVFormatContext *format_ctx = nullptr;
    AVCodecContext *codec_ctx = nullptr;
    SwrContext *swr = nullptr;

    int audio_stream = -1;
};

} // namespace

std::shared_ptr<PlaylistState> playlist_state_get()
{
    std::lock_guard<std::mutex> lock(registry_mutex);

    if (!global_state) {
        global_state = std::make_shared<PlaylistState>();
    }

    return global_state;
}

PlaylistState::PlaylistState()
{
    decoder_thread =
        std::thread(&PlaylistState::decoder_thread_main, this);
}

PlaylistState::~PlaylistState()
{
    {
        std::lock_guard<std::mutex> lock(mutex);

        shutdown = true;
        active = false;
        playing = false;
        ++generation;
    }

    condition.notify_all();

    if (decoder_thread.joinable()) {
        decoder_thread.join();
    }
}

void PlaylistState::configure(
    const std::string &new_directory,
    int new_repeat_mode,
    bool new_shuffle)
{
    const std::string normalized =
        normalize_directory(new_directory);

    {
        std::lock_guard<std::mutex> lock(mutex);

        directory = normalized;
        repeat_mode = new_repeat_mode;
        shuffle = new_shuffle;

        scan_playlist_locked();

        current = 0;

        if (shuffle && playlist.size() > 1) {
            static std::mt19937 rng{
                std::random_device{}()
            };

            std::shuffle(
                playlist.begin(),
                playlist.end(),
                rng);
        }

++generation;

        played_frames = 0;
        duration_ms = 0;

        seek_pending = false;
        seek_ms = 0;

        playback_start_time_set = false;

        if (!playlist.empty()) {
            current_track_name =
                std::filesystem::path(
                    playlist[current])
                    .filename()
                    .string();
            current_track_name_no_ext =
                strip_extension(current_track_name);
        } else {
            current_track_name.clear();
            current_track_name_no_ext.clear();
        }

        stopped = playlist.empty();
        playing = active && !playlist.empty();
    }

    condition.notify_all();
}

void PlaylistState::scan_playlist_locked()
{
    playlist.clear();

    if (directory.empty())
        return;

    try {
        const std::filesystem::path root(directory);

        if (!std::filesystem::exists(root) ||
            !std::filesystem::is_directory(root)) {
            return;
        }

        for (const auto &entry :
             std::filesystem::directory_iterator(root)) {

            if (!entry.is_regular_file())
                continue;

            if (!is_audio_file(entry.path()))
                continue;

            playlist.push_back(
                entry.path().string());
        }

        std::sort(
            playlist.begin(),
            playlist.end());

    } catch (...) {
        playlist.clear();
    }
}

void PlaylistState::set_active(bool value)
{
    {
        std::lock_guard<std::mutex> lock(mutex);

        active = value;

        if (!active) {
            // Don't stop playback on scene switch - keep playing state
            // Just reset timing for rate limiting when reactivated
            playback_start_time_set = false;
        } else if (!playlist.empty() && !stopped) {
            playing = true;
            playback_start_time_set = false;
        }
    }

    condition.notify_all();
}

void PlaylistState::save_position_and_pause()
{
    blog(LOG_DEBUG, "[obs-music-playlist] PlaylistState::save_position_and_pause: played_frames=%llu, audio_timestamp_ns=%llu, active=%d, playing=%d, stopped=%d, generation=%lu", 
         played_frames, audio_timestamp_ns, active, playing, stopped, generation);
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (!playing || stopped) {
            blog(LOG_DEBUG, "[obs-music-playlist] save_position_and_pause: not playing or already stopped");
            return;
        }

        saved_played_frames = played_frames;
        saved_audio_timestamp_ns = audio_timestamp_ns;
        has_saved_position = true;

        playing = false;
        playback_start_time_set = false;
    }

    condition.notify_all();
    blog(LOG_DEBUG, "[obs-music-playlist] save_position_and_pause: done, has_saved=%d", has_saved_position);
}

void PlaylistState::resume_from_saved_position()
{
    blog(LOG_DEBUG, "[obs-music-playlist] PlaylistState::resume_from_saved_position: has_saved=%d, saved_frames=%llu, saved_ts=%llu, active=%d, playing=%d, stopped=%d, generation=%lu", 
         has_saved_position, saved_played_frames, saved_audio_timestamp_ns, active, playing, stopped, generation);
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (!has_saved_position || stopped) {
            blog(LOG_DEBUG, "[obs-music-playlist] resume_from_saved_position: no saved position or stopped");
            return;
        }

        // Restore position
        played_frames = saved_played_frames;
        audio_timestamp_ns = saved_audio_timestamp_ns;
        rate_limit_timestamp_ns = 0;

        // Seek to saved position
        seek_pending = true;
        seek_ms = static_cast<int64_t>(audio_timestamp_ns / 1000000);

        // Increment generation to force decoder thread to restart at outer loop
        generation++;

        stopped = false;
        playing = true;
        playback_start_time_set = false;
    }

    condition.notify_all();
    blog(LOG_DEBUG, "[obs-music-playlist] resume_from_saved_position: done, new generation=%lu, seek_pending=%d", generation, seek_pending);
}

void PlaylistState::play_pause(bool pause)
{
    blog(LOG_DEBUG, "[obs-music-playlist] PlaylistState::play_pause: pause=%d, active=%d, playing=%d, stopped=%d", 
         pause, active, playing, stopped);
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (playlist.empty()) {
            blog(LOG_DEBUG, "[obs-music-playlist] PlaylistState::play_pause: playlist empty");
            return;
        }

        if (pause) {
            playing = false;
            // Don't reset playback_start_time_set - keep decoder position
        } else {
            // If we were stopped, restart from beginning
            if (stopped) {
                played_frames = 0;
                audio_timestamp_ns = 0;
                rate_limit_timestamp_ns = 0;
                duration_ms = 0;
                seek_pending = true;
                seek_ms = 0;
                stopped = false;
                ++generation;
                blog(LOG_DEBUG, "[obs-music-playlist] play_pause: was stopped, restarting from beginning, generation=%lu", generation);
            } else {
                stopped = false;
            }
            playing = true;
            
            // Resync on resume
            auto now = std::chrono::steady_clock::now();
            if (audio_timestamp_ns > 0) {
                auto audio_duration = std::chrono::nanoseconds(static_cast<int64_t>(audio_timestamp_ns));
                playback_start_time = now - audio_duration;
                playback_start_time_set = true;
                blog(LOG_DEBUG, "[obs-music-playlist] play_pause resume: resynced playback_start_time, audio_timestamp_ns=%llu", audio_timestamp_ns);
            } else {
                playback_start_time_set = false;
            }
        }
    }

    condition.notify_all();
    blog(LOG_DEBUG, "[obs-music-playlist] PlaylistState::play_pause: after, playing=%d, stopped=%d", playing, stopped);
}

void PlaylistState::restart()
{
    blog(LOG_DEBUG, "[obs-music-playlist] PlaylistState::restart: active=%d, playing=%d", active, playing);
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (playlist.empty()) {
            blog(LOG_DEBUG, "[obs-music-playlist] PlaylistState::restart: playlist empty");
            return;
        }

        played_frames = 0;
        seek_ms = 0;
        seek_pending = true;
        audio_timestamp_ns = 0;
        rate_limit_timestamp_ns = 0;

        stopped = false;
        playing = active;

        playback_start_time_set = false;

        ++generation;
    }

    condition.notify_all();
    blog(LOG_DEBUG, "[obs-music-playlist] PlaylistState::restart: after, generation=%lu", generation);
}

void PlaylistState::stop()
{
    blog(LOG_DEBUG, "[obs-music-playlist] PlaylistState::stop");
    {
        std::lock_guard<std::mutex> lock(mutex);

        playing = false;
        stopped = true;

        played_frames = 0;
        audio_timestamp_ns = 0;
        rate_limit_timestamp_ns = 0;

        seek_pending = false;
        seek_ms = 0;

        playback_start_time_set = false;

        ++generation;
    }

    condition.notify_all();
    blog(LOG_DEBUG, "[obs-music-playlist] PlaylistState::stop: after, playing=%d, stopped=%d", playing, stopped);
}

void PlaylistState::next()
{
    blog(LOG_DEBUG, "[obs-music-playlist] PlaylistState::next: current=%zu, size=%zu", current, playlist.size());
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (playlist.empty()) {
            blog(LOG_DEBUG, "[obs-music-playlist] PlaylistState::next: playlist empty");
            return;
        }

        if (current + 1 < playlist.size()) {
            ++current;
        } else {
            if (repeat_mode == REPEAT_PLAYLIST) {
                current = 0;
            } else {
                stopped = true;
                playing = false;
                blog(LOG_DEBUG, "[obs-music-playlist] PlaylistState::next: REPEAT_NONE, stopping");
                condition.notify_all();
                return;
            }
        }

        played_frames = 0;
        duration_ms = 0;
        audio_timestamp_ns = 0;
        rate_limit_timestamp_ns = 0;

        seek_pending = false;
        seek_ms = 0;

        current_track_name =
            std::filesystem::path(
                playlist[current])
                .filename()
                .string();
        current_track_name_no_ext =
            strip_extension(current_track_name);

        stopped = false;
        playing = active;

        playback_start_time_set = false;

        ++generation;
        blog(LOG_DEBUG, "[obs-music-playlist] PlaylistState::next: new current=%zu, track=%s", current, current_track_name.c_str());
    }

    condition.notify_all();
}

void PlaylistState::previous()
{
    blog(LOG_DEBUG, "[obs-music-playlist] PlaylistState::previous: current=%zu", current);
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (playlist.empty()) {
            blog(LOG_DEBUG, "[obs-music-playlist] PlaylistState::previous: playlist empty");
            return;
        }

        if (current > 0) {
            --current;
        } else {
            if (repeat_mode == REPEAT_PLAYLIST) {
                current = playlist.size() - 1;
            } else {
                blog(LOG_DEBUG, "[obs-music-playlist] PlaylistState::previous: REPEAT_NONE, at start");
                condition.notify_all();
                return;
            }
        }

        played_frames = 0;
        duration_ms = 0;
        audio_timestamp_ns = 0;
        rate_limit_timestamp_ns = 0;

        seek_pending = false;
        seek_ms = 0;

        current_track_name =
            std::filesystem::path(
                playlist[current])
                .filename()
                .string();
        current_track_name_no_ext =
            strip_extension(current_track_name);

        stopped = false;
        playing = active;

        playback_start_time_set = false;

        ++generation;
        blog(LOG_DEBUG, "[obs-music-playlist] PlaylistState::previous: new current=%zu, track=%s", current, current_track_name.c_str());
    }

    condition.notify_all();
}

int64_t PlaylistState::get_duration_ms() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return duration_ms;
}

int64_t PlaylistState::get_time_ms() const
{
    std::lock_guard<std::mutex> lock(mutex);

    return static_cast<int64_t>(
        played_frames * 1000 /
        OUTPUT_SAMPLE_RATE);
}

void PlaylistState::set_time_ms(int64_t ms)
{
    if (ms < 0)
        ms = 0;

    {
        std::lock_guard<std::mutex> lock(mutex);

        if (duration_ms > 0 &&
            ms > duration_ms) {
            ms = duration_ms;
        }

        played_frames =
            static_cast<uint64_t>(
                ms * OUTPUT_SAMPLE_RATE / 1000);

        seek_ms = ms;
        seek_pending = true;

        playback_start_time_set = false;

        ++generation;
    }

    condition.notify_all();
}

PlaylistMediaState PlaylistState::get_media_state() const
{
    std::lock_guard<std::mutex> lock(mutex);

    if (stopped)
        return PlaylistMediaState::STOPPED;

    if (playing)
        return PlaylistMediaState::PLAYING;

    return PlaylistMediaState::PAUSED;
}

std::string PlaylistState::current_track() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return current_track_name;
}

std::string PlaylistState::current_track_no_ext() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return current_track_name_no_ext;
}

void PlaylistState::set_audio_source(obs_source_t *source)
{
    std::lock_guard<std::mutex> lock(audio_source_mutex);

    if (audio_source) {
        obs_source_release(audio_source);
    }

    audio_source = source;

    if (audio_source) {
        obs_source_addref(audio_source);
    }
}

std::string PlaylistState::current_filename_locked() const
{
    if (playlist.empty() ||
        current >= playlist.size()) {
        return {};
    }

    return playlist[current];
}

void PlaylistState::advance_after_end_locked()
{
    blog(LOG_DEBUG, "[obs-music-playlist] advance_after_end_locked called, repeat_mode=%d, current=%zu, playlist_size=%zu",
         repeat_mode, current, playlist.size());

    if (playlist.empty()) {
        blog(LOG_DEBUG, "[obs-music-playlist] playlist empty, stopping");
        playing = false;
        stopped = true;
        return;
    }

    if (repeat_mode == REPEAT_SONG) {
        blog(LOG_DEBUG, "[obs-music-playlist] REPEAT_SONG mode, restarting current");
        played_frames = 0;
        duration_ms = 0;
        audio_timestamp_ns = 0;
        rate_limit_timestamp_ns = 0;

        seek_pending = false;
        seek_ms = 0;

        playback_start_time_set = false;

        ++generation;
        return;
    }

    if (current + 1 < playlist.size()) {
        ++current;
        blog(LOG_DEBUG, "[obs-music-playlist] advancing to next song, current=%zu", current);
    } else if (repeat_mode == REPEAT_PLAYLIST) {
            current = 0;
            blog(LOG_DEBUG, "[obs-music-playlist] REPEAT_PLAYLIST mode, wrapping to start");
        } else {
            blog(LOG_DEBUG, "[obs-music-playlist] REPEAT_NONE mode, stopping");
            playing = false;
            stopped = true;
            return;
        }

    played_frames = 0;
    duration_ms = 0;
    audio_timestamp_ns = 0;
    rate_limit_timestamp_ns = 0;

    seek_pending = false;
    seek_ms = 0;

    playback_start_time_set = false;

    current_track_name =
        std::filesystem::path(
            playlist[current])
            .filename()
            .string();
    current_track_name_no_ext =
        strip_extension(current_track_name);

    blog(LOG_DEBUG, "[obs-music-playlist] new track: %s", current_track_name.c_str());

    ++generation;
}

bool PlaylistState::audio_render(
    uint64_t *,
    uint32_t,
    uint32_t,
    float *)
{
    return false;
}

void PlaylistState::decoder_thread_main()
{
    Decoder decoder;

    while (true) {
        std::string filename;
        uint64_t local_generation = 0;
        int64_t local_seek = 0;
        bool do_seek = false;

        {
            std::unique_lock<std::mutex> lock(mutex);

            condition.wait(
                lock,
                [&] {
                    return shutdown ||
                           (active &&
                            playing &&
                            !playlist.empty());
                });

            if (shutdown)
                return;

            filename =
                current_filename_locked();

            local_generation = generation;

            if (seek_pending) {
                local_seek = seek_ms;
                seek_pending = false;
                do_seek = true;
            }
        }

        if (filename.empty())
            continue;

        if (!decoder.open(filename)) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(100));

            std::lock_guard<std::mutex> lock(mutex);

            if (local_generation == generation) {
                advance_after_end_locked();
            }

            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);

            if (local_generation != generation)
                continue;

            duration_ms = decoder.duration_ms();

            if (do_seek) {
                decoder.seek_ms(local_seek);

                // Resync rate limiting after seek
                auto now = std::chrono::steady_clock::now();
                if (audio_timestamp_ns > 0) {
                    auto audio_duration = std::chrono::nanoseconds(static_cast<int64_t>(audio_timestamp_ns));
                    playback_start_time = now - audio_duration;
                    playback_start_time_set = true;
                } else {
                    playback_start_time_set = false;
                }
            }
        }

        bool eof = false;
        std::deque<float> decoded_buffer;
        bool just_seeked = do_seek;

        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex);

                if (shutdown) {
                    return;
                }

                if (!active ||
                    local_generation != generation) {
                    break;
                }

                if (!playing) {
                    condition.wait(lock, [&] {
                        return shutdown ||
                               !active ||
                               playing ||
                               local_generation != generation;
                    });
                    continue;
                }

                // Check if seek is pending (e.g., after scene switch resume)
                if (seek_pending) {
                    break;
                }
            }

            // Only decode more if buffer is low
            constexpr std::size_t MIN_BUFFER_FRAMES = 4800; // 100ms at 48kHz
            if (decoded_buffer.size() < MIN_BUFFER_FRAMES * OUTPUT_CHANNELS) {
                std::deque<float> new_decoded;
                if (!decoder.decode(new_decoded)) {
                    eof = true;
                } else {
                    decoded_buffer.insert(decoded_buffer.end(), new_decoded.begin(), new_decoded.end());
                }
            }

            if (decoded_buffer.empty()) {
                if (eof) break;
                continue;
            }

            if (just_seeked) {
                just_seeked = false;
                // Rate limiting starts fresh from 0 after seek (rate_limit_timestamp_ns was reset)
                playback_start_time_set = false;
            }

            obs_source_t *source = nullptr;

            {
                std::lock_guard<std::mutex> lock(audio_source_mutex);
                source = audio_source;
                if (source) {
                    obs_source_addref(source);
                }
            }

            if (!source) {
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(mutex);

                if (shutdown) {
                    obs_source_release(source);
                    return;
                }

                if (local_generation != generation) {
                    obs_source_release(source);
                    break;
                }

                if (!active) {
                    obs_source_release(source);
                    break;
                }
            }

            // Limit frames per push to avoid buffer overflow after seek
            constexpr std::size_t MAX_FRAMES_PER_PUSH = 4800; // 100ms at 48kHz
            const std::size_t total_samples = decoded_buffer.size();
            std::size_t frames = total_samples / OUTPUT_CHANNELS;

            if (frames > MAX_FRAMES_PER_PUSH) {
                frames = MAX_FRAMES_PER_PUSH;
            }

            std::vector<float> left(frames);
            std::vector<float> right(frames);

            for (std::size_t i = 0; i < frames; ++i) {
                left[i] = decoded_buffer[i * 2];
                right[i] = decoded_buffer[i * 2 + 1];
            }

            // Remove pushed samples from buffer
            if (total_samples > frames * OUTPUT_CHANNELS) {
                decoded_buffer.erase(decoded_buffer.begin(),
                    decoded_buffer.begin() + frames * OUTPUT_CHANNELS);
            } else {
                decoded_buffer.clear();
            }

            obs_source_audio audio_data = {};
            audio_data.data[0] = reinterpret_cast<uint8_t *>(left.data());
            audio_data.data[1] = reinterpret_cast<uint8_t *>(right.data());
            audio_data.frames = static_cast<uint32_t>(frames);
            audio_data.speakers = SPEAKERS_STEREO;
            audio_data.samples_per_sec = OUTPUT_SAMPLE_RATE;
            audio_data.format = AUDIO_FORMAT_FLOAT_PLANAR;
            audio_data.timestamp = audio_timestamp_ns;

            obs_source_output_audio(source, &audio_data);

            audio_timestamp_ns +=
                static_cast<uint64_t>(frames) *
                1000000000ULL /
                OUTPUT_SAMPLE_RATE;
            rate_limit_timestamp_ns +=
                static_cast<uint64_t>(frames) *
                1000000000ULL /
                OUTPUT_SAMPLE_RATE;

            played_frames += frames;

            // Rate limiting: sleep to match real-time playback
            auto now = std::chrono::steady_clock::now();
            if (!playback_start_time_set) {
                playback_start_time = now;
                playback_start_time_set = true;
            } else {
                auto expected_elapsed_ns =
                    static_cast<int64_t>(rate_limit_timestamp_ns);
                auto real_elapsed_ns =
                    std::chrono::duration_cast<
                        std::chrono::nanoseconds>(
                        now - playback_start_time)
                        .count();
                int64_t sleep_ns = expected_elapsed_ns - real_elapsed_ns;
                if (sleep_ns > 1000000) { // sleep if ahead by > 1ms
                    std::this_thread::sleep_for(
                        std::chrono::nanoseconds(sleep_ns));
                }
            }

            obs_source_release(source);
        }

        if (eof) {
            blog(LOG_DEBUG, "[obs-music-playlist] EOF reached, local_gen=%lu, gen=%lu, active=%d, playing=%d",
                 local_generation, generation, active, playing);

            std::lock_guard<std::mutex> lock(mutex);

            if (local_generation == generation &&
                active &&
                playing) {
                advance_after_end_locked();
            }

            condition.notify_all();
        }
    }
}
