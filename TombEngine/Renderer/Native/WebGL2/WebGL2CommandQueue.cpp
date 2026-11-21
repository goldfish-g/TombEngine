#include "framework.h"
#include "Renderer/Native/WebGL2/WebGL2CommandQueue.h"

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include <emscripten/threading.h>

namespace TEN::Renderer::Native::WebGL2
{
    CommandList::~CommandList()
    {
        // Destroy what was recorded but never run, so captured data is freed.
        for (auto& chunk : _chunks)
        {
            int offset = 0;
            while (offset < chunk.Used)
            {
                auto* header = (Header*)(chunk.Data.get() + offset);
                header->Destroy(chunk.Data.get() + offset + AlignUp((int)sizeof(Header)));
                offset += header->Size;
            }
        }
    }

    unsigned char* CommandList::Allocate(int size)
    {
        while (_currentChunk < (int)_chunks.size())
        {
            auto& chunk = _chunks[_currentChunk];
            if (chunk.Capacity - chunk.Used >= size)
            {
                auto* memory = chunk.Data.get() + chunk.Used;
                chunk.Used += size;
                return memory;
            }

            _currentChunk++;
        }

        auto chunk = Chunk{};
        chunk.Capacity = std::max(CHUNK_SIZE, size);
        chunk.Data = std::make_unique<unsigned char[]>(chunk.Capacity);
        chunk.Used = size;
        _chunks.push_back(std::move(chunk));
        _currentChunk = (int)_chunks.size() - 1;

        return _chunks.back().Data.get();
    }

    void CommandList::ExecuteAndClear()
    {
        int headerSize = AlignUp((int)sizeof(Header));

        for (auto& chunk : _chunks)
        {
            int offset = 0;
            while (offset < chunk.Used)
            {
                auto* header = (Header*)(chunk.Data.get() + offset);
                auto* command = chunk.Data.get() + offset + headerSize;

                header->Invoke(command);
                header->Destroy(command);
                offset += header->Size;
            }

            chunk.Used = 0;
        }

        _currentChunk = 0;
        _count = 0;
    }

    void CommandQueue::SubmitFrame()
    {
        if (emscripten_is_main_browser_thread())
        {
            ExecuteAllNow();
            _frameCount++;
            return;
        }

        auto lock = std::unique_lock<std::mutex>(_mutex);
        _consumed.wait(lock, [this]() { return (_pending == nullptr && _free != nullptr) || _shutdown; });

        if (_shutdown)
            return;

        _pending = _recording;
        _recording = _free;
        _free = nullptr;
    }

    bool CommandQueue::ExecutePendingFrame()
    {
        CommandList* list = nullptr;
        {
            auto lock = std::lock_guard<std::mutex>(_mutex);
            if (_pending == nullptr)
                return false;

            list = _pending;
            _pending = nullptr;
        }

        // Run outside the lock, so the engine records the next frame meanwhile.
        list->ExecuteAndClear();
        _frameCount++;

        {
            auto lock = std::lock_guard<std::mutex>(_mutex);
            _free = list;
        }

        _consumed.notify_all();
        return true;
    }

    void CommandQueue::ExecuteAllNow()
    {
        ExecutePendingFrame();

        auto lock = std::lock_guard<std::mutex>(_mutex);
        _recording->ExecuteAndClear();
    }

    void CommandQueue::Shutdown()
    {
        {
            auto lock = std::lock_guard<std::mutex>(_mutex);
            _shutdown = true;
        }

        _consumed.notify_all();
    }
}

#endif
