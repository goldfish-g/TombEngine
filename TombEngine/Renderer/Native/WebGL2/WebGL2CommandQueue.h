#pragma once

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <vector>

namespace TEN::Renderer::Native::WebGL2
{
    // A list of recorded commands. Commands are callables stored back to back in
    // chunks that are kept between frames, so recording doesn't allocate once the
    // chunks have grown to the size of a frame.
    class CommandList
    {
    private:
        static constexpr int CHUNK_SIZE = 1 << 20;
        static constexpr int ALIGNMENT  = 16;

        struct Header
        {
            void (*Invoke)(void*);
            void (*Destroy)(void*);
            int Size;
        };

        struct Chunk
        {
            std::unique_ptr<unsigned char[]> Data = nullptr;
            int Capacity = 0;
            int Used	 = 0;
        };

        std::vector<Chunk> _chunks = {};
        int _currentChunk = 0;
        int _count		  = 0;

        static constexpr int AlignUp(int value)
        {
            return (value + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        }

        template <typename T>
        static void InvokeCommand(void* command)
        {
            (*(T*)command)();
        }

        template <typename T>
        static void DestroyCommand(void* command)
        {
            ((T*)command)->~T();
        }

        unsigned char* Allocate(int size);

    public:
        CommandList() = default;
        CommandList(const CommandList&) = delete;
        CommandList& operator=(const CommandList&) = delete;
        ~CommandList();

        template <typename Function>
        void Add(Function&& function)
        {
            using Command = std::decay_t<Function>;

            int headerSize = AlignUp((int)sizeof(Header));
            int size = headerSize + AlignUp((int)sizeof(Command));
            auto* memory = Allocate(size);

            new (memory) Header{ &InvokeCommand<Command>, &DestroyCommand<Command>, size };
            new (memory + headerSize) Command(std::forward<Function>(function));
            _count++;
        }

        bool IsEmpty() const { return _count == 0; }

        // Runs every command in recording order, then empties the list.
        void ExecuteAndClear();
    };

    // Commands recorded by any engine thread and executed on the browser's main
    // thread, which owns the WebGL context. Recording is serialized, which the
    // engine already guarantees as Direct3D 11's immediate context needs it too.
    // A frame is handed over when it is presented; the recording thread then
    // waits while the main thread still runs the previous one.
    class CommandQueue
    {
    private:
        std::mutex				_mutex		= {};
        std::condition_variable _consumed	= {};
        CommandList				_lists[2]	= {};
        CommandList*			_recording	= &_lists[0];
        CommandList*			_pending	= nullptr;
        CommandList*			_free		= &_lists[1];
        bool					_shutdown	= false;
        unsigned int			_frameCount = 0;

    public:
        template <typename Function>
        void Record(Function&& function)
        {
            auto lock = std::lock_guard<std::mutex>(_mutex);
            _recording->Add(std::forward<Function>(function));
        }

        // Hands the recorded commands over as a frame. Called from the main
        // thread, it runs them right away instead, as there is nobody to wait for.
        void SubmitFrame();

        // Main thread: runs the frame handed over, if any. Returns whether it did.
        bool ExecutePendingFrame();

        // Main thread: runs everything recorded so far, frame or not.
        void ExecuteAllNow();

        // Stops any thread waiting in SubmitFrame (when the page closes the game).
        void Shutdown();

        unsigned int GetFrameCount() const { return _frameCount; }
    };
}

#endif
