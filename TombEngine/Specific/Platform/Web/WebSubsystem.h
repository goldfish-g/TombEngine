#pragma once

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include "Specific/Platform/ISubsystem.h"

namespace TEN::Platform
{
    // The platform subsystem of the browser build. The page puts the engine's own
    // files in /engine and the game's files where the gamedir argument says.
    class WebSubsystem final : public ISubsystem
    {
    private:
        SDL_Window* _window = nullptr;

    public:
        SDL_Window*                 GetSDL3Window() override;
        std::string                 GetBinaryPath(bool includeExeName) override;
        std::vector<unsigned short> GetProductOrFileVersion(bool productVersion) override;

        void SetSDL3Window(SDL_Window* window) override;

        void Initialize() override;
        void Tick() override;
        void Shutdown() override;

        void InstallCrashHandler() override;
        bool CreateDummyTitleLevel(const std::string& levelPath) override;
        void CheckPrerequisites() override;
        void ConfigureConsole() override;
        void HideConsole() override;
        void ShowErrorMessage(const std::string& msg) override;

        void InitialiseAudioCodecs() override;
        void ReleaseAudioCodecs() override;
    };
}

#endif
