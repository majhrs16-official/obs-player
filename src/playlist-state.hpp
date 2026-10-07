#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <obs-module.h>

enum RepeatMode {
    REPEAT_PLAYLIST = 0,
    REPEAT_SONG = 1,
    REPEAT_NONE = 2
};

enum class PlaylistMediaState {
    STOPPED,
    PLAYING,
    PAUSED
};

class PlaylistState {
public:
    PlaylistState();
    ~PlaylistState();

    PlaylistState(const PlaylistState &) = delete;
    PlaylistState &operator=(const PlaylistState &) = delete;

    void configure(
        const std::string &directory,
        int repeat_mode,
        bool shuffle);

    void set_active(bool active);

    void play_pause(bool pause);
    void restart();
    void stop();
    void next();
    void previous();

    int64_t get_duration_ms() const;
    int64_t get_time_ms() const;
    void set_time_ms(int64_t ms);

    PlaylistMediaState get_media_state() const;

    std::string current_track() const;
    std::string current_track_no_ext() const;

    bool audio_render(
        uint64_t *timestamp_ns,
        uint32_t frames,
        uint32_t channels,
        float *buffer);

    void set_audio_source(obs_source_t *source);

private:
    void decoder_thread_main();

    void scan_playlist_locked();

    void advance_after_end_locked();

    std::string current_filename_locked() const;

private:
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::thread decoder_thread;

    bool shutdown = false;
    bool active = false;
    bool playing = false;
    bool stopped = true;

    std::string directory;

    std::vector<std::string> playlist;
    std::size_t current = 0;

    int repeat_mode = REPEAT_PLAYLIST;
    bool shuffle = false;

    std::string current_track_name;
    std::string current_track_name_no_ext;

    int64_t duration_ms = 0;

    uint64_t played_frames = 0;
    uint64_t generation = 0;

    bool seek_pending = false;
    int64_t seek_ms = 0;

    uint64_t audio_timestamp_ns = 0;

    std::chrono::steady_clock::time_point playback_start_time;
    bool playback_start_time_set = false;

    obs_source_t *audio_source = nullptr;
    std::mutex audio_source_mutex;
};

std::shared_ptr<PlaylistState> playlist_state_get();
