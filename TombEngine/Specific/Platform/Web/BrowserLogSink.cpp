#include "framework.h"
#include "Specific/Platform/Web/BrowserLogSink.h"

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include <emscripten/threading.h>

namespace TEN::Platform
{
    struct PendingLogMessage
    {
        std::string Text = {};
        std::string Path = {};
    };

    static void WriteLogMessage(PendingLogMessage* message)
    {
        fwrite(message->Text.data(), 1, message->Text.size(), stdout);
        fflush(stdout);

        auto* file = fopen(message->Path.c_str(), "ab");
        if (file != nullptr)
        {
            fwrite(message->Text.data(), 1, message->Text.size(), file);
            fclose(file);
        }

        delete message;
    }

    BrowserLogSink::BrowserLogSink(const std::string& path) :
        _path(path)
    {
    }

    void BrowserLogSink::sink_it_(const spdlog::details::log_msg& message)
    {
        auto formatted = spdlog::memory_buf_t{};
        formatter_->format(message, formatted);

        auto* pending = new PendingLogMessage{ std::string(formatted.data(), formatted.size()), _path };
        emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_VI, (void*)&WriteLogMessage, pending);
    }

    void BrowserLogSink::flush_()
    {
    }
}

#endif
