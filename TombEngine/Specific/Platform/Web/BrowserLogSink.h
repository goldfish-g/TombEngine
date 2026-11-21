#pragma once

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include <spdlog/sinks/base_sink.h>

namespace TEN::Platform
{
    // The log of the browser build. In the browser, a thread's file and console
    // writes are done by the main thread while the thread waits; doing them while
    // holding the log's lock would deadlock with the main thread logging too. So
    // messages are handed to the main thread without waiting, and it prints them
    // and appends them to the log file.
    class BrowserLogSink : public spdlog::sinks::base_sink<std::mutex>
    {
    private:
        std::string _path = {};

    protected:
        void sink_it_(const spdlog::details::log_msg& message) override;
        void flush_() override;

    public:
        BrowserLogSink(const std::string& path);
    };
}

#endif
