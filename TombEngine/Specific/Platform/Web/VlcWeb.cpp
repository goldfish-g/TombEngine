#include "framework.h"

#ifdef SDL_PLATFORM_EMSCRIPTEN

// Stand-in for libVLC until browser video playback is in place: no media opens,
// so the engine skips videos as it does when one is missing.

extern "C"
{
    libvlc_instance_t* libvlc_new(int argc, const char* const* argv) { return nullptr; }
    void libvlc_release(libvlc_instance_t* instance) {}
    const char* libvlc_errmsg(void) { return "Video playback isn't available in the browser yet."; }
    void libvlc_log_set(libvlc_instance_t* instance, libvlc_log_cb cb, void* data) {}

    libvlc_media_t* libvlc_media_new_path(const char* path) { return nullptr; }
    void libvlc_media_release(libvlc_media_t* media) {}
    libvlc_time_t libvlc_media_get_duration(libvlc_media_t* media) { return -1; }

    libvlc_media_player_t* libvlc_media_player_new_from_media(libvlc_instance_t* instance, libvlc_media_t* media) { return nullptr; }
    void libvlc_media_player_release(libvlc_media_player_t* player) {}
    libvlc_media_t* libvlc_media_player_get_media(libvlc_media_player_t* player) { return nullptr; }
    int libvlc_media_player_play(libvlc_media_player_t* player) { return -1; }
    void libvlc_media_player_pause(libvlc_media_player_t* player) {}
    int libvlc_media_player_stop_async(libvlc_media_player_t* player) { return 0; }
    bool libvlc_media_player_is_playing(libvlc_media_player_t* player) { return false; }
    libvlc_state_t libvlc_media_player_get_state(libvlc_media_player_t* player) { return libvlc_Stopped; }
    double libvlc_media_player_get_position(libvlc_media_player_t* player) { return -1.0; }
    int libvlc_media_player_set_position(libvlc_media_player_t* player, double position, bool fast) { return -1; }

    void libvlc_video_set_callbacks(libvlc_media_player_t* player, libvlc_video_lock_cb lock, libvlc_video_unlock_cb unlock,
                                    libvlc_video_display_cb display, void* opaque) {}
    void libvlc_video_set_format_callbacks(libvlc_media_player_t* player, libvlc_video_format_cb setup, libvlc_video_cleanup_cb cleanup) {}
    void libvlc_audio_set_callbacks(libvlc_media_player_t* player, libvlc_audio_play_cb play, libvlc_audio_pause_cb pause,
                                    libvlc_audio_resume_cb resume, libvlc_audio_flush_cb flush, libvlc_audio_drain_cb drain, void* opaque) {}
    void libvlc_audio_set_format_callbacks(libvlc_media_player_t* player, libvlc_audio_setup_cb setup, libvlc_audio_cleanup_cb cleanup) {}
}

#endif
