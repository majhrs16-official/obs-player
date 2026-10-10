#include <obs-module.h>

#include "playlist-state.hpp"

#include <memory>

namespace {

#define AUDIO_SOURCE_ID "obs-music-playlist-audio"

struct MusicPlaylistAudio {
    std::shared_ptr<PlaylistState> state;
    obs_source_t *source = nullptr;
};

static obs_properties_t *audio_properties(void *)
{
    obs_properties_t *props =
        obs_properties_create();

    obs_properties_add_path(
        props,
        "directory",
        "Ruta de música",
        OBS_PATH_DIRECTORY,
        "Directorio que contiene los archivos de música",
        nullptr);

    obs_property_t *repeat =
        obs_properties_add_list(
            props,
            "repeat_mode",
            "Repetición",
            OBS_COMBO_TYPE_LIST,
            OBS_COMBO_FORMAT_INT);

    obs_property_list_add_int(
        repeat,
        "Repetir playlist",
        REPEAT_PLAYLIST);

    obs_property_list_add_int(
        repeat,
        "Repetir canción",
        REPEAT_SONG);

    obs_property_list_add_int(
        repeat,
        "No repetir",
        REPEAT_NONE);

    obs_properties_add_bool(
        props,
        "shuffle",
        "Aleatorio");

    return props;
}

static void audio_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(
        settings,
        "directory",
        "");

    obs_data_set_default_int(
        settings,
        "repeat_mode",
        REPEAT_PLAYLIST);

    obs_data_set_default_bool(
        settings,
        "shuffle",
        false);
}

static void audio_update(
    void *data,
    obs_data_t *settings)
{
    auto *audio =
        static_cast<MusicPlaylistAudio *>(data);

    if (!audio)
        return;

    const char *directory =
        obs_data_get_string(
            settings,
            "directory");

    const int repeat_mode =
        static_cast<int>(
            obs_data_get_int(
                settings,
                "repeat_mode"));

    const bool shuffle =
        obs_data_get_bool(
            settings,
            "shuffle");

    /*
     * SIEMPRE obtenemos la instancia global.
     * No existe un estado independiente por ruta.
     */
    audio->state =
        playlist_state_get();

    audio->state->configure(
        directory ? directory : "",
        repeat_mode,
        shuffle);
}

static void *audio_create(
    obs_data_t *settings,
    obs_source_t *source)
{
    auto *audio =
        new MusicPlaylistAudio();

    audio->source = source;
    audio->state =
        playlist_state_get();

    audio_update(
        audio,
        settings);

    return audio;
}

static void audio_destroy(void *data)
{
    auto *audio =
        static_cast<MusicPlaylistAudio *>(data);

    delete audio;
}

static void audio_activate(void *data)
{
    blog(LOG_INFO, "[obs-music-playlist] audio_activate: data=%p, source=%p", data, data ? static_cast<MusicPlaylistAudio*>(data)->source : nullptr);
    auto *audio =
        static_cast<MusicPlaylistAudio *>(data);

    if (!audio || !audio->state || !audio->source) {
        blog(LOG_ERROR, "[obs-music-playlist] audio_activate: null check failed audio=%p state=%p source=%p", audio, audio ? audio->state : nullptr, audio ? audio->source : nullptr);
        return;
    }

    obs_source_addref(audio->source);
    audio->state->set_audio_source(audio->source);
    audio->state->set_active(true);

    // Resume from saved position
    audio->state->resume_from_saved_position();
}

static void audio_deactivate(void *data)
{
    blog(LOG_INFO, "[obs-music-playlist] audio_deactivate: data=%p", data);
    auto *audio =
        static_cast<MusicPlaylistAudio *>(data);

    if (!audio || !audio->state) {
        blog(LOG_ERROR, "[obs-music-playlist] audio_deactivate: null check failed audio=%p state=%p", audio, audio ? audio->state : nullptr);
        return;
    }

    // Save position and pause on scene switch
    audio->state->save_position_and_pause();
    audio->state->set_audio_source(nullptr);

    // Don't release source here - OBS manages the source lifecycle
    // The source instance persists across scene switches
    // audio->source is set in audio_create and cleared in audio_destroy
}

static void audio_media_play_pause(
    void *data,
    bool pause)
{
    blog(LOG_DEBUG, "[obs-music-playlist] audio_media_play_pause: pause=%d", pause);
    auto *audio =
        static_cast<MusicPlaylistAudio *>(data);

    if (!audio || !audio->state)
        return;

    audio->state->play_pause(pause);
}

static void audio_media_restart(void *data)
{
    blog(LOG_DEBUG, "[obs-music-playlist] audio_media_restart");
    auto *audio =
        static_cast<MusicPlaylistAudio *>(data);

    if (audio && audio->state)
        audio->state->restart();
}

static void audio_media_stop(void *data)
{
    blog(LOG_DEBUG, "[obs-music-playlist] audio_media_stop");
    auto *audio =
        static_cast<MusicPlaylistAudio *>(data);

    if (audio && audio->state)
        audio->state->stop();
}

static void audio_media_next(void *data)
{
    blog(LOG_DEBUG, "[obs-music-playlist] audio_media_next");
    auto *audio =
        static_cast<MusicPlaylistAudio *>(data);

    if (audio && audio->state)
        audio->state->next();
}

static void audio_media_previous(void *data)
{
    blog(LOG_DEBUG, "[obs-music-playlist] audio_media_previous");
    auto *audio =
        static_cast<MusicPlaylistAudio *>(data);

    if (audio && audio->state)
        audio->state->previous();
}

static int64_t audio_media_get_duration(void *data)
{
    auto *audio =
        static_cast<MusicPlaylistAudio *>(data);

    if (!audio || !audio->state)
        return 0;

    return audio->state->get_duration_ms();
}

static int64_t audio_media_get_time(void *data)
{
    auto *audio =
        static_cast<MusicPlaylistAudio *>(data);

    if (!audio || !audio->state)
        return 0;

    return audio->state->get_time_ms();
}

static void audio_media_set_time(
    void *data,
    int64_t ms)
{
    auto *audio =
        static_cast<MusicPlaylistAudio *>(data);

    if (audio && audio->state)
        audio->state->set_time_ms(ms);
}

static enum obs_media_state audio_media_get_state(
    void *data)
{
    auto *audio =
        static_cast<MusicPlaylistAudio *>(data);

    if (!audio || !audio->state)
        return OBS_MEDIA_STATE_STOPPED;

    PlaylistMediaState state = audio->state->get_media_state();

    enum obs_media_state obs_state;
    switch (state) {
    case PlaylistMediaState::PLAYING:
        obs_state = OBS_MEDIA_STATE_PLAYING;
        break;
    case PlaylistMediaState::PAUSED:
        obs_state = OBS_MEDIA_STATE_PAUSED;
        break;
    case PlaylistMediaState::STOPPED:
        obs_state = OBS_MEDIA_STATE_STOPPED;
        break;
    default:
        obs_state = OBS_MEDIA_STATE_STOPPED;
    }

    blog(LOG_INFO, "[obs-music-playlist] media_get_state: internal=%d -> obs=%d", (int)state, obs_state);
    return obs_state;
}

} // namespace

extern "C" obs_source_info music_playlist_audio_source;

void initialize_music_playlist_audio_source()
{
    music_playlist_audio_source = {};

    music_playlist_audio_source.id =
        AUDIO_SOURCE_ID;

    music_playlist_audio_source.type =
        OBS_SOURCE_TYPE_INPUT;

    music_playlist_audio_source.output_flags =
        OBS_SOURCE_AUDIO |
        OBS_SOURCE_CONTROLLABLE_MEDIA;

    music_playlist_audio_source.get_name =
        [](void *) {
            return "Music Playlist Audio";
        };

    music_playlist_audio_source.create =
        audio_create;

    music_playlist_audio_source.destroy =
        audio_destroy;

    music_playlist_audio_source.update =
        audio_update;

    music_playlist_audio_source.activate =
        audio_activate;

    music_playlist_audio_source.deactivate =
        audio_deactivate;

    music_playlist_audio_source.get_defaults =
        audio_defaults;

    music_playlist_audio_source.get_properties =
        audio_properties;

    /*
     * Media controls.
     */
    music_playlist_audio_source.media_play_pause =
        audio_media_play_pause;

    music_playlist_audio_source.media_restart =
        audio_media_restart;

    music_playlist_audio_source.media_stop =
        audio_media_stop;

    music_playlist_audio_source.media_next =
        audio_media_next;

    music_playlist_audio_source.media_previous =
        audio_media_previous;

    music_playlist_audio_source.media_get_duration =
        audio_media_get_duration;

    music_playlist_audio_source.media_get_time =
        audio_media_get_time;

    music_playlist_audio_source.media_set_time =
        audio_media_set_time;

    music_playlist_audio_source.media_get_state =
        audio_media_get_state;

    music_playlist_audio_source.icon_type =
        OBS_ICON_TYPE_MEDIA;
}
