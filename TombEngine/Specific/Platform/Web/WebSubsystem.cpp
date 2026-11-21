#include "framework.h"
#include "Specific/Platform/Web/WebSubsystem.h"

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include <emscripten.h>

#include "version.h"

namespace TEN::Platform
{
    constexpr auto ENGINE_DIRECTORY = "/engine/";

    SDL_Window* WebSubsystem::GetSDL3Window()
    {
        return _window;
    }

    void WebSubsystem::SetSDL3Window(SDL_Window* window)
    {
        _window = window;
    }

    std::string WebSubsystem::GetBinaryPath(bool includeExeName)
    {
        return includeExeName ? std::string(ENGINE_DIRECTORY) + "TombEngine" : std::string(ENGINE_DIRECTORY);
    }

    std::vector<unsigned short> WebSubsystem::GetProductOrFileVersion(bool productVersion)
    {
        // Windows reads these from the executable's version resource.
        return { TEN_MAJOR_VERSION, TEN_MINOR_VERSION, TEN_BUILD_NUMBER, TEN_REVISION_NUMBER };
    }

    void WebSubsystem::Initialize()
    {
    }

    void WebSubsystem::Tick()
    {
    }

    void WebSubsystem::Shutdown()
    {
    }

    void WebSubsystem::InstallCrashHandler()
    {
        // The page sees crashes as errors of the WebAssembly module.
    }

    bool WebSubsystem::CreateDummyTitleLevel(const std::string& levelPath)
    {
        // Windows keeps this level in the executable (Resources/title.bin); the page puts it in /engine.
        auto sourcePath = std::filesystem::path(ENGINE_DIRECTORY) / "title.bin";
        if (!std::filesystem::is_regular_file(sourcePath))
        {
            TENLog("Embedded title level file not found.", LogLevel::Error);
            return false;
        }

        try
        {
            auto directory = std::filesystem::path(levelPath).parent_path();
            if (!directory.empty())
                std::filesystem::create_directories(directory);

            std::filesystem::copy_file(sourcePath, levelPath, std::filesystem::copy_options::overwrite_existing);
        }
        catch (const std::exception& ex)
        {
            TENLog("Error while generating title level file: " + std::string(ex.what()), LogLevel::Error);
            return false;
        }

        return true;
    }

    void WebSubsystem::CheckPrerequisites()
    {
    }

    void WebSubsystem::ConfigureConsole()
    {
    }

    void WebSubsystem::HideConsole()
    {
    }

    void WebSubsystem::ShowErrorMessage(const std::string& msg)
    {
        // The page shows it: Module.onEngineError, or the console without one.
        MAIN_THREAD_EM_ASM(
        {
            var message = UTF8ToString($0);
            if (Module.onEngineError)
                Module.onEngineError(message);
            else
                console.error(message);
        }, msg.c_str());
    }

    void WebSubsystem::InitialiseAudioCodecs()
    {
    }

    void WebSubsystem::ReleaseAudioCodecs()
    {
    }

    std::unique_ptr<ISubsystem> CreatePlatformSubsystem()
    {
        return std::make_unique<WebSubsystem>();
    }
}

#endif
