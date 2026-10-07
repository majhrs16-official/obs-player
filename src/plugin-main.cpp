#include <obs-module.h>

OBS_DECLARE_MODULE()

extern "C" {

obs_source_info music_playlist_audio_source = {};
obs_source_info music_playlist_text_source = {};

}

void initialize_music_playlist_audio_source();
void initialize_music_playlist_text_source();

bool obs_module_load(void)
{
    initialize_music_playlist_audio_source();
    initialize_music_playlist_text_source();

    blog(LOG_INFO, "[obs-music-playlist] Registering audio source: id=%s, name=%s",
         music_playlist_audio_source.id,
         music_playlist_audio_source.get_name(nullptr));
    
    obs_register_source(
        &music_playlist_audio_source);

    blog(LOG_INFO, "[obs-music-playlist] Registering text source: id=%s, name=%s",
         music_playlist_text_source.id,
         music_playlist_text_source.get_name(nullptr));
    
    obs_register_source(
        &music_playlist_text_source);

    blog(
        LOG_INFO,
        "[obs-music-playlist] Plugin loaded");

    return true;
}

void obs_module_unload(void)
{
    blog(
        LOG_INFO,
        "[obs-music-playlist] Plugin unloaded");
}
