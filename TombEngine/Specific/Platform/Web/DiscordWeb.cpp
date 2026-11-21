#include "framework.h"

#ifdef SDL_PLATFORM_EMSCRIPTEN

// Discord's Rich Presence talks to the Discord desktop application, which a web
// page can't reach: these do nothing.

extern "C"
{
    void Discord_Initialize(const char* applicationId, DiscordEventHandlers* handlers, int autoRegister, const char* optionalSteamId) {}
    void Discord_Shutdown(void) {}
    void Discord_UpdatePresence(const DiscordRichPresence* presence) {}
    void Discord_ClearPresence(void) {}
}

#endif
