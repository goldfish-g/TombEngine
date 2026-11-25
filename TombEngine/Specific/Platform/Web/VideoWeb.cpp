#include "framework.h"
#include "Specific/Video/Video.h"

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include <emscripten.h>

#include "Renderer/Native/WebGL2/WebGL2GraphicsDevice.h"
#include "Renderer/Renderer.h"
#include "Sound/sound.h"
#include "Specific/configuration.h"
#include "Specific/EngineMain.h"
#include "Specific/Input/Input.h"
#include "Specific/Input/TouchControls.h"

using namespace TEN::Input;
using namespace TEN::Renderer::Native::WebGL2;

// The video player in the browser. Videos play in <video> elements of the page (see
// Module.tenWebVideo in Tools/WebGL/pre.js): the browser decodes them and plays their
// sound, and each new frame is copied into the video texture on the GPU, where VLC on
// Windows hands frames over in memory. The player otherwise behaves as Video.cpp's.

namespace TEN::Video
{
    // While an exclusive video shows the same frame, the loops calling Update() rest this long.
    constexpr auto WAIT_FOR_FRAME_MS = 4;

    // Longest side of the video texture: what every WebGL 2 device takes, and more than a
    // full-screen video needs. Larger videos (some are 8K) are scaled down to it.
    constexpr auto TEXTURE_SIZE_MAX = 2048;

    enum WebVideoFlag
    {
        WEB_VIDEO_READY	  = 1 << 0,
        WEB_VIDEO_PLAYING = 1 << 1,
        WEB_VIDEO_PAUSED  = 1 << 2,
        WEB_VIDEO_ENDED	  = 1 << 3,
        WEB_VIDEO_FAILED  = 1 << 4
    };

    // As Module.tenWebVideo.state() writes it.
    struct WebVideoState
    {
        double FrameCount = 0.0;
        double Time		  = 0.0; // Seconds.
        double Duration	  = 0.0; // Seconds, 0 until known.
        double Width	  = 0.0;
        double Height	  = 0.0;
        double Flags	  = WEB_VIDEO_FAILED;

        bool Has(WebVideoFlag flag) const { return ((int)Flags & flag) != 0; }
    };

    static auto LastVideoID			 = 0;
    static auto State				 = WebVideoState{};
    static auto DominantColorVideoID = 0;
    static auto DominantColorFrame	 = -1;
    static auto DominantColor		 = Color(0.0f, 0.0f, 0.0f);

    static WebVideoState ReadState(int videoID)
    {
        auto state = WebVideoState{};
        if (videoID != 0)
        {
            MAIN_THREAD_EM_ASM({ Module['tenWebVideo']['state']($0, $1, $2); },
                videoID, &state, g_Configuration.SfxVolume / 100.0f);
        }

        return state;
    }

    static Vector2i GetTextureSize(int width, int height)
    {
        float scale = std::min(1.0f, TEXTURE_SIZE_MAX / (float)std::max(width, height));
        return Vector2i(std::max((int)std::round(width * scale), 1), std::max((int)std::round(height * scale), 1));
    }

    // The paths Video.cpp tries for a video, in its order.
    static std::vector<std::string> GetVideoPaths(const std::string& filename, const std::string& videoDirectory)
    {
        auto paths = std::vector<std::string>{ filename, videoDirectory + filename };
        for (const auto& extension : VIDEO_EXTENSIONS)
            paths.push_back(videoDirectory + filename + extension);

        return paths;
    }

    int VideoHandler::GetPosition() const
    {
        return (int)(ReadState(_webVideoID).Time * FPS);
    }

    float VideoHandler::GetNormalizedPosition() const
    {
        auto state = ReadState(_webVideoID);
        if (state.Duration <= 0.0)
            return 0.0f;

        return (float)(state.Time / state.Duration);
    }

    std::string VideoHandler::GetFileName() const
    {
        if (_webVideoID == 0)
            return std::string();

        return _fileName;
    }

    // Of the frame shown, worked out by the page once per frame.
    Color VideoHandler::GetDominantColor() const
    {
        if (_webVideoID == 0)
            return Color(0.0f, 0.0f, 0.0f);

        if (DominantColorVideoID != _webVideoID || DominantColorFrame != _webFrameCount)
        {
            unsigned int rgb = (unsigned int)MAIN_THREAD_EM_ASM_INT({ return Module['tenWebVideo']['averageColor']($0); }, _webVideoID);
            auto average = Vector3((float)((rgb >> 16) & 0xFF), (float)((rgb >> 8) & 0xFF), (float)(rgb & 0xFF)) / 255.0f;

            DominantColor = AdjustDominantColor(average);
            DominantColorVideoID = _webVideoID;
            DominantColorFrame = _webFrameCount;
        }

        return DominantColor;
    }

    bool VideoHandler::GetSilent() const
    {
        return _silent;
    }

    bool VideoHandler::GetLooped() const
    {
        return _looped;
    }

    void VideoHandler::SetPosition(int frameCount)
    {
        if (_webVideoID == 0)
            return;

        float seconds = frameCount / (float)FPS;
        auto state = ReadState(_webVideoID);
        if (state.Duration > 0.0 && seconds > state.Duration)
        {
            TENLog("Video position is out of bounds.", LogLevel::Warning);
            return;
        }

        MAIN_THREAD_EM_ASM({ Module['tenWebVideo']['seek']($0, $1); }, _webVideoID, seconds);
    }

    void VideoHandler::SetNormalizedPosition(float pos)
    {
        if (_webVideoID == 0)
            return;

        auto state = ReadState(_webVideoID);
        float seconds = std::clamp(pos, 0.0f, 1.0f) * (float)state.Duration;
        MAIN_THREAD_EM_ASM({ Module['tenWebVideo']['seek']($0, $1); }, _webVideoID, seconds);
    }

    bool VideoHandler::IsPlaying() const
    {
        return ReadState(_webVideoID).Has(WEB_VIDEO_PLAYING);
    }

    bool VideoHandler::IsBackgroundPlaybackQueued() const
    {
        return (_webVideoID != 0 && _playbackMode == VideoPlaybackMode::Exclusive);
    }

    void VideoHandler::Initialize(const std::string& gameDir, IGraphicsDevice* device)
    {
        TENLog("Initializing video player...", LogLevel::Info);

        _device = device;
        _videoDirectory = gameDir + VIDEO_PATH;
        _size = Vector2i::Zero;
        _fileName = {};
        _playbackMode = VideoPlaybackMode::Exclusive;
        _looped = false;
        _silent = false;
    }

    void VideoHandler::DeInitialize()
    {
        TENLog("Shutting down video player...", LogLevel::Info);

        Stop();
        _deInitializing = true;
    }

    bool VideoHandler::Play(const std::string& filename, VideoPlaybackMode mode, bool silent, bool loop)
    {
        auto paths = GetVideoPaths(filename, _videoDirectory);

        // Don't start playback if same video is already playing.
        if (_webVideoID != 0 && _playbackMode == mode && IsPlaying() &&
            std::find(paths.begin(), paths.end(), _fileName) != paths.end())
        {
            TENLog("Video file " + _fileName + " is already playing.", LogLevel::Warning);
            return false;
        }

        // Stop previous player instance if it exists.
        Stop();

        auto joinedPaths = std::string();
        for (const auto& path : paths)
            joinedPaths += (joinedPaths.empty() ? "" : "\n") + path;

        int videoID = ++LastVideoID;
        int found = MAIN_THREAD_EM_ASM_INT(
            { return Module['tenWebVideo']['open']($0, UTF8ToString($1).split('\n'), $2, $3); },
            videoID, joinedPaths.c_str(), loop, silent);

        if (found < 0)
        {
            TENLog("Video file " + _videoDirectory + filename + " not found.", LogLevel::Warning);
            return false;
        }

        _webVideoID = videoID;
        _webFrameCount = 0;
        _looped = loop;
        _silent = silent;
        _playbackMode = mode;
        _fileName = paths[found];
        _needRender = _updateInput = false;

        auto filePath = std::filesystem::path(_fileName);
        TENLog("Playing video file: " + filePath.filename().string() + " (" + (mode == VideoPlaybackMode::Exclusive ? "Exclusive" : "Background") + " mode)", LogLevel::Info);

        if (_playbackMode == VideoPlaybackMode::Exclusive)
            PauseAllSounds(SoundPauseMode::Pause);

        _starting = true;
        return true;
    }

    bool VideoHandler::Pause()
    {
        if (_webVideoID == 0 || ReadState(_webVideoID).Has(WEB_VIDEO_PAUSED))
            return false;

        MAIN_THREAD_EM_ASM({ Module['tenWebVideo']['pause']($0); }, _webVideoID);
        return (_playbackMode == VideoPlaybackMode::Exclusive);
    }

    bool VideoHandler::Resume()
    {
        if (_webVideoID == 0 || !ReadState(_webVideoID).Has(WEB_VIDEO_PAUSED))
            return false;

        MAIN_THREAD_EM_ASM({ Module['tenWebVideo']['play']($0); }, _webVideoID);
        return (_playbackMode == VideoPlaybackMode::Exclusive);
    }

    void VideoHandler::Stop()
    {
        DeinitializePlayer();
        DeinitializeVideoTexture();

        _needRender = false;
    }

    bool VideoHandler::Update()
    {
        if (_deInitializing || _webVideoID == 0)
            return false;

        State = ReadState(_webVideoID);

        if (_videoTexture == nullptr && State.Has(WEB_VIDEO_READY))
        {
            _size = GetTextureSize((int)State.Width, (int)State.Height);
            TENLog("Creating video texture: " + std::to_string(_size.x) + "x" + std::to_string(_size.y), LogLevel::Info);
            _videoTexture = _device->CreateTexture2D(_size.x, _size.y, SurfaceFormat::SF_RGBA8_Unorm, nullptr, true);
        }

        // Draw each new frame once, as VLC's frame callbacks have it drawn.
        if (_videoTexture != nullptr && (int)State.FrameCount != _webFrameCount)
        {
            _webFrameCount = (int)State.FrameCount;
            ((WebGL2GraphicsDevice*)_device)->UpdateTexture2DFromVideo(_videoTexture.get(), _webVideoID);

            if (_playbackMode == VideoPlaybackMode::Exclusive)
            {
                _updateInput = true;
                RenderExclusive();
            }
            else
            {
                RenderBackground();
            }
        }
        else if (_playbackMode == VideoPlaybackMode::Exclusive)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(WAIT_FOR_FRAME_MS));
        }

        if (_playbackMode == VideoPlaybackMode::Exclusive)
            UpdateExclusive();
        else
            UpdateBackground();

        return (_playbackMode == VideoPlaybackMode::Exclusive);
    }

    void VideoHandler::UpdateExclusive()
    {
        if (_deInitializing || _webVideoID == 0)
            return;

        bool interruptPlayback = false;
        if (_updateInput)
        {
            ResetClock = true;
            UpdateInputActions(true);
            interruptPlayback = IsHeld(In::Deselect) || IsHeld(In::Look) || IsAnyFingerDown();
            _updateInput = _starting = false;
        }

        HandleError();

        // Skipped, played to its end, or unplayable in this browser.
        bool isOver = State.Has(WEB_VIDEO_FAILED) || (!_looped && State.Has(WEB_VIDEO_ENDED));
        if (interruptPlayback || isOver)
        {
            Stop();
            ClearAction(In::Pause); // HACK: Otherwise pause key won't work after video ends.
            ResumeAllSounds(SoundPauseMode::Pause);
        }
    }

    void VideoHandler::RenderExclusive()
    {
        g_Renderer.RenderFullScreenTexture(_videoTexture.get(), (float)_size.x / (float)_size.y);
    }

    void VideoHandler::UpdateBackground()
    {
        if (_deInitializing || _webVideoID == 0)
            return;

        HandleError();

        if (State.Has(WEB_VIDEO_FAILED) || (!_looped && State.Has(WEB_VIDEO_ENDED)))
            Stop();
    }

    void VideoHandler::RenderBackground()
    {
        g_Renderer.UpdateVideoTexture(_videoTexture.get());
    }

    bool VideoHandler::HandleError()
    {
        if (!State.Has(WEB_VIDEO_FAILED))
            return true;

        TENLog("Video " + _fileName + " can't play in this browser.", LogLevel::Warning);
        return false;
    }

    bool VideoHandler::InitializeVideoTexture()
    {
        return true;
    }

    void VideoHandler::DeinitializeVideoTexture()
    {
        g_Renderer.ClearVideoTexture();

        _videoTexture.reset();
        _frameBuffer.clear();
        _size = Vector2i::Zero;
    }

    void VideoHandler::DeinitializePlayer()
    {
        if (_webVideoID == 0)
            return;

        MAIN_THREAD_EM_ASM({ Module['tenWebVideo']['close']($0); }, _webVideoID);

        _webVideoID = 0;
        _webFrameCount = 0;
        _fileName = {};
        State = WebVideoState{};
    }
}

#endif
