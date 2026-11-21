#include "framework.h"

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include <cmath>
#include <cstring>
#include <mutex>

#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>

#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include <stb_vorbis.c>

// stb_vorbis leaves these short macros defined.
#undef C
#undef CHECK
#undef L
#undef R

// Browser implementation of the part of BASS 2.4 and BASS_FX 2.4 the engine uses (see Sound/sound.cpp).
// Channels are mixed in software on the callback of an SDL3 audio stream, which SDL plays with Web Audio.
// 3D channels mix into a bus that carries the FX of the 3D mix stream (STREAMPROC_DEVICE_3D) before it
// reaches the output. The API and the mixer share one mutex; syncs are called and resources are freed
// only after it has been released, so callbacks can call BASS again.

namespace TEN::Platform::BassWeb
{
    constexpr auto NO_SOUND_DEVICE         = 0;
    constexpr auto OUTPUT_DEVICE           = 1;
    constexpr auto DEVICE_COUNT            = 2;
    constexpr auto DEFAULT_OUTPUT_RATE     = 44100;
    constexpr auto MIN_OUTPUT_RATE         = 8000;
    constexpr auto MAX_OUTPUT_RATE         = 192000;
    constexpr auto OUTPUT_CHANNELS         = 2;
    constexpr auto OUTPUT_FRAME_BYTES      = OUTPUT_CHANNELS * (int)sizeof(float);
    constexpr auto OUTPUT_DEVICE_FRAMES    = "2048";
    constexpr auto MIX_CHUNK_FRAMES        = 256;
    constexpr auto NANOSECONDS_PER_SECOND  = 1000000000LL;
    constexpr auto OUTPUT_STALL_TIME       = 250000000LL;   // In ns.
    constexpr auto MAX_STALL_CATCH_UP_TIME = 1000000000LL;  // In ns.

    constexpr auto MAX_SAMPLE_SLOTS        = 16384;
    constexpr auto MAX_CHANNEL_SLOTS       = 256;
    constexpr auto MAX_SYNC_SLOTS          = 256;
    constexpr auto MAX_EFFECT_SLOTS        = 32;
    constexpr auto MAX_CHANNEL_EFFECTS     = 4;
    constexpr auto MAX_PENDING_SYNC_CALLS  = 256;
    constexpr auto LEVEL_BLOCK_COUNT       = 192; // Blocks of MIX_CHUNK_FRAMES, a little over a second.
    constexpr auto MAX_LEVEL_SECONDS       = 1.0f;

    constexpr auto DECODE_CHUNK_FRAMES     = 4096;
    constexpr auto STREAM_BUFFER_FRAMES    = 2048;
    constexpr auto PUSH_QUEUE_SECONDS      = 4;
    constexpr auto MAX_PUSH_RATE           = 384000;
    constexpr auto MAX_MP3_SEEK_POINTS     = 4096;

    constexpr auto MIN_PLAYBACK_RATE       = 100.0f;
    constexpr auto MAX_PLAYBACK_STEP       = 16.0;
    constexpr auto LOG_SLIDE_FLOOR         = 0.001f;
    constexpr auto MAX_3D_FACTOR           = 10.0f;
    constexpr auto SPEED_OF_SOUND          = 343.0f; // Meters per second.
    constexpr auto MIN_DOPPLER_SHIFT       = 0.9f;  // Real motion shifts a few percent; camera cuts give TEN huge velocities.
    constexpr auto MAX_DOPPLER_SHIFT       = 1.1f;
    constexpr auto PAN_DEPTH_3D            = 0.8f;
    constexpr auto MIN_DIRECTION_DISTANCE  = 0.001f;
    constexpr auto MIN_ORIENTATION_LENGTH  = 0.000001f;
    constexpr auto DEFAULT_MIN_DISTANCE    = 1.0f;
    constexpr auto DEFAULT_MAX_DISTANCE    = 1000000000.0f;
    constexpr auto FULL_CONE_ANGLE         = 360;
    constexpr auto ANTI_DENORMAL           = 1.0e-18f;

    constexpr auto FX_VERSION              = 0x02040C00; // BASS_FX 2.4.12, the version bass_fx.h is written for.
    constexpr auto STORED_SAMPLE_FLAGS     = BASS_SAMPLE_8BITS | BASS_SAMPLE_FLOAT | BASS_SAMPLE_LOOP | BASS_SAMPLE_3D | BASS_SAMPLE_MUTEMAX | BASS_SAMPLE_OVER_DIST;
    constexpr auto STORED_STREAM_FLAGS     = BASS_SAMPLE_8BITS | BASS_SAMPLE_FLOAT | BASS_SAMPLE_LOOP | BASS_SAMPLE_3D | BASS_SAMPLE_MUTEMAX | BASS_STREAM_AUTOFREE;
    constexpr auto OVERRIDE_FLAGS          = BASS_SAMPLE_OVER_DIST; // OVER_VOL, OVER_POS and OVER_DIST share these bits.
    constexpr auto SYNC_FLAGS              = BASS_SYNC_MIXTIME | BASS_SYNC_ONETIME | BASS_SYNC_THREAD;
    constexpr auto POSITION_MODE_MASK      = 0xFF;

    constexpr auto HANDLE_KIND_SHIFT       = 28;
    constexpr auto HANDLE_GENERATION_SHIFT = 14;
    constexpr auto HANDLE_FIELD_MASK       = 0x3FFF;

    // Jezar's Freeverb, tuned for 44.1 kHz.
    constexpr auto FREEVERB_COMB_COUNT       = 8;
    constexpr auto FREEVERB_ALLPASS_COUNT    = 4;
    constexpr auto FREEVERB_STEREO_SPREAD    = 23;
    constexpr auto FREEVERB_TUNING_RATE      = 44100.0f;
    constexpr auto FREEVERB_FIXED_GAIN       = 0.015f;
    constexpr auto FREEVERB_SCALE_DAMP       = 0.4f;
    constexpr auto FREEVERB_SCALE_ROOM       = 0.28f;
    constexpr auto FREEVERB_OFFSET_ROOM      = 0.7f;
    constexpr auto FREEVERB_ALLPASS_FEEDBACK = 0.5f;
    constexpr auto FREEVERB_MAX_WET          = 3.0f;
    constexpr int  FREEVERB_COMB_TUNING[FREEVERB_COMB_COUNT]       = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
    constexpr int  FREEVERB_ALLPASS_TUNING[FREEVERB_ALLPASS_COUNT] = { 556, 441, 341, 225 };

    constexpr auto COMPRESSOR_MIN_REDUCTION = 0.0001f; // In dB.

    enum HandleKind
    {
        HANDLE_KIND_SAMPLE = 1,
        HANDLE_KIND_CHANNEL,
        HANDLE_KIND_SYNC,
        HANDLE_KIND_EFFECT
    };

    enum SlideAttribute
    {
        SLIDE_VOLUME,
        SLIDE_PAN,
        SLIDE_FREQUENCY,
        SLIDE_COUNT
    };

    enum class SlotState
    {
        Free,
        Live,
        Releasing
    };

    enum class ChannelType
    {
        Sample,
        File,
        Push,
        Device3D
    };

    enum class PlaybackState
    {
        Stopped,
        Playing,
        Paused
    };

    enum class DecoderFormat
    {
        Wav,
        Vorbis,
        Mp3
    };

    // dr_wav and dr_mp3 keep pointers to their own state, so a decoder never moves once open.
    struct AudioDecoder
    {
        DecoderFormat Format      = DecoderFormat::Wav;
        bool          IsOpen      = false;
        drwav         Wav         = {};
        drmp3         Mp3         = {};
        stb_vorbis*   Vorbis      = nullptr;
        int           Channels    = 0;
        int           Frequency   = 0;
        long long     TotalFrames = 0;
        DWORD         CType       = 0;
        DWORD         OrigRes     = 0;

        std::vector<drmp3_seek_point> SeekPoints = {};

        AudioDecoder() = default;
        AudioDecoder(const AudioDecoder&) = delete;
        AudioDecoder& operator=(const AudioDecoder&) = delete;
        ~AudioDecoder();
    };

    // File and push stream data, owned by the channel and freed outside the mixer lock.
    struct StreamData
    {
        std::vector<unsigned char> FileData = {};
        std::string                FileName = {};
        AudioDecoder               Decoder;

        std::vector<float> Buffer = {}; // Decoded frames waiting to be resampled.
        std::vector<float> Queue  = {}; // Ring of pushed frames.

        int  QueueCapacity = 0;
        int  QueueStart    = 0;
        int  QueueFrames   = 0;
        bool IsQueueEnded  = false;
    };

    struct CombFilter
    {
        std::vector<float> Buffer = {};
        int   Size  = 1;
        int   Index = 0;
        float Store = 0.0f;
    };

    struct AllpassFilter
    {
        std::vector<float> Buffer = {};
        int Size  = 1;
        int Index = 0;
    };

    struct FreeverbState
    {
        CombFilter    Combs[OUTPUT_CHANNELS][FREEVERB_COMB_COUNT]        = {};
        AllpassFilter Allpasses[OUTPUT_CHANNELS][FREEVERB_ALLPASS_COUNT] = {};

        bool  IsEnabled = true;
        float InputGain = FREEVERB_FIXED_GAIN;
        float Feedback  = 0.0f;
        float Damp1     = 0.0f;
        float Damp2     = 1.0f;
        float Wet1      = 0.0f;
        float Wet2      = 0.0f;
        float Dry       = 0.0f;
    };

    struct CompressorState
    {
        bool  IsEnabled   = true;
        float OutputGain  = 1.0f;
        float Threshold   = 1.0f;
        float ThresholdDb = 0.0f;
        float Slope       = 0.0f;
        float Attack      = 0.0f;
        float Release     = 0.0f;
        float Envelope    = 0.0f;
    };

    struct EffectProcessor
    {
        DWORD           Type       = 0;
        FreeverbState   Reverb     = {};
        CompressorState Compressor = {};
    };

    struct EffectSlot
    {
        SlotState        State        = SlotState::Free;
        int              Generation   = 0;
        int              ChannelIndex = -1;
        int              Priority     = 0;
        EffectProcessor* Processor    = nullptr;
    };

    struct SlideState
    {
        bool  IsActive      = false;
        bool  IsLogarithmic = false;
        bool  StopsChannel  = false;
        float From          = 0.0f;
        float To            = 0.0f;
        int   Elapsed       = 0;
        int   Duration      = 1;
    };

    struct LevelBlock
    {
        float SumLeft   = 0.0f;
        float SumRight  = 0.0f;
        float PeakLeft  = 0.0f;
        float PeakRight = 0.0f;
        int   Frames    = 0;
    };

    struct SampleSlot
    {
        SlotState State      = SlotState::Free;
        int       Generation = 0;
        int       Device     = OUTPUT_DEVICE;

        float* Data      = nullptr; // Interleaved float frames.
        int    Frames    = 0;
        int    Channels  = 1;
        int    Frequency = DEFAULT_OUTPUT_RATE;
        DWORD  Flags     = 0;
        DWORD  Max       = 1;
        DWORD  OrigRes   = 0;
        float  Volume    = 1.0f;
        float  Pan       = 0.0f;
        DWORD  MinGap    = 0;

        DWORD Mode3D        = BASS_3DMODE_NORMAL;
        float MinDistance   = DEFAULT_MIN_DISTANCE;
        float MaxDistance   = DEFAULT_MAX_DISTANCE;
        DWORD InsideAngle   = FULL_CONE_ANGLE;
        DWORD OutsideAngle  = FULL_CONE_ANGLE;
        float OutsideVolume = 1.0f;
    };

    struct ChannelSlot
    {
        SlotState     State          = SlotState::Free;
        int           Generation     = 0;
        ChannelType   Type           = ChannelType::Sample;
        PlaybackState Playback       = PlaybackState::Stopped;
        int           Device         = OUTPUT_DEVICE;
        DWORD         Flags          = 0;
        int           Frequency      = DEFAULT_OUTPUT_RATE;
        int           Channels       = 1;
        DWORD         CType          = 0;
        DWORD         OrigRes        = 0;
        int           SampleIndex    = -1;
        bool          IsSampleStream = false;
        int           LockCount      = 0;
        bool          IsSeeking      = false;
        bool          HasEnded       = false;
        long long     StartOrder     = 0;

        // Samples: frame position in the sample. Streams: frame position in the stream buffer.
        double      Position      = 0.0;
        long long   DecodedFrames = 0;
        int         BufferFrames  = 0;
        StreamData* Stream        = nullptr;

        float      Volume              = 1.0f;
        float      Pan                 = 0.0f;
        float      Rate                = 0.0f; // 0 = original rate.
        SlideState Slides[SLIDE_COUNT] = {};
        float      GainLeft            = 0.0f;
        float      GainRight           = 0.0f;
        bool       HasGain             = false;

        int     Mode3D        = BASS_3DMODE_NORMAL;
        float   MinDistance   = DEFAULT_MIN_DISTANCE;
        float   MaxDistance   = DEFAULT_MAX_DISTANCE;
        int     InsideAngle   = FULL_CONE_ANGLE;
        int     OutsideAngle  = FULL_CONE_ANGLE;
        float   OutsideVolume = 1.0f;
        Vector3 Position3D    = Vector3::Zero;
        Vector3 Orientation3D = Vector3::Zero;
        Vector3 Velocity3D    = Vector3::Zero;

        int EffectCount                  = 0;
        int Effects[MAX_CHANNEL_EFFECTS] = {};

        LevelBlock Levels[LEVEL_BLOCK_COUNT] = {};
        int        LevelNext                 = 0;
        int        LevelCount                = 0;
    };

    struct SyncSlot
    {
        SlotState State        = SlotState::Free;
        int       Generation   = 0;
        int       ChannelIndex = -1;
        DWORD     Type         = 0;
        bool      IsOneTime    = false;
        SYNCPROC* Proc         = nullptr;
        void*     User         = nullptr;
    };

    struct SyncCall
    {
        SYNCPROC* Proc    = nullptr;
        HSYNC     Handle  = 0;
        DWORD     Channel = 0;
        DWORD     Data    = 0;
        void*     User    = nullptr;
    };

    struct ListenerState
    {
        Vector3 Position       = Vector3::Zero;
        Vector3 Velocity       = Vector3::Zero;
        Vector3 Front          = Vector3(0.0f, 0.0f, 1.0f);
        Vector3 Top            = Vector3(0.0f, 1.0f, 0.0f);
        Vector3 Right          = Vector3(1.0f, 0.0f, 0.0f);
        float   DistanceFactor = 1.0f;
        float   RolloffFactor  = 1.0f;
        float   DopplerFactor  = 1.0f;
    };

    struct ReleaseBatch
    {
        std::vector<StreamData*>      Streams    = {};
        std::vector<EffectProcessor*> Processors = {};
        std::vector<float*>           SampleData = {};
    };

    struct DecodedAudio
    {
        std::vector<float> Data      = {};
        int                Frames    = 0;
        int                Channels  = 0;
        int                Frequency = 0;
        DWORD              OrigRes   = 0;
    };

    static std::mutex MixerMutex;
    static thread_local int LastError    = BASS_OK;
    static thread_local int ThreadDevice = -1;

    static bool             DeviceInitialized[DEVICE_COUNT] = {};
    static bool             DevicePaused[DEVICE_COUNT]      = {};
    static int              Device3DStreams[DEVICE_COUNT]   = { -1, -1 };
    static int              LastInitializedDevice           = -1;
    static int              OutputRate                      = DEFAULT_OUTPUT_RATE;
    static SDL_AudioStream* OutputStream                    = nullptr;
    static long long        LastCallbackTime                = 0; // SDL_GetTicksNS() of the last audio callback.
    static long long        MixedUntilTime                  = 0; // Time the mix has advanced to.

    static SampleSlot    SampleSlots[MAX_SAMPLE_SLOTS];
    static ChannelSlot   ChannelSlots[MAX_CHANNEL_SLOTS];
    static SyncSlot      SyncSlots[MAX_SYNC_SLOTS];
    static EffectSlot    EffectSlots[MAX_EFFECT_SLOTS];
    static int           NextSampleSlot        = 0;
    static int           NextChannelSlot       = 0;
    static int           ReleasingChannelCount = 0;
    static long long     ChannelCounter        = 0;
    static ListenerState Listener              = {};

    static SyncCall PendingSyncCalls[MAX_PENDING_SYNC_CALLS];
    static int      PendingSyncCallCount = 0;

    // Mixer buffers. MixBuffer belongs to the audio callback, StallBuffer to API calls that advance a
    // stalled mix; the others are used under the lock only.
    static float MixBuffer[MIX_CHUNK_FRAMES * OUTPUT_CHANNELS];
    static float StallBuffer[MIX_CHUNK_FRAMES * OUTPUT_CHANNELS];
    static float BusBuffer[MIX_CHUNK_FRAMES * OUTPUT_CHANNELS];
    static float ChannelBuffer[MIX_CHUNK_FRAMES * OUTPUT_CHANNELS];

    static void CatchUpStalledOutput();

    // ===============
    // Errors, handles
    // ===============

    static BOOL Fail(int error)
    {
        LastError = error;
        return FALSE;
    }

    static DWORD FailHandle(int error)
    {
        LastError = error;
        return 0;
    }

    static QWORD FailPosition(int error)
    {
        LastError = error;
        return (QWORD)-1;
    }

    static BOOL Succeed()
    {
        LastError = BASS_OK;
        return TRUE;
    }

    static DWORD MakeHandle(int kind, int generation, int index)
    {
        return ((DWORD)kind << HANDLE_KIND_SHIFT) |
               ((DWORD)(generation & HANDLE_FIELD_MASK) << HANDLE_GENERATION_SHIFT) |
               (DWORD)index;
    }

    template <typename TSlot>
    static int GetLiveSlotIndex(DWORD handle, int kind, const TSlot* slots, int count)
    {
        if ((int)(handle >> HANDLE_KIND_SHIFT) != kind)
            return -1;

        int index = (int)(handle & HANDLE_FIELD_MASK);
        if (index >= count || slots[index].State != SlotState::Live)
            return -1;

        int generation = (int)((handle >> HANDLE_GENERATION_SHIFT) & HANDLE_FIELD_MASK);
        return (generation == (slots[index].Generation & HANDLE_FIELD_MASK)) ? index : -1;
    }

    static int GetSampleIndex(DWORD handle)
    {
        return GetLiveSlotIndex(handle, HANDLE_KIND_SAMPLE, SampleSlots, MAX_SAMPLE_SLOTS);
    }

    static int GetChannelIndex(DWORD handle)
    {
        return GetLiveSlotIndex(handle, HANDLE_KIND_CHANNEL, ChannelSlots, MAX_CHANNEL_SLOTS);
    }

    static int GetEffectIndex(DWORD handle)
    {
        return GetLiveSlotIndex(handle, HANDLE_KIND_EFFECT, EffectSlots, MAX_EFFECT_SLOTS);
    }

    static DWORD GetChannelHandle(int index)
    {
        return MakeHandle(HANDLE_KIND_CHANNEL, ChannelSlots[index].Generation, index);
    }

    static DWORD GetSampleHandle(int index)
    {
        return MakeHandle(HANDLE_KIND_SAMPLE, SampleSlots[index].Generation, index);
    }

    static int GetBytesPerSample(DWORD flags)
    {
        if (flags & BASS_SAMPLE_FLOAT)
            return (int)sizeof(float);

        return (flags & BASS_SAMPLE_8BITS) ? 1 : 2;
    }

    static int GetBytesPerFrame(const ChannelSlot& channel)
    {
        return channel.Channels * GetBytesPerSample(channel.Flags);
    }

    // BASS_Init and BASS_ChannelSetDevice take -1 for the default device.
    static int ResolveDevice(int device)
    {
        return (device == -1) ? OUTPUT_DEVICE : device;
    }

    // The device of the calling thread: the one it set or initialized, else the last one initialized.
    static int GetCurrentDevice()
    {
        if (ThreadDevice >= 0 && DeviceInitialized[ThreadDevice])
            return ThreadDevice;

        if (LastInitializedDevice >= 0 && DeviceInitialized[LastInitializedDevice])
            return LastInitializedDevice;

        for (int device = DEVICE_COUNT - 1; device >= 0; device--)
        {
            if (DeviceInitialized[device])
                return device;
        }

        return -1;
    }

    static bool IsFinite(const BASS_3DVECTOR& vector)
    {
        return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
    }

    static Vector3 ToVector3(const BASS_3DVECTOR& vector)
    {
        return Vector3(vector.x, vector.y, vector.z);
    }

    // ===========================
    // Syncs and resource release
    // ===========================

    static void FreeSyncSlot(int index)
    {
        SyncSlots[index].State = SlotState::Free;
        SyncSlots[index].Generation++;
    }

    // Queues calls of the channel's syncs of a type; they run once the mixer lock is released.
    static void QueueSyncs(int channelIndex, DWORD type, DWORD data)
    {
        for (int i = 0; i < MAX_SYNC_SLOTS; i++)
        {
            const auto& sync = SyncSlots[i];
            if (sync.State != SlotState::Live || sync.ChannelIndex != channelIndex || sync.Type != type)
                continue;

            if (PendingSyncCallCount < MAX_PENDING_SYNC_CALLS)
            {
                auto& call = PendingSyncCalls[PendingSyncCallCount++];
                call.Proc = sync.Proc;
                call.Handle = MakeHandle(HANDLE_KIND_SYNC, sync.Generation, i);
                call.Channel = GetChannelHandle(channelIndex);
                call.Data = data;
                call.User = sync.User;
            }

            if (sync.IsOneTime)
                FreeSyncSlot(i);
        }
    }

    static int TakePendingSyncCalls(SyncCall* calls)
    {
        int count = PendingSyncCallCount;
        for (int i = 0; i < count; i++)
            calls[i] = PendingSyncCalls[i];

        PendingSyncCallCount = 0;
        return count;
    }

    // Calls syncs without the mixer lock. The caller's error code survives BASS calls made by them.
    static void RunSyncCalls(const SyncCall* calls, int count)
    {
        int error = LastError;
        for (int i = 0; i < count; i++)
            calls[i].Proc(calls[i].Handle, calls[i].Channel, calls[i].Data, calls[i].User);

        LastError = error;
    }

    // Invalidates a channel's handle at once. Its stream data and FX are freed later, outside the lock.
    static void ReleaseChannel(int index)
    {
        auto& channel = ChannelSlots[index];
        QueueSyncs(index, BASS_SYNC_FREE, 0);

        for (int i = 0; i < MAX_SYNC_SLOTS; i++)
        {
            if (SyncSlots[i].State == SlotState::Live && SyncSlots[i].ChannelIndex == index)
                FreeSyncSlot(i);
        }

        for (int i = 0; i < channel.EffectCount; i++)
        {
            auto& effect = EffectSlots[channel.Effects[i]];
            effect.State = SlotState::Releasing;
            effect.Generation++;
        }

        if (channel.Type == ChannelType::Device3D && Device3DStreams[channel.Device] == index)
            Device3DStreams[channel.Device] = -1;

        channel.State = SlotState::Releasing;
        channel.Generation++;
        channel.Playback = PlaybackState::Stopped;
        ReleasingChannelCount++;
    }

    // Takes the resources of released channels. A stream being seeked is left until the seek is done.
    static void CollectReleases(ReleaseBatch& batch)
    {
        if (ReleasingChannelCount == 0)
            return;

        for (int i = 0; i < MAX_CHANNEL_SLOTS; i++)
        {
            auto& channel = ChannelSlots[i];
            if (channel.State != SlotState::Releasing || channel.IsSeeking)
                continue;

            if (channel.Stream != nullptr)
                batch.Streams.push_back(channel.Stream);

            for (int j = 0; j < channel.EffectCount; j++)
            {
                auto& effect = EffectSlots[channel.Effects[j]];
                batch.Processors.push_back(effect.Processor);
                effect.Processor = nullptr;
                effect.State = SlotState::Free;
            }

            channel.Stream = nullptr;
            channel.EffectCount = 0;
            channel.State = SlotState::Free;
            ReleasingChannelCount--;
        }
    }

    static void DestroyReleases(ReleaseBatch& batch)
    {
        for (auto* stream : batch.Streams)
            delete stream;

        for (auto* processor : batch.Processors)
            delete processor;

        for (auto* data : batch.SampleData)
            delete[] data;

        batch.Streams.clear();
        batch.Processors.clear();
        batch.SampleData.clear();
    }

    // Holds the mixer lock for an API call. On unlock, frees released resources and runs queued syncs.
    class ApiScope
    {
    public:
        ApiScope()
        {
            MixerMutex.lock();
            _isLocked = true;
            CatchUpStalledOutput();
        }

        ApiScope(const ApiScope&) = delete;
        ApiScope& operator=(const ApiScope&) = delete;

        ~ApiScope()
        {
            Unlock();
        }

        void FreeSampleData(float* data)
        {
            if (data != nullptr)
                _batch.SampleData.push_back(data);
        }

        void Unlock()
        {
            if (!_isLocked)
                return;

            SyncCall calls[MAX_PENDING_SYNC_CALLS];
            CollectReleases(_batch);
            int callCount = TakePendingSyncCalls(calls);

            _isLocked = false;
            MixerMutex.unlock();

            DestroyReleases(_batch);
            RunSyncCalls(calls, callCount);
        }

    private:
        bool         _isLocked = false;
        ReleaseBatch _batch    = {};
    };

    static int AllocateChannel(ChannelType type, int device)
    {
        for (int i = 0; i < MAX_CHANNEL_SLOTS; i++)
        {
            int index = (NextChannelSlot + i) % MAX_CHANNEL_SLOTS;
            auto& channel = ChannelSlots[index];
            if (channel.State != SlotState::Free)
                continue;

            int generation = channel.Generation;
            channel = ChannelSlot{};
            channel.Generation = generation;
            channel.State = SlotState::Live;
            channel.Type = type;
            channel.Device = device;
            channel.StartOrder = ++ChannelCounter;

            NextChannelSlot = (index + 1) % MAX_CHANNEL_SLOTS;
            return index;
        }

        return -1;
    }

    static int AllocateSample()
    {
        for (int i = 0; i < MAX_SAMPLE_SLOTS; i++)
        {
            int index = (NextSampleSlot + i) % MAX_SAMPLE_SLOTS;
            auto& sample = SampleSlots[index];
            if (sample.State != SlotState::Free)
                continue;

            int generation = sample.Generation;
            sample = SampleSlot{};
            sample.Generation = generation;
            sample.State = SlotState::Live;

            NextSampleSlot = (index + 1) % MAX_SAMPLE_SLOTS;
            return index;
        }

        return -1;
    }

    // Frees a sample and all of its channels. Returns the sample data, to be deleted outside the lock.
    static float* FreeSample(int index)
    {
        for (int i = 0; i < MAX_CHANNEL_SLOTS; i++)
        {
            const auto& channel = ChannelSlots[i];
            if (channel.State == SlotState::Live && channel.Type == ChannelType::Sample && channel.SampleIndex == index)
                ReleaseChannel(i);
        }

        auto& sample = SampleSlots[index];
        float* data = sample.Data;
        sample.Data = nullptr;
        sample.State = SlotState::Free;
        sample.Generation++;
        return data;
    }

    // ========
    // Decoding
    // ========

    static void CloseDecoder(AudioDecoder& decoder)
    {
        if (!decoder.IsOpen)
            return;

        switch (decoder.Format)
        {
        case DecoderFormat::Wav:
            drwav_uninit(&decoder.Wav);
            break;

        case DecoderFormat::Vorbis:
            stb_vorbis_close(decoder.Vorbis);
            decoder.Vorbis = nullptr;
            break;

        case DecoderFormat::Mp3:
            drmp3_uninit(&decoder.Mp3);
            break;
        }

        decoder.IsOpen = false;
    }

    AudioDecoder::~AudioDecoder()
    {
        CloseDecoder(*this);
    }

    static void DescribeWavDecoder(AudioDecoder& decoder)
    {
        const auto& wav = decoder.Wav;
        decoder.Channels = wav.channels;
        decoder.Frequency = (int)wav.sampleRate;
        decoder.TotalFrames = (long long)wav.totalPCMFrameCount;
        decoder.OrigRes = wav.bitsPerSample;

        if (wav.translatedFormatTag == DR_WAVE_FORMAT_PCM)
        {
            decoder.CType = BASS_CTYPE_STREAM_WAV_PCM;
        }
        else if (wav.translatedFormatTag == DR_WAVE_FORMAT_IEEE_FLOAT)
        {
            decoder.CType = BASS_CTYPE_STREAM_WAV_FLOAT;
            decoder.OrigRes |= BASS_ORIGRES_FLOAT;
        }
        else
        {
            decoder.CType = BASS_CTYPE_STREAM_WAV | wav.translatedFormatTag;
        }
    }

    // A seek table makes MP3 seeks cheap; without one dr_mp3 decodes from the start of the file.
    static void BuildMp3SeekTable(AudioDecoder& decoder)
    {
        if (decoder.TotalFrames <= 0 || decoder.Frequency <= 0)
            return;

        // About one seek point per second.
        auto pointCount = (drmp3_uint32)std::clamp(decoder.TotalFrames / decoder.Frequency, 1LL, (long long)MAX_MP3_SEEK_POINTS);
        decoder.SeekPoints.resize(pointCount);
        if (!drmp3_calculate_seek_points(&decoder.Mp3, &pointCount, decoder.SeekPoints.data()) || pointCount == 0)
            return;

        decoder.SeekPoints.resize(pointCount);
        drmp3_bind_seek_table(&decoder.Mp3, pointCount, decoder.SeekPoints.data());
    }

    // Opens WAV (any format dr_wav decodes, including ADPCM), Ogg Vorbis or MP3 data in memory.
    // The data must stay valid while the decoder is open.
    static bool OpenDecoder(AudioDecoder& decoder, const unsigned char* data, size_t size, bool isStream)
    {
        if (data == nullptr || size < 4)
            return false;

        bool isWav = memcmp(data, "RIFF", 4) == 0 || memcmp(data, "RF64", 4) == 0 || memcmp(data, "riff", 4) == 0;
        if (isWav)
        {
            if (!drwav_init_memory(&decoder.Wav, data, size, nullptr))
                return false;

            decoder.Format = DecoderFormat::Wav;
            decoder.IsOpen = true;
            DescribeWavDecoder(decoder);
            return true;
        }

        if (memcmp(data, "OggS", 4) == 0)
        {
            int error = 0;
            decoder.Vorbis = stb_vorbis_open_memory(data, (int)size, &error, nullptr);
            if (decoder.Vorbis == nullptr)
                return false;

            auto info = stb_vorbis_get_info(decoder.Vorbis);
            decoder.Format = DecoderFormat::Vorbis;
            decoder.IsOpen = true;
            decoder.Channels = info.channels;
            decoder.Frequency = (int)info.sample_rate;
            decoder.TotalFrames = (long long)stb_vorbis_stream_length_in_samples(decoder.Vorbis);
            decoder.CType = BASS_CTYPE_STREAM_OGG;
            return true;
        }

        if (!drmp3_init_memory(&decoder.Mp3, data, size, nullptr))
            return false;

        // The frame count leaves out the encoder delay and padding the decoder skips.
        decoder.Format = DecoderFormat::Mp3;
        decoder.IsOpen = true;
        decoder.Channels = (int)decoder.Mp3.channels;
        decoder.Frequency = (int)decoder.Mp3.sampleRate;
        decoder.CType = BASS_CTYPE_STREAM_MP3;
        decoder.TotalFrames = (long long)drmp3_get_pcm_frame_count(&decoder.Mp3);

        if (isStream)
            BuildMp3SeekTable(decoder);

        return true;
    }

    // Reads interleaved float frames. Returns 0 at the end of the data.
    static int ReadDecoder(AudioDecoder& decoder, float* output, int frames)
    {
        switch (decoder.Format)
        {
        case DecoderFormat::Wav:
            return (int)drwav_read_pcm_frames_f32(&decoder.Wav, (drwav_uint64)frames, output);

        case DecoderFormat::Vorbis:
            return stb_vorbis_get_samples_float_interleaved(decoder.Vorbis, decoder.Channels, output, frames * decoder.Channels);

        case DecoderFormat::Mp3:
            return (int)drmp3_read_pcm_frames_f32(&decoder.Mp3, (drmp3_uint64)frames, output);
        }

        return 0;
    }

    static bool SeekDecoder(AudioDecoder& decoder, long long frame)
    {
        switch (decoder.Format)
        {
        case DecoderFormat::Wav:
            return drwav_seek_to_pcm_frame(&decoder.Wav, (drwav_uint64)frame);

        case DecoderFormat::Vorbis:
            if (frame == 0)
                return stb_vorbis_seek_start(decoder.Vorbis) != 0;

            return stb_vorbis_seek(decoder.Vorbis, (unsigned int)frame) != 0;

        case DecoderFormat::Mp3:
            return drmp3_seek_to_pcm_frame(&decoder.Mp3, (drmp3_uint64)frame);
        }

        return false;
    }

    static void DownmixToMono(DecodedAudio& audio)
    {
        int frames = (int)(audio.Data.size() / 2);
        for (int i = 0; i < frames; i++)
            audio.Data[i] = (audio.Data[i * 2] + audio.Data[i * 2 + 1]) * 0.5f;

        audio.Data.resize(frames);
        audio.Channels = 1;
    }

    // Decodes a whole file in memory to float frames, for samples.
    static int DecodeAudio(const unsigned char* data, size_t size, DWORD flags, DecodedAudio& audio)
    {
        AudioDecoder decoder;
        if (!OpenDecoder(decoder, data, size, false))
            return BASS_ERROR_FILEFORM;

        if (decoder.Channels < 1 || decoder.Channels > OUTPUT_CHANNELS || decoder.Frequency <= 0)
            return BASS_ERROR_FORMAT;

        audio.Channels = decoder.Channels;
        audio.Frequency = decoder.Frequency;
        audio.OrigRes = decoder.OrigRes;
        if (decoder.TotalFrames > 0)
            audio.Data.reserve((size_t)decoder.TotalFrames * decoder.Channels);

        auto chunk = std::vector<float>(DECODE_CHUNK_FRAMES * OUTPUT_CHANNELS);
        while (true)
        {
            int count = ReadDecoder(decoder, chunk.data(), DECODE_CHUNK_FRAMES);
            if (count <= 0)
                break;

            audio.Data.insert(audio.Data.end(), chunk.begin(), chunk.begin() + count * decoder.Channels);
        }

        // BASS turns only MPEG samples to mono on request.
        if ((flags & BASS_SAMPLE_MONO) && decoder.Format == DecoderFormat::Mp3 && audio.Channels == 2)
            DownmixToMono(audio);

        audio.Frames = (int)(audio.Data.size() / audio.Channels);
        return BASS_OK;
    }

    // Reads a file (or a part of it) in one go. Never called with the mixer lock held: file access from
    // a worker thread is proxied to the browser's main thread, which also runs the audio callback.
    static bool ReadFileBytes(const char* path, QWORD offset, QWORD length, std::vector<unsigned char>& bytes)
    {
        auto* file = fopen(path, "rb");
        if (file == nullptr)
            return false;

        bool isRead = false;
        if (fseek(file, 0, SEEK_END) == 0)
        {
            long long size = (long long)ftell(file);
            if (size >= 0 && offset <= (QWORD)size)
            {
                QWORD available = (QWORD)size - offset;
                QWORD count = (length > 0) ? std::min(length, available) : available;
                bytes.resize((size_t)count);
                isRead = fseek(file, (long)offset, SEEK_SET) == 0 && fread(bytes.data(), 1, (size_t)count, file) == (size_t)count;
            }
        }

        fclose(file);
        return isRead;
    }

    // ===========
    // Push queues
    // ===========

    static float ReadPcmValue(const void* data, int index, int sampleSize)
    {
        if (sampleSize == (int)sizeof(float))
            return ((const float*)data)[index];

        if (sampleSize == 2)
            return (float)((const short*)data)[index] / 32768.0f;

        return (float)((int)((const unsigned char*)data)[index] - 128) / 128.0f;
    }

    static void PushToQueue(StreamData& stream, int channels, DWORD flags, const void* data, int frames)
    {
        int count = std::min(frames, stream.QueueCapacity - stream.QueueFrames);
        int sampleSize = GetBytesPerSample(flags);

        for (int i = 0; i < count; i++)
        {
            int slot = (stream.QueueStart + stream.QueueFrames + i) % stream.QueueCapacity;
            for (int c = 0; c < channels; c++)
                stream.Queue[slot * channels + c] = ReadPcmValue(data, i * channels + c, sampleSize);
        }

        stream.QueueFrames += count;
    }

    static int PopFromQueue(StreamData& stream, int channels, float* output, int frames)
    {
        int count = std::min(frames, stream.QueueFrames);
        int first = std::min(count, stream.QueueCapacity - stream.QueueStart);
        const float* queue = stream.Queue.data();

        memcpy(output, queue + stream.QueueStart * channels, first * channels * sizeof(float));
        memcpy(output + first * channels, queue, (count - first) * channels * sizeof(float));

        stream.QueueStart = (stream.QueueStart + count) % stream.QueueCapacity;
        stream.QueueFrames -= count;
        return count;
    }

    static void ResetStreamBuffer(ChannelSlot& channel, long long decodedFrames)
    {
        channel.BufferFrames = 0;
        channel.Position = 0.0;
        channel.DecodedFrames = decodedFrames;
        channel.HasEnded = false;
    }

    static void ClearPushQueue(ChannelSlot& channel)
    {
        auto& stream = *channel.Stream;
        stream.QueueStart = 0;
        stream.QueueFrames = 0;
        stream.IsQueueEnded = false;
        ResetStreamBuffer(channel, 0);
    }

    // =======================
    // Channel state, attributes
    // =======================

    static void StopChannel(int index)
    {
        auto& channel = ChannelSlots[index];
        channel.Playback = PlaybackState::Stopped;

        if (channel.Type == ChannelType::Push)
            ClearPushQueue(channel);

        // Sample channels are freed when stopped, streams when they have the AUTOFREE flag.
        bool isFreed = (channel.Type == ChannelType::Sample && !channel.IsSampleStream) ||
                       (channel.Type != ChannelType::Device3D && (channel.Flags & BASS_STREAM_AUTOFREE));
        if (isFreed)
            ReleaseChannel(index);
    }

    static void StartChannel(ChannelSlot& channel, bool restart)
    {
        if (channel.Type == ChannelType::Push && restart)
            ClearPushQueue(channel);

        if (channel.Type == ChannelType::Sample && (restart || channel.HasEnded))
            channel.Position = 0.0;

        if (channel.Playback != PlaybackState::Playing)
            channel.HasGain = false;

        channel.HasEnded = false;
        channel.Playback = PlaybackState::Playing;
    }

    static int GetSlideAttribute(DWORD attribute)
    {
        switch (attribute)
        {
        case BASS_ATTRIB_VOL:
            return SLIDE_VOLUME;

        case BASS_ATTRIB_PAN:
            return SLIDE_PAN;

        case BASS_ATTRIB_FREQ:
            return SLIDE_FREQUENCY;

        default:
            return -1;
        }
    }

    static float GetAttribute(const ChannelSlot& channel, int attribute)
    {
        switch (attribute)
        {
        case SLIDE_VOLUME:
            return channel.Volume;

        case SLIDE_PAN:
            return channel.Pan;

        default:
            return (channel.Rate > 0.0f) ? channel.Rate : (float)channel.Frequency;
        }
    }

    static void SetAttribute(ChannelSlot& channel, int attribute, float value)
    {
        switch (attribute)
        {
        case SLIDE_VOLUME:
            channel.Volume = std::max(value, 0.0f);
            break;

        case SLIDE_PAN:
            channel.Pan = std::clamp(value, -1.0f, 1.0f);
            break;

        default:
            channel.Rate = (value <= 0.0f) ? 0.0f : std::max(value, MIN_PLAYBACK_RATE);
            break;
        }
    }

    static float InterpolateSlide(const SlideState& slide, float alpha)
    {
        if (alpha >= 1.0f)
            return slide.To;

        // Logarithmic slides are linear in dB. Fades towards silence stop at a floor, then drop to 0.
        if (slide.IsLogarithmic && slide.From >= 0.0f && slide.To >= 0.0f)
        {
            float from = std::max(slide.From, LOG_SLIDE_FLOOR);
            float to = std::max(slide.To, LOG_SLIDE_FLOOR);
            return from * powf(to / from, alpha);
        }

        if (slide.IsLogarithmic && slide.From < 0.0f && slide.To < 0.0f)
            return slide.From * powf(slide.To / slide.From, alpha);

        return slide.From + (slide.To - slide.From) * alpha;
    }

    // Slides advance whether the channel plays or not, as in BASS.
    static void AdvanceSlides(int index, int frames)
    {
        auto& channel = ChannelSlots[index];
        for (int attribute = 0; attribute < SLIDE_COUNT; attribute++)
        {
            auto& slide = channel.Slides[attribute];
            if (!slide.IsActive)
                continue;

            slide.Elapsed = std::min(slide.Elapsed + frames, slide.Duration);
            SetAttribute(channel, attribute, InterpolateSlide(slide, (float)slide.Elapsed / (float)slide.Duration));
            if (slide.Elapsed < slide.Duration)
                continue;

            slide.IsActive = false;
            QueueSyncs(index, BASS_SYNC_SLIDE, 0);

            if (slide.StopsChannel)
            {
                StopChannel(index);
                return;
            }
        }
    }

    // =====
    // Mixer
    // =====

    // Resamples source frames linearly to stereo output until the frame after the current one is past
    // the end of the source. Returns the number of frames written.
    static int ResampleRun(const float* source, int channels, int sourceFrames, double& position, double step, float* output, int outputFrames)
    {
        int written = 0;
        if (channels == 1)
        {
            while (written < outputFrames)
            {
                int frame = (int)position;
                if (frame + 1 >= sourceFrames)
                    break;

                float fraction = (float)(position - frame);
                float value = source[frame] + (source[frame + 1] - source[frame]) * fraction;
                output[written * 2] = value;
                output[written * 2 + 1] = value;
                position += step;
                written++;
            }
        }
        else
        {
            while (written < outputFrames)
            {
                int frame = (int)position;
                if (frame + 1 >= sourceFrames)
                    break;

                float fraction = (float)(position - frame);
                const float* current = source + frame * 2;
                output[written * 2] = current[0] + (current[2] - current[0]) * fraction;
                output[written * 2 + 1] = current[1] + (current[3] - current[1]) * fraction;
                position += step;
                written++;
            }
        }

        return written;
    }

    // Interpolates the last frame of a source towards the frame that follows it (-1 = silence).
    static void InterpolateLastFrame(const float* source, int channels, int frame, int nextFrame, float fraction, float* output)
    {
        for (int c = 0; c < channels; c++)
        {
            float current = source[frame * channels + c];
            float next = (nextFrame >= 0) ? source[nextFrame * channels + c] : 0.0f;
            output[c] = current + (next - current) * fraction;
        }

        if (channels == 1)
            output[1] = output[0];
    }

    static int RenderSample(int index, float* output, int frames, double step)
    {
        auto& channel = ChannelSlots[index];
        const auto& sample = SampleSlots[channel.SampleIndex];
        bool isLooped = (channel.Flags & BASS_SAMPLE_LOOP) != 0;

        int written = 0;
        while (written < frames)
        {
            written += ResampleRun(sample.Data, sample.Channels, sample.Frames, channel.Position, step, output + written * OUTPUT_CHANNELS, frames - written);
            if (written >= frames)
                break;

            int frame = (int)channel.Position;
            if (frame < sample.Frames)
            {
                InterpolateLastFrame(sample.Data, sample.Channels, frame, isLooped ? 0 : -1, (float)(channel.Position - frame), output + written * OUTPUT_CHANNELS);
                channel.Position += step;
                written++;
                continue;
            }

            if (!isLooped || sample.Frames == 0)
            {
                channel.HasEnded = true;
                break;
            }

            channel.Position = fmod(channel.Position, (double)sample.Frames);
            QueueSyncs(index, BASS_SYNC_END, 0);
        }

        return written;
    }

    // Gets the next frames of a file or push stream. File streams loop here when they have the LOOP flag.
    static int ReadStreamSource(int index, float* output, int frames)
    {
        auto& channel = ChannelSlots[index];
        auto& stream = *channel.Stream;

        if (channel.Type == ChannelType::Push)
        {
            int count = PopFromQueue(stream, channel.Channels, output, frames);
            channel.DecodedFrames += count;
            if (count == 0 && stream.IsQueueEnded)
                channel.HasEnded = true;

            return count;
        }

        int count = ReadDecoder(stream.Decoder, output, frames);
        if (count == 0 && (channel.Flags & BASS_SAMPLE_LOOP) && channel.DecodedFrames > 0 && SeekDecoder(stream.Decoder, 0))
        {
            channel.DecodedFrames = 0;
            QueueSyncs(index, BASS_SYNC_END, 0);
            count = ReadDecoder(stream.Decoder, output, frames);
        }

        if (count == 0)
            channel.HasEnded = true;

        channel.DecodedFrames += count;
        return count;
    }

    // Moves the unplayed frames to the front of the stream buffer and decodes more after them.
    static bool RefillStream(int index)
    {
        auto& channel = ChannelSlots[index];
        auto& buffer = channel.Stream->Buffer;
        int channels = channel.Channels;

        int keepFrom = std::min((int)channel.Position, channel.BufferFrames);
        int kept = channel.BufferFrames - keepFrom;
        if (kept > 0 && keepFrom > 0)
            memmove(buffer.data(), buffer.data() + keepFrom * channels, kept * channels * sizeof(float));

        channel.Position -= keepFrom;
        channel.BufferFrames = kept;

        int capacity = (int)buffer.size() / channels;
        int count = ReadStreamSource(index, buffer.data() + kept * channels, capacity - kept);
        channel.BufferFrames += count;
        return count > 0;
    }

    static int RenderStream(int index, float* output, int frames, double step)
    {
        auto& channel = ChannelSlots[index];

        // The API thread owns the decoder while it seeks.
        if (channel.IsSeeking)
            return 0;

        int written = 0;
        while (written < frames)
        {
            written += ResampleRun(channel.Stream->Buffer.data(), channel.Channels, channel.BufferFrames, channel.Position, step, output + written * OUTPUT_CHANNELS, frames - written);
            if (written >= frames || !RefillStream(index))
                break;
        }

        return written;
    }

    static long long GetStreamFrame(const ChannelSlot& channel)
    {
        long long frame = channel.DecodedFrames - (long long)(channel.BufferFrames - (int)channel.Position);
        return std::max(frame, 0LL);
    }

    // Distance model of DirectSound and BASS: full volume within the minimum distance, no further
    // attenuation beyond the maximum, min / (min + rolloff * (distance - min)) in between.
    static float GetDistanceGain(float distance, float minDistance, float maxDistance, float rolloff)
    {
        if (minDistance <= 0.0f || distance <= minDistance)
            return 1.0f;

        float clamped = std::min(distance, std::max(maxDistance, minDistance));
        return minDistance / (minDistance + rolloff * (clamped - minDistance));
    }

    static float GetDopplerShift(float listenerSpeed, float sourceSpeed)
    {
        if (Listener.DopplerFactor <= 0.0f)
            return 1.0f;

        float speedOfSound = SPEED_OF_SOUND / Listener.DistanceFactor;
        float shift = (speedOfSound + listenerSpeed * Listener.DopplerFactor) / (speedOfSound + sourceSpeed * Listener.DopplerFactor);
        if (!std::isfinite(shift))
            return 1.0f;

        return std::clamp(shift, MIN_DOPPLER_SHIFT, MAX_DOPPLER_SHIFT);
    }

    // Left and right gains of a channel (volume, pan or 3D position) and its doppler shift.
    static void ComputeChannelGains(const ChannelSlot& channel, float& left, float& right, float& doppler)
    {
        doppler = 1.0f;
        float volume = channel.Volume;

        if (!(channel.Flags & BASS_SAMPLE_3D))
        {
            left = volume * ((channel.Pan > 0.0f) ? (1.0f - channel.Pan) : 1.0f);
            right = volume * ((channel.Pan < 0.0f) ? (1.0f + channel.Pan) : 1.0f);
            return;
        }

        if (channel.Mode3D == BASS_3DMODE_OFF)
        {
            left = right = volume;
            return;
        }

        // Offset from the listener, and the part of it to the listener's right.
        bool isRelative = (channel.Mode3D == BASS_3DMODE_RELATIVE);
        auto offset = isRelative ? channel.Position3D : (channel.Position3D - Listener.Position);
        float distance = offset.Length();
        float sideways = isRelative ? offset.x : offset.Dot(Listener.Right);

        float pan = 0.0f;
        if (distance > MIN_DIRECTION_DISTANCE)
        {
            auto direction = offset / distance;
            float listenerSpeed = isRelative ? 0.0f : Listener.Velocity.Dot(direction);
            doppler = GetDopplerShift(listenerSpeed, channel.Velocity3D.Dot(direction));
            pan = std::clamp(sideways / distance, -1.0f, 1.0f);
        }

        float gain = volume * GetDistanceGain(distance, channel.MinDistance, channel.MaxDistance, Listener.RolloffFactor);
        if ((channel.Flags & BASS_SAMPLE_MUTEMAX) && distance >= channel.MaxDistance)
            gain = 0.0f;

        float farSide = 1.0f - fabsf(pan) * PAN_DEPTH_3D;
        left = gain * ((pan > 0.0f) ? farSide : 1.0f);
        right = gain * ((pan < 0.0f) ? farSide : 1.0f);
    }

    static float ProcessComb(CombFilter& comb, float input, float feedback, float damp1, float damp2)
    {
        float output = comb.Buffer[comb.Index];
        comb.Store = output * damp2 + comb.Store * damp1;
        comb.Store += ANTI_DENORMAL;
        comb.Store -= ANTI_DENORMAL;
        comb.Buffer[comb.Index] = input + comb.Store * feedback;

        if (++comb.Index >= comb.Size)
            comb.Index = 0;

        return output;
    }

    static float ProcessAllpass(AllpassFilter& allpass, float input)
    {
        float delayed = allpass.Buffer[allpass.Index];
        float stored = input + delayed * FREEVERB_ALLPASS_FEEDBACK;
        stored += ANTI_DENORMAL;
        stored -= ANTI_DENORMAL;
        allpass.Buffer[allpass.Index] = stored;

        if (++allpass.Index >= allpass.Size)
            allpass.Index = 0;

        return delayed - input;
    }

    // Jezar's Freeverb: 8 parallel combs and 4 serial allpasses per side, fed with the mono sum.
    static void ProcessFreeverb(FreeverbState& reverb, float* buffer, int frames)
    {
        if (!reverb.IsEnabled)
            return;

        for (int i = 0; i < frames; i++)
        {
            float inputLeft = buffer[i * 2];
            float inputRight = buffer[i * 2 + 1];
            float input = (inputLeft + inputRight) * reverb.InputGain;

            float outputLeft = 0.0f;
            float outputRight = 0.0f;
            for (int c = 0; c < FREEVERB_COMB_COUNT; c++)
            {
                outputLeft += ProcessComb(reverb.Combs[0][c], input, reverb.Feedback, reverb.Damp1, reverb.Damp2);
                outputRight += ProcessComb(reverb.Combs[1][c], input, reverb.Feedback, reverb.Damp1, reverb.Damp2);
            }

            for (int a = 0; a < FREEVERB_ALLPASS_COUNT; a++)
            {
                outputLeft = ProcessAllpass(reverb.Allpasses[0][a], outputLeft);
                outputRight = ProcessAllpass(reverb.Allpasses[1][a], outputRight);
            }

            buffer[i * 2] = outputLeft * reverb.Wet1 + outputRight * reverb.Wet2 + inputLeft * reverb.Dry;
            buffer[i * 2 + 1] = outputRight * reverb.Wet1 + outputLeft * reverb.Wet2 + inputRight * reverb.Dry;
        }
    }

    // Feed-forward peak compressor with a dB envelope (linked stereo), then the output gain.
    static void ProcessCompressor(CompressorState& compressor, float* buffer, int frames)
    {
        if (!compressor.IsEnabled)
            return;

        for (int i = 0; i < frames; i++)
        {
            float left = buffer[i * 2];
            float right = buffer[i * 2 + 1];
            float key = std::max(fabsf(left), fabsf(right));

            float overDb = (key > compressor.Threshold) ? (20.0f * log10f(key) - compressor.ThresholdDb) : 0.0f;
            float coefficient = (overDb > compressor.Envelope) ? compressor.Attack : compressor.Release;
            compressor.Envelope = overDb + coefficient * (compressor.Envelope - overDb);

            float gain = compressor.OutputGain;
            if (compressor.Envelope > COMPRESSOR_MIN_REDUCTION)
                gain *= powf(10.0f, compressor.Envelope * compressor.Slope / 20.0f);
            else
                compressor.Envelope = 0.0f;

            buffer[i * 2] = left * gain;
            buffer[i * 2 + 1] = right * gain;
        }
    }

    static void ApplyEffects(const ChannelSlot& channel, float* buffer, int frames)
    {
        for (int i = 0; i < channel.EffectCount; i++)
        {
            auto* processor = EffectSlots[channel.Effects[i]].Processor;
            if (processor->Type == BASS_FX_BFX_FREEVERB)
            {
                ProcessFreeverb(processor->Reverb, buffer, frames);
            }
            else
            {
                ProcessCompressor(processor->Compressor, buffer, frames);
            }
        }
    }

    // Keeps per-block sums and peaks of what the channel played (before volume) for BASS_ChannelGetLevelEx.
    static void RecordLevels(ChannelSlot& channel, const float* buffer, int frames)
    {
        auto block = LevelBlock{};
        block.Frames = frames;

        for (int i = 0; i < frames; i++)
        {
            float left = buffer[i * 2];
            float right = buffer[i * 2 + 1];
            block.SumLeft += left * left;
            block.SumRight += right * right;
            block.PeakLeft = std::max(block.PeakLeft, fabsf(left));
            block.PeakRight = std::max(block.PeakRight, fabsf(right));
        }

        channel.Levels[channel.LevelNext] = block;
        channel.LevelNext = (channel.LevelNext + 1) % LEVEL_BLOCK_COUNT;
        channel.LevelCount = std::min(channel.LevelCount + 1, LEVEL_BLOCK_COUNT);
    }

    // Adds a buffer to the mix, ramping the gains over the chunk to avoid clicks.
    static void AccumulateFrames(float* destination, const float* source, int frames, float fromLeft, float fromRight, float toLeft, float toRight)
    {
        float stepLeft = (toLeft - fromLeft) / (float)frames;
        float stepRight = (toRight - fromRight) / (float)frames;
        float gainLeft = fromLeft;
        float gainRight = fromRight;

        for (int i = 0; i < frames; i++)
        {
            gainLeft += stepLeft;
            gainRight += stepRight;
            destination[i * 2] += source[i * 2] * gainLeft;
            destination[i * 2 + 1] += source[i * 2 + 1] * gainRight;
        }
    }

    static void MixRendered(ChannelSlot& channel, float* destination, const float* source, int frames, float targetLeft, float targetRight)
    {
        if (!channel.HasGain)
        {
            channel.GainLeft = targetLeft;
            channel.GainRight = targetRight;
            channel.HasGain = true;
        }

        AccumulateFrames(destination, source, frames, channel.GainLeft, channel.GainRight, targetLeft, targetRight);
        channel.GainLeft = targetLeft;
        channel.GainRight = targetRight;
    }

    static void MixChannel(int index, float* destination, int frames)
    {
        auto& channel = ChannelSlots[index];

        float targetLeft = 0.0f;
        float targetRight = 0.0f;
        float doppler = 1.0f;
        ComputeChannelGains(channel, targetLeft, targetRight, doppler);

        float rate = ((channel.Rate > 0.0f) ? channel.Rate : (float)channel.Frequency) * doppler;
        double step = std::clamp((double)rate / (double)OutputRate, 0.0, MAX_PLAYBACK_STEP);

        int rendered = (channel.Type == ChannelType::Sample) ?
            RenderSample(index, ChannelBuffer, frames, step) :
            RenderStream(index, ChannelBuffer, frames, step);

        if (rendered < frames)
            memset(ChannelBuffer + rendered * OUTPUT_CHANNELS, 0, (frames - rendered) * OUTPUT_FRAME_BYTES);

        if (channel.EffectCount > 0)
            ApplyEffects(channel, ChannelBuffer, frames);

        RecordLevels(channel, ChannelBuffer, frames);
        MixRendered(channel, destination, ChannelBuffer, frames, targetLeft, targetRight);

        if (channel.HasEnded)
        {
            QueueSyncs(index, BASS_SYNC_END, 0);
            StopChannel(index);
        }
    }

    // The 3D bus goes through the FX and volume of the 3D mix stream while that plays, else straight out.
    static void MixBus(float* output, int frames)
    {
        int busIndex = Device3DStreams[OUTPUT_DEVICE];
        bool isBusPlaying = busIndex >= 0 &&
                            ChannelSlots[busIndex].State == SlotState::Live &&
                            ChannelSlots[busIndex].Playback == PlaybackState::Playing &&
                            ChannelSlots[busIndex].LockCount == 0;

        if (!isBusPlaying)
        {
            for (int i = 0; i < frames * OUTPUT_CHANNELS; i++)
                output[i] += BusBuffer[i];

            return;
        }

        auto& bus = ChannelSlots[busIndex];
        if (bus.EffectCount > 0)
            ApplyEffects(bus, BusBuffer, frames);

        float targetLeft = 0.0f;
        float targetRight = 0.0f;
        float doppler = 1.0f;
        ComputeChannelGains(bus, targetLeft, targetRight, doppler);

        RecordLevels(bus, BusBuffer, frames);
        MixRendered(bus, output, BusBuffer, frames, targetLeft, targetRight);
    }

    static void MixChunk(float* output, int frames)
    {
        int valueCount = frames * OUTPUT_CHANNELS;
        memset(output, 0, valueCount * sizeof(float));
        memset(BusBuffer, 0, valueCount * sizeof(float));

        if (!DeviceInitialized[OUTPUT_DEVICE] || DevicePaused[OUTPUT_DEVICE])
            return;

        for (int i = 0; i < MAX_CHANNEL_SLOTS; i++)
        {
            auto& channel = ChannelSlots[i];
            if (channel.State != SlotState::Live || channel.Device != OUTPUT_DEVICE || channel.LockCount > 0)
                continue;

            AdvanceSlides(i, frames);
            if (channel.State != SlotState::Live || channel.Playback != PlaybackState::Playing || channel.Type == ChannelType::Device3D)
                continue;

            MixChannel(i, (channel.Flags & BASS_SAMPLE_3D) ? BusBuffer : output, frames);
        }

        MixBus(output, frames);

        for (int i = 0; i < valueCount; i++)
            output[i] = std::clamp(output[i], -1.0f, 1.0f);
    }

    // Web Audio stops calling back while the page's audio context is suspended (before the first user
    // gesture, or in a hidden tab on some browsers). The mix then advances with the clock, unheard, so
    // sounds still end and free their channels as they would on a playing device.
    static void CatchUpStalledOutput()
    {
        if (OutputStream == nullptr || !DeviceInitialized[OUTPUT_DEVICE])
            return;

        auto now = (long long)SDL_GetTicksNS();
        if (now - LastCallbackTime < OUTPUT_STALL_TIME)
            return;

        // After a long stall, skip ahead instead of mixing all of it.
        long long elapsed = now - MixedUntilTime;
        if (elapsed > MAX_STALL_CATCH_UP_TIME)
        {
            MixedUntilTime = now - MAX_STALL_CATCH_UP_TIME;
            elapsed = MAX_STALL_CATCH_UP_TIME;
        }

        // Stop early while many syncs wait, so none are dropped; the next call continues.
        int chunks = (int)(elapsed * OutputRate / NANOSECONDS_PER_SECOND / MIX_CHUNK_FRAMES);
        int mixedChunks = 0;
        while (mixedChunks < chunks && PendingSyncCallCount < MAX_PENDING_SYNC_CALLS / 2)
        {
            MixChunk(StallBuffer, MIX_CHUNK_FRAMES);
            mixedChunks++;
        }

        MixedUntilTime += (long long)mixedChunks * MIX_CHUNK_FRAMES * NANOSECONDS_PER_SECOND / OutputRate;
    }

    // SDL asks for more data: mix it in chunks, each under the lock, and call syncs between them.
    static void SDLCALL OnAudioRequest(void* userData, SDL_AudioStream* stream, int additionalAmount, int totalAmount)
    {
        auto now = (long long)SDL_GetTicksNS();
        int framesLeft = (additionalAmount + OUTPUT_FRAME_BYTES - 1) / OUTPUT_FRAME_BYTES;
        while (framesLeft > 0)
        {
            int frames = std::min(framesLeft, MIX_CHUNK_FRAMES);
            SyncCall calls[MAX_PENDING_SYNC_CALLS];
            int callCount = 0;

            MixerMutex.lock();
            LastCallbackTime = now;
            MixedUntilTime = now;
            MixChunk(MixBuffer, frames);
            callCount = TakePendingSyncCalls(calls);
            MixerMutex.unlock();

            RunSyncCalls(calls, callCount);
            SDL_PutAudioStreamData(stream, MixBuffer, frames * OUTPUT_FRAME_BYTES);
            framesLeft -= frames;
        }
    }

    static SDL_AudioStream* OpenOutputStream(int rate)
    {
        // SDL's default buffer is small for Web Audio on a main thread that also draws the game.
        if (SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES) == nullptr)
            SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, OUTPUT_DEVICE_FRAMES);

        auto spec = SDL_AudioSpec{ SDL_AUDIO_F32, OUTPUT_CHANNELS, rate };
        auto* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, OnAudioRequest, nullptr);
        if (stream == nullptr)
            return nullptr;

        SDL_ResumeAudioStreamDevice(stream);
        return stream;
    }

    // ==========================
    // Stream seeking, sample use
    // ==========================

    // Seeks a file stream on the calling thread without the mixer lock; the mixer leaves the stream
    // silent meanwhile (decoders such as ADPCM WAV may decode up to the target to seek).
    static BOOL SeekFileStream(DWORD handle, long long frame)
    {
        int index = -1;
        AudioDecoder* decoder = nullptr;
        {
            auto scope = ApiScope();
            index = GetChannelIndex(handle);
            if (index < 0)
                return Fail(BASS_ERROR_HANDLE);

            auto& channel = ChannelSlots[index];
            if (channel.IsSeeking)
                return Fail(BASS_ERROR_BUSY);

            channel.IsSeeking = true;
            decoder = &channel.Stream->Decoder;
        }

        bool isSeeked = SeekDecoder(*decoder, frame);

        auto scope = ApiScope();
        auto& channel = ChannelSlots[index];
        channel.IsSeeking = false;

        // The stream may have been freed meanwhile; the scope frees its data now.
        if (channel.State != SlotState::Live || GetChannelIndex(handle) != index)
            return Fail(BASS_ERROR_HANDLE);

        ResetStreamBuffer(channel, isSeeked ? frame : 0);
        if (!isSeeked)
            return Fail(BASS_ERROR_POSITION);

        return Succeed();
    }

    static float GetOverrideScore(const ChannelSlot& channel, DWORD mode)
    {
        if (mode == BASS_SAMPLE_OVER_DIST)
            return Vector3::Distance(channel.Position3D, Listener.Position);

        if (mode == BASS_SAMPLE_OVER_VOL)
            return -channel.Volume;

        return 0.0f;
    }

    // Applies the sample's limit of simultaneous channels, overriding one if the flags allow it.
    static bool MakeRoomForSampleChannel(int sampleIndex, DWORD flags)
    {
        const auto& sample = SampleSlots[sampleIndex];
        DWORD mode = (flags & OVERRIDE_FLAGS) ? (flags & OVERRIDE_FLAGS) : (sample.Flags & OVERRIDE_FLAGS);

        int count = 0;
        int victim = -1;
        for (int i = 0; i < MAX_CHANNEL_SLOTS; i++)
        {
            const auto& channel = ChannelSlots[i];
            if (channel.State != SlotState::Live || channel.Type != ChannelType::Sample ||
                channel.IsSampleStream || channel.SampleIndex != sampleIndex)
            {
                continue;
            }

            count++;
            if (victim < 0)
            {
                victim = i;
                continue;
            }

            // Highest score first, the oldest channel on a tie (and for OVER_POS).
            float score = GetOverrideScore(channel, mode);
            float victimScore = GetOverrideScore(ChannelSlots[victim], mode);
            if (score > victimScore || (score == victimScore && channel.StartOrder < ChannelSlots[victim].StartOrder))
                victim = i;
        }

        if (count < (int)sample.Max)
            return true;

        if ((flags & BASS_SAMCHAN_NEW) || mode == 0 || victim < 0)
            return false;

        ReleaseChannel(victim);
        return true;
    }

    static void InitializeSampleChannel(ChannelSlot& channel, int sampleIndex, DWORD flags)
    {
        const auto& sample = SampleSlots[sampleIndex];
        channel.SampleIndex = sampleIndex;
        channel.IsSampleStream = (flags & BASS_SAMCHAN_STREAM) != 0;
        channel.Flags = (sample.Flags & (STORED_SAMPLE_FLAGS & ~OVERRIDE_FLAGS)) | (flags & BASS_SAMPLE_LOOP);
        if (channel.IsSampleStream)
            channel.Flags |= flags & BASS_STREAM_AUTOFREE;

        channel.Frequency = sample.Frequency;
        channel.Channels = sample.Channels;
        channel.CType = channel.IsSampleStream ? BASS_CTYPE_STREAM_SAMPLE : BASS_CTYPE_SAMPLE;
        channel.OrigRes = sample.OrigRes;
        channel.Volume = sample.Volume;
        channel.Pan = sample.Pan;
        channel.Mode3D = (int)sample.Mode3D;
        channel.MinDistance = sample.MinDistance;
        channel.MaxDistance = sample.MaxDistance;
        channel.InsideAngle = (int)sample.InsideAngle;
        channel.OutsideAngle = (int)sample.OutsideAngle;
        channel.OutsideVolume = sample.OutsideVolume;

        // A new sample channel starts paused in BASS, a sample stream stopped.
        channel.Playback = channel.IsSampleStream ? PlaybackState::Stopped : PlaybackState::Paused;
    }

    static void WriteSampleData(const SampleSlot& sample, void* buffer)
    {
        int valueCount = sample.Frames * sample.Channels;
        if (sample.Flags & BASS_SAMPLE_FLOAT)
        {
            memcpy(buffer, sample.Data, valueCount * sizeof(float));
        }
        else if (sample.Flags & BASS_SAMPLE_8BITS)
        {
            auto* output = (unsigned char*)buffer;
            for (int i = 0; i < valueCount; i++)
                output[i] = (unsigned char)std::clamp((int)lrintf(sample.Data[i] * 128.0f) + 128, 0, 255);
        }
        else
        {
            auto* output = (short*)buffer;
            for (int i = 0; i < valueCount; i++)
                output[i] = (short)std::clamp((int)lrintf(sample.Data[i] * 32768.0f), -32768, 32767);
        }
    }

    // =======
    // Effects
    // =======

    static void InitializeFreeverb(FreeverbState& reverb, int rate)
    {
        float scale = (float)rate / FREEVERB_TUNING_RATE;
        for (int side = 0; side < OUTPUT_CHANNELS; side++)
        {
            int spread = side * FREEVERB_STEREO_SPREAD;
            for (int c = 0; c < FREEVERB_COMB_COUNT; c++)
            {
                auto& comb = reverb.Combs[side][c];
                comb.Size = std::max(1, (int)((float)(FREEVERB_COMB_TUNING[c] + spread) * scale));
                comb.Buffer.assign(comb.Size, 0.0f);
            }

            for (int a = 0; a < FREEVERB_ALLPASS_COUNT; a++)
            {
                auto& allpass = reverb.Allpasses[side][a];
                allpass.Size = std::max(1, (int)((float)(FREEVERB_ALLPASS_TUNING[a] + spread) * scale));
                allpass.Buffer.assign(allpass.Size, 0.0f);
            }
        }
    }

    // BASS_FX takes Freeverb's internal wet (0..3) and dry (0..1) gains, and its 0..1 room size and damping.
    static void SetFreeverbParameters(FreeverbState& reverb, const BASS_BFX_FREEVERB& parameters)
    {
        float roomSize = std::clamp(parameters.fRoomSize, 0.0f, 1.0f) * FREEVERB_SCALE_ROOM + FREEVERB_OFFSET_ROOM;
        float damp = std::clamp(parameters.fDamp, 0.0f, 1.0f) * FREEVERB_SCALE_DAMP;
        float wet = std::clamp(parameters.fWetMix, 0.0f, FREEVERB_MAX_WET);
        float width = std::clamp(parameters.fWidth, 0.0f, 1.0f);
        bool isFrozen = (parameters.lMode & BASS_BFX_FREEVERB_MODE_FREEZE) != 0;

        reverb.IsEnabled = (parameters.lChannel != BASS_BFX_CHANNONE);
        reverb.Dry = std::clamp(parameters.fDryMix, 0.0f, 1.0f);
        reverb.Wet1 = wet * (width / 2.0f + 0.5f);
        reverb.Wet2 = wet * ((1.0f - width) / 2.0f);
        reverb.Feedback = isFrozen ? 1.0f : roomSize;
        reverb.Damp1 = isFrozen ? 0.0f : damp;
        reverb.Damp2 = 1.0f - reverb.Damp1;
        reverb.InputGain = isFrozen ? 0.0f : FREEVERB_FIXED_GAIN;
    }

    static void SetCompressorParameters(CompressorState& compressor, const BASS_BFX_COMPRESSOR2& parameters, int rate)
    {
        float thresholdDb = std::clamp(parameters.fThreshold, -60.0f, 0.0f);
        float ratio = std::max(parameters.fRatio, 1.0f);
        float attack = std::clamp(parameters.fAttack, 0.01f, 1000.0f);
        float release = std::clamp(parameters.fRelease, 0.01f, 5000.0f);

        compressor.IsEnabled = (parameters.lChannel != BASS_BFX_CHANNONE);
        compressor.OutputGain = powf(10.0f, std::clamp(parameters.fGain, -60.0f, 60.0f) / 20.0f);
        compressor.ThresholdDb = thresholdDb;
        compressor.Threshold = powf(10.0f, thresholdDb / 20.0f);
        compressor.Slope = 1.0f / ratio - 1.0f;
        compressor.Attack = expf(-1000.0f / (attack * (float)rate));
        compressor.Release = expf(-1000.0f / (release * (float)rate));
    }

    static std::unique_ptr<EffectProcessor> CreateEffectProcessor(DWORD type, int rate)
    {
        auto processor = std::make_unique<EffectProcessor>();
        processor->Type = type;

        if (type == BASS_FX_BFX_FREEVERB)
        {
            InitializeFreeverb(processor->Reverb, rate);
            auto defaults = BASS_BFX_FREEVERB{ 0.0f, 1.0f, 0.5f, 0.5f, 1.0f, 0, BASS_BFX_CHANALL };
            SetFreeverbParameters(processor->Reverb, defaults);
        }
        else
        {
            auto defaults = BASS_BFX_COMPRESSOR2{ 0.0f, -15.0f, 3.0f, 10.0f, 200.0f, BASS_BFX_CHANALL };
            SetCompressorParameters(processor->Compressor, defaults, rate);
        }

        return processor;
    }

    // DSP/FX with a higher priority run first; equal priorities keep the order they were set in.
    static void InsertChannelEffect(ChannelSlot& channel, int effectIndex, int priority)
    {
        int position = channel.EffectCount;
        while (position > 0 && EffectSlots[channel.Effects[position - 1]].Priority < priority)
        {
            channel.Effects[position] = channel.Effects[position - 1];
            position--;
        }

        channel.Effects[position] = effectIndex;
        channel.EffectCount++;
    }

    static HSTREAM CreateDevice3DStream()
    {
        auto scope = ApiScope();
        int device = GetCurrentDevice();
        if (device < 0)
            return FailHandle(BASS_ERROR_INIT);

        // Like BASS, a device has one 3D mix stream; asking again returns it.
        if (Device3DStreams[device] >= 0)
        {
            LastError = BASS_OK;
            return GetChannelHandle(Device3DStreams[device]);
        }

        int index = AllocateChannel(ChannelType::Device3D, device);
        if (index < 0)
            return FailHandle(BASS_ERROR_MEM);

        auto& channel = ChannelSlots[index];
        channel.Frequency = OutputRate;
        channel.Channels = OUTPUT_CHANNELS;
        channel.Flags = BASS_SAMPLE_FLOAT;
        channel.CType = BASS_CTYPE_STREAM_DEVICE;
        Device3DStreams[device] = index;

        LastError = BASS_OK;
        return GetChannelHandle(index);
    }

    static HSTREAM InsertStream(std::unique_ptr<StreamData> stream, ChannelType type, int frequency, int channels, DWORD flags, DWORD ctype, DWORD origRes)
    {
        auto scope = ApiScope();
        int device = GetCurrentDevice();
        if (device < 0)
            return FailHandle(BASS_ERROR_INIT);

        int index = AllocateChannel(type, device);
        if (index < 0)
            return FailHandle(BASS_ERROR_MEM);

        auto& channel = ChannelSlots[index];
        channel.Flags = flags & STORED_STREAM_FLAGS;
        channel.Frequency = frequency;
        channel.Channels = channels;
        channel.CType = ctype;
        channel.OrigRes = origRes;
        channel.Stream = stream.release();

        LastError = BASS_OK;
        return GetChannelHandle(index);
    }
}

using namespace TEN::Platform::BassWeb;

extern "C"
{
    BOOL BASS_SetConfig(DWORD option, DWORD value)
    {
        // Buffer, update and 3D algorithm options have no meaning for this mixer.
        return Succeed();
    }

    int BASS_ErrorGetCode(void)
    {
        return LastError;
    }

    BOOL BASS_GetDeviceInfo(DWORD device, BASS_DEVICEINFO* info)
    {
        if (device >= DEVICE_COUNT)
            return Fail(BASS_ERROR_DEVICE);

        if (info == nullptr)
            return Fail(BASS_ERROR_ILLPARAM);

        auto scope = ApiScope();
        if (device == NO_SOUND_DEVICE)
        {
            info->name = "No sound";
            info->driver = nullptr;
            info->flags = BASS_DEVICE_ENABLED;
        }
        else
        {
            info->name = "Browser";
            info->driver = "WebAudio";
            info->flags = BASS_DEVICE_ENABLED | BASS_DEVICE_DEFAULT;
        }

        if (DeviceInitialized[device])
            info->flags |= BASS_DEVICE_INIT;

        return Succeed();
    }

    BOOL BASS_Init(int device, DWORD freq, DWORD flags, void* win, const void* dsguid)
    {
        device = ResolveDevice(device);
        if (device < 0 || device >= DEVICE_COUNT)
            return Fail(BASS_ERROR_DEVICE);

        bool needsOutput = false;
        {
            auto scope = ApiScope();
            if (DeviceInitialized[device])
                return Fail(BASS_ERROR_ALREADY);

            needsOutput = (device == OUTPUT_DEVICE && OutputStream == nullptr);
        }

        // SDL calls are made without the mixer lock, as SDL holds its own lock around the audio callback.
        int rate = (freq > 0) ? std::clamp((int)freq, MIN_OUTPUT_RATE, MAX_OUTPUT_RATE) : DEFAULT_OUTPUT_RATE;
        SDL_AudioStream* stream = nullptr;
        if (needsOutput)
        {
            stream = OpenOutputStream(rate);
            if (stream == nullptr)
                return Fail(BASS_ERROR_DRIVER);
        }

        auto scope = ApiScope();
        if (device == OUTPUT_DEVICE)
        {
            if (stream != nullptr)
                OutputStream = stream;

            OutputRate = rate;
            LastCallbackTime = (long long)SDL_GetTicksNS();
            MixedUntilTime = LastCallbackTime;
        }

        DeviceInitialized[device] = true;
        DevicePaused[device] = false;
        LastInitializedDevice = device;
        ThreadDevice = device;
        return Succeed();
    }

    BOOL BASS_Free(void)
    {
        SDL_AudioStream* stream = nullptr;
        {
            auto scope = ApiScope();
            int device = GetCurrentDevice();
            if (device < 0)
                return Fail(BASS_ERROR_INIT);

            for (int i = 0; i < MAX_CHANNEL_SLOTS; i++)
            {
                if (ChannelSlots[i].State == SlotState::Live && ChannelSlots[i].Device == device)
                    ReleaseChannel(i);
            }

            for (int i = 0; i < MAX_SAMPLE_SLOTS; i++)
            {
                if (SampleSlots[i].State == SlotState::Live && SampleSlots[i].Device == device)
                    scope.FreeSampleData(FreeSample(i));
            }

            DeviceInitialized[device] = false;
            DevicePaused[device] = false;
            if (LastInitializedDevice == device)
                LastInitializedDevice = -1;

            if (device == OUTPUT_DEVICE)
            {
                stream = OutputStream;
                OutputStream = nullptr;
            }

            LastError = BASS_OK;
        }

        if (stream != nullptr)
            SDL_DestroyAudioStream(stream);

        return TRUE;
    }

    BOOL BASS_SetDevice(DWORD device)
    {
        int target = ResolveDevice((int)device);
        if (target < 0 || target >= DEVICE_COUNT)
            return Fail(BASS_ERROR_DEVICE);

        auto scope = ApiScope();
        if (!DeviceInitialized[target])
            return Fail(BASS_ERROR_INIT);

        ThreadDevice = target;
        return Succeed();
    }

    DWORD BASS_GetDevice(void)
    {
        auto scope = ApiScope();
        int device = GetCurrentDevice();
        if (device < 0)
        {
            LastError = BASS_ERROR_INIT;
            return (DWORD)-1;
        }

        LastError = BASS_OK;
        return (DWORD)device;
    }

    BOOL BASS_Start(void)
    {
        auto scope = ApiScope();
        int device = GetCurrentDevice();
        if (device < 0)
            return Fail(BASS_ERROR_INIT);

        DevicePaused[device] = false;
        return Succeed();
    }

    BOOL BASS_Pause(void)
    {
        auto scope = ApiScope();
        int device = GetCurrentDevice();
        if (device < 0)
            return Fail(BASS_ERROR_INIT);

        DevicePaused[device] = true;
        return Succeed();
    }

    BOOL BASS_Set3DFactors(float distf, float rollf, float doppf)
    {
        auto scope = ApiScope();
        if (GetCurrentDevice() < 0)
            return Fail(BASS_ERROR_INIT);

        // Values out of range keep the current setting.
        if (std::isfinite(distf) && distf > 0.0f)
            Listener.DistanceFactor = distf;

        if (std::isfinite(rollf) && rollf >= 0.0f)
            Listener.RolloffFactor = std::min(rollf, MAX_3D_FACTOR);

        if (std::isfinite(doppf) && doppf >= 0.0f)
            Listener.DopplerFactor = std::min(doppf, MAX_3D_FACTOR);

        return Succeed();
    }

    BOOL BASS_Set3DPosition(const BASS_3DVECTOR* pos, const BASS_3DVECTOR* vel, const BASS_3DVECTOR* front, const BASS_3DVECTOR* top)
    {
        auto scope = ApiScope();
        if (GetCurrentDevice() < 0)
            return Fail(BASS_ERROR_INIT);

        if (pos != nullptr && IsFinite(*pos))
            Listener.Position = ToVector3(*pos);

        if (vel != nullptr && IsFinite(*vel))
            Listener.Velocity = ToVector3(*vel);

        if (front == nullptr || top == nullptr || !IsFinite(*front) || !IsFinite(*top))
            return Succeed();

        // The listener's right in BASS's frame (x right, y up, z front) is top x front.
        auto frontVector = ToVector3(*front);
        auto topVector = ToVector3(*top);
        auto right = topVector.Cross(frontVector);
        if (frontVector.Length() < MIN_ORIENTATION_LENGTH || right.Length() < MIN_ORIENTATION_LENGTH)
            return Succeed();

        frontVector.Normalize();
        topVector.Normalize();
        right.Normalize();
        Listener.Front = frontVector;
        Listener.Top = topVector;
        Listener.Right = right;
        return Succeed();
    }

    void BASS_Apply3D(void)
    {
        // 3D settings are read by the mixer as they are set. The engine applies every change at once,
        // and 2D sounds played from menus that never call this would stay at their default 3D setup.
    }

    HSAMPLE BASS_SampleLoad(BOOL mem, const void* file, QWORD offset, DWORD length, DWORD max, DWORD flags)
    {
        if (file == nullptr)
            return FailHandle(BASS_ERROR_FILEOPEN);

        if (max < 1 || max > 65535)
            return FailHandle(BASS_ERROR_ILLPARAM);

        {
            auto scope = ApiScope();
            if (GetCurrentDevice() < 0)
                return FailHandle(BASS_ERROR_INIT);
        }

        // Read and decode without the lock.
        auto fileData = std::vector<unsigned char>{};
        const unsigned char* data = nullptr;
        size_t size = 0;
        if (mem)
        {
            if (length == 0)
                return FailHandle(BASS_ERROR_ILLPARAM);

            data = (const unsigned char*)file + offset;
            size = length;
        }
        else
        {
            if (!ReadFileBytes((const char*)file, offset, length, fileData))
                return FailHandle(BASS_ERROR_FILEOPEN);

            data = fileData.data();
            size = fileData.size();
        }

        auto audio = DecodedAudio{};
        int error = DecodeAudio(data, size, flags, audio);
        if (error != BASS_OK)
            return FailHandle(error);

        if ((flags & BASS_SAMPLE_3D) && audio.Channels != 1)
            return FailHandle(BASS_ERROR_NO3D);

        auto* sampleData = new float[std::max((size_t)1, audio.Data.size())];
        if (!audio.Data.empty())
            memcpy(sampleData, audio.Data.data(), audio.Data.size() * sizeof(float));

        auto scope = ApiScope();
        int device = GetCurrentDevice();
        int index = (device >= 0) ? AllocateSample() : -1;
        if (index < 0)
        {
            scope.FreeSampleData(sampleData);
            return FailHandle((device < 0) ? BASS_ERROR_INIT : BASS_ERROR_MEM);
        }

        auto& sample = SampleSlots[index];
        sample.Device = device;
        sample.Data = sampleData;
        sample.Frames = audio.Frames;
        sample.Channels = audio.Channels;
        sample.Frequency = audio.Frequency;
        sample.Flags = flags & STORED_SAMPLE_FLAGS;
        sample.Max = max;
        sample.OrigRes = audio.OrigRes;

        LastError = BASS_OK;
        return GetSampleHandle(index);
    }

    BOOL BASS_SampleFree(HSAMPLE handle)
    {
        auto scope = ApiScope();
        int index = GetSampleIndex(handle);
        if (index < 0)
            return Fail(BASS_ERROR_HANDLE);

        scope.FreeSampleData(FreeSample(index));
        return Succeed();
    }

    BOOL BASS_SampleGetData(HSAMPLE handle, void* buffer)
    {
        auto scope = ApiScope();
        int index = GetSampleIndex(handle);
        if (index < 0)
            return Fail(BASS_ERROR_HANDLE);

        if (buffer == nullptr)
            return Fail(BASS_ERROR_ILLPARAM);

        WriteSampleData(SampleSlots[index], buffer);
        return Succeed();
    }

    BOOL BASS_SampleGetInfo(HSAMPLE handle, BASS_SAMPLE* info)
    {
        if (info == nullptr)
            return Fail(BASS_ERROR_ILLPARAM);

        memset(info, 0, sizeof(BASS_SAMPLE));

        auto scope = ApiScope();
        int index = GetSampleIndex(handle);
        if (index < 0)
            return Fail(BASS_ERROR_HANDLE);

        const auto& sample = SampleSlots[index];
        info->freq = (DWORD)sample.Frequency;
        info->volume = sample.Volume;
        info->pan = sample.Pan;
        info->flags = sample.Flags;
        info->length = (DWORD)(sample.Frames * sample.Channels * GetBytesPerSample(sample.Flags));
        info->max = sample.Max;
        info->origres = sample.OrigRes;
        info->chans = (DWORD)sample.Channels;
        info->mingap = sample.MinGap;
        info->mode3d = sample.Mode3D;
        info->mindist = sample.MinDistance;
        info->maxdist = sample.MaxDistance;
        info->iangle = sample.InsideAngle;
        info->oangle = sample.OutsideAngle;
        info->outvol = sample.OutsideVolume;
        return Succeed();
    }

    DWORD BASS_SampleGetChannel(HSAMPLE handle, DWORD flags)
    {
        auto scope = ApiScope();
        int sampleIndex = GetSampleIndex(handle);
        if (sampleIndex < 0)
            return FailHandle(BASS_ERROR_HANDLE);

        // Only plain sample channels count towards the sample's maximum, as in BASS.
        if (!(flags & BASS_SAMCHAN_STREAM) && !MakeRoomForSampleChannel(sampleIndex, flags))
            return FailHandle(BASS_ERROR_NOCHAN);

        int index = AllocateChannel(ChannelType::Sample, SampleSlots[sampleIndex].Device);
        if (index < 0)
            return FailHandle(BASS_ERROR_NOCHAN);

        InitializeSampleChannel(ChannelSlots[index], sampleIndex, flags);
        LastError = BASS_OK;
        return GetChannelHandle(index);
    }

    HSTREAM BASS_StreamCreate(DWORD freq, DWORD chans, DWORD flags, STREAMPROC* proc, void* user)
    {
        if (proc == STREAMPROC_DEVICE_3D)
            return CreateDevice3DStream();

        // Only push streams and the 3D mix stream are needed by the engine.
        if (proc != STREAMPROC_PUSH || (flags & BASS_STREAM_DECODE))
            return FailHandle(BASS_ERROR_NOTAVAIL);

        if (chans < 1 || chans > OUTPUT_CHANNELS)
            return FailHandle(BASS_ERROR_FORMAT);

        if (freq < 1 || freq > MAX_PUSH_RATE)
            return FailHandle(BASS_ERROR_FREQ);

        if ((flags & BASS_SAMPLE_3D) && chans != 1)
            return FailHandle(BASS_ERROR_NO3D);

        auto stream = std::make_unique<StreamData>();
        stream->QueueCapacity = (int)freq * PUSH_QUEUE_SECONDS;
        stream->Queue.resize(stream->QueueCapacity * chans);
        stream->Buffer.resize((STREAM_BUFFER_FRAMES + 1) * chans);

        return InsertStream(std::move(stream), ChannelType::Push, (int)freq, (int)chans, flags, BASS_CTYPE_STREAM, 0);
    }

    HSTREAM BASS_StreamCreateFile(BOOL mem, const void* file, QWORD offset, QWORD length, DWORD flags)
    {
        if (file == nullptr)
            return FailHandle(BASS_ERROR_FILEOPEN);

        if (flags & BASS_STREAM_DECODE)
            return FailHandle(BASS_ERROR_NOTAVAIL);

        {
            auto scope = ApiScope();
            if (GetCurrentDevice() < 0)
                return FailHandle(BASS_ERROR_INIT);
        }

        // The whole file is kept in memory and decoded as it plays. The engine passes a UTF-8 path with
        // BASS_UNICODE (std::filesystem::path::c_str() is char on this platform).
        auto stream = std::make_unique<StreamData>();
        if (mem)
        {
            if (length == 0)
                return FailHandle(BASS_ERROR_ILLPARAM);

            const auto* data = (const unsigned char*)file + offset;
            stream->FileData.assign(data, data + length);
        }
        else
        {
            stream->FileName = (const char*)file;
            if (!ReadFileBytes(stream->FileName.c_str(), offset, length, stream->FileData))
                return FailHandle(BASS_ERROR_FILEOPEN);
        }

        auto& decoder = stream->Decoder;
        if (!OpenDecoder(decoder, stream->FileData.data(), stream->FileData.size(), true))
            return FailHandle(BASS_ERROR_FILEFORM);

        if (decoder.Channels < 1 || decoder.Channels > OUTPUT_CHANNELS || decoder.Frequency <= 0)
            return FailHandle(BASS_ERROR_FORMAT);

        if ((flags & BASS_SAMPLE_3D) && decoder.Channels != 1)
            return FailHandle(BASS_ERROR_NO3D);

        stream->Buffer.resize((STREAM_BUFFER_FRAMES + 1) * decoder.Channels);

        int frequency = decoder.Frequency;
        int channels = decoder.Channels;
        DWORD ctype = decoder.CType;
        DWORD origRes = decoder.OrigRes;
        return InsertStream(std::move(stream), ChannelType::File, frequency, channels, flags, ctype, origRes);
    }

    BOOL BASS_StreamFree(HSTREAM handle)
    {
        auto scope = ApiScope();
        int index = GetChannelIndex(handle);
        if (index < 0)
            return Fail(BASS_ERROR_HANDLE);

        const auto& channel = ChannelSlots[index];
        if (channel.Type == ChannelType::Sample && !channel.IsSampleStream)
            return Fail(BASS_ERROR_HANDLE);

        // As in BASS, a device mix stream is freed only with its device.
        if (channel.Type == ChannelType::Device3D)
            return Fail(BASS_ERROR_NOTAVAIL);

        ReleaseChannel(index);
        return Succeed();
    }

    DWORD BASS_StreamPutData(HSTREAM handle, const void* buffer, DWORD length)
    {
        auto scope = ApiScope();
        int index = GetChannelIndex(handle);
        if (index < 0)
        {
            LastError = BASS_ERROR_HANDLE;
            return (DWORD)-1;
        }

        auto& channel = ChannelSlots[index];
        if (channel.Type != ChannelType::Push)
        {
            LastError = BASS_ERROR_NOTAVAIL;
            return (DWORD)-1;
        }

        auto& stream = *channel.Stream;
        int bytesPerFrame = GetBytesPerFrame(channel);
        DWORD bytes = length & ~BASS_STREAMPROC_END;
        if (stream.IsQueueEnded)
        {
            LastError = BASS_ERROR_ENDED;
            return (DWORD)-1;
        }

        if (bytes % bytesPerFrame != 0)
        {
            LastError = BASS_ERROR_ILLPARAM;
            return (DWORD)-1;
        }

        // Data beyond the queue's few seconds is dropped; the engine pushes video audio as it plays.
        if (buffer != nullptr && bytes > 0)
            PushToQueue(stream, channel.Channels, channel.Flags, buffer, (int)(bytes / bytesPerFrame));

        if (length & BASS_STREAMPROC_END)
            stream.IsQueueEnded = true;

        LastError = BASS_OK;
        return (DWORD)(stream.QueueFrames * bytesPerFrame);
    }

    double BASS_ChannelBytes2Seconds(DWORD handle, QWORD pos)
    {
        auto scope = ApiScope();
        int index = GetChannelIndex(handle);
        if (index < 0)
        {
            LastError = BASS_ERROR_HANDLE;
            return -1.0;
        }

        const auto& channel = ChannelSlots[index];
        LastError = BASS_OK;
        return (double)pos / ((double)channel.Frequency * (double)GetBytesPerFrame(channel));
    }

    BOOL BASS_ChannelSetDevice(DWORD handle, DWORD device)
    {
        int target = ResolveDevice((int)device);
        if (target < 0 || target >= DEVICE_COUNT)
            return Fail(BASS_ERROR_DEVICE);

        auto scope = ApiScope();
        if (!DeviceInitialized[target])
            return Fail(BASS_ERROR_INIT);

        // Moving a sample moves the channels it gets from now on.
        int sampleIndex = GetSampleIndex(handle);
        if (sampleIndex >= 0)
        {
            SampleSlots[sampleIndex].Device = target;
            return Succeed();
        }

        int index = GetChannelIndex(handle);
        if (index < 0)
            return Fail(BASS_ERROR_HANDLE);

        auto& channel = ChannelSlots[index];
        if (channel.Type == ChannelType::Device3D)
            return Fail(BASS_ERROR_NOTAVAIL);

        channel.Device = target;
        channel.HasGain = false;
        return Succeed();
    }

    DWORD BASS_ChannelIsActive(DWORD handle)
    {
        auto scope = ApiScope();
        int index = GetChannelIndex(handle);
        if (index < 0)
        {
            LastError = BASS_ERROR_HANDLE;
            return BASS_ACTIVE_STOPPED;
        }

        LastError = BASS_OK;
        const auto& channel = ChannelSlots[index];
        if (channel.Playback == PlaybackState::Paused)
            return BASS_ACTIVE_PAUSED;

        if (channel.Playback == PlaybackState::Stopped)
            return BASS_ACTIVE_STOPPED;

        if (DevicePaused[channel.Device])
            return BASS_ACTIVE_PAUSED_DEVICE;

        // A push stream with nothing left to play waits for data.
        if (channel.Type == ChannelType::Push && channel.Stream->QueueFrames == 0 && channel.BufferFrames - (int)channel.Position < 2)
            return BASS_ACTIVE_STALLED;

        return BASS_ACTIVE_PLAYING;
    }

    BOOL BASS_ChannelGetInfo(DWORD handle, BASS_CHANNELINFO* info)
    {
        if (info == nullptr)
            return Fail(BASS_ERROR_ILLPARAM);

        memset(info, 0, sizeof(BASS_CHANNELINFO));

        auto scope = ApiScope();
        int index = GetChannelIndex(handle);
        if (index < 0)
            return Fail(BASS_ERROR_HANDLE);

        const auto& channel = ChannelSlots[index];
        info->freq = (DWORD)channel.Frequency;
        info->chans = (DWORD)channel.Channels;
        info->flags = channel.Flags;
        info->ctype = channel.CType;
        info->origres = channel.OrigRes;
        info->plugin = 0;
        info->sample = (channel.Type == ChannelType::Sample) ? GetSampleHandle(channel.SampleIndex) : 0;
        info->filename = (channel.Type == ChannelType::File && !channel.Stream->FileName.empty()) ? channel.Stream->FileName.c_str() : nullptr;
        return Succeed();
    }

    DWORD BASS_ChannelFlags(DWORD handle, DWORD flags, DWORD mask)
    {
        auto scope = ApiScope();
        int index = GetChannelIndex(handle);
        if (index < 0)
        {
            LastError = BASS_ERROR_HANDLE;
            return (DWORD)-1;
        }

        auto& channel = ChannelSlots[index];
        DWORD changeable = 0;
        if (channel.Type != ChannelType::Device3D)
        {
            changeable = BASS_SAMPLE_LOOP;
            if (channel.Type != ChannelType::Sample || channel.IsSampleStream)
                changeable |= BASS_STREAM_AUTOFREE;
        }

        DWORD changed = mask & changeable;
        channel.Flags = (channel.Flags & ~changed) | (flags & changed);

        LastError = BASS_OK;
        return channel.Flags;
    }

    BOOL BASS_ChannelLock(DWORD handle, BOOL lock)
    {
        // A locked channel is left alone by the mixer until it is unlocked.
        auto scope = ApiScope();
        int index = GetChannelIndex(handle);
        if (index < 0)
            return Fail(BASS_ERROR_HANDLE);

        auto& channel = ChannelSlots[index];
        if (lock)
        {
            channel.LockCount++;
        }
        else if (channel.LockCount > 0)
        {
            channel.LockCount--;
        }
        else
        {
            return Fail(BASS_ERROR_ALREADY);
        }

        return Succeed();
    }

    BOOL BASS_ChannelPlay(DWORD handle, BOOL restart)
    {
        {
            auto scope = ApiScope();
            int index = GetChannelIndex(handle);
            if (index < 0)
                return Fail(BASS_ERROR_HANDLE);

            auto& channel = ChannelSlots[index];
            if (DevicePaused[channel.Device])
                return Fail(BASS_ERROR_START);

            // A file stream that ended plays again from the start.
            bool needsSeek = (channel.Type == ChannelType::File && (restart || channel.HasEnded));
            if (!needsSeek)
            {
                StartChannel(channel, restart != FALSE);
                return Succeed();
            }
        }

        if (!SeekFileStream(handle, 0))
            return FALSE;

        auto scope = ApiScope();
        int index = GetChannelIndex(handle);
        if (index < 0)
            return Fail(BASS_ERROR_HANDLE);

        StartChannel(ChannelSlots[index], false);
        return Succeed();
    }

    BOOL BASS_ChannelStart(DWORD handle)
    {
        return BASS_ChannelPlay(handle, FALSE);
    }

    BOOL BASS_ChannelStop(DWORD handle)
    {
        auto scope = ApiScope();
        int index = GetChannelIndex(handle);
        if (index < 0)
            return Fail(BASS_ERROR_HANDLE);

        StopChannel(index);
        return Succeed();
    }

    BOOL BASS_ChannelPause(DWORD handle)
    {
        auto scope = ApiScope();
        int index = GetChannelIndex(handle);
        if (index < 0)
            return Fail(BASS_ERROR_HANDLE);

        auto& channel = ChannelSlots[index];
        if (channel.Playback == PlaybackState::Paused)
            return Fail(BASS_ERROR_ALREADY);

        if (channel.Playback != PlaybackState::Playing)
            return Fail(BASS_ERROR_NOPLAY);

        channel.Playback = PlaybackState::Paused;
        return Succeed();
    }

    BOOL BASS_ChannelSetAttribute(DWORD handle, DWORD attrib, float value)
    {
        auto scope = ApiScope();
        int index = GetChannelIndex(handle);
        if (index < 0)
            return Fail(BASS_ERROR_HANDLE);

        int attribute = GetSlideAttribute(attrib);
        if (attribute < 0)
            return Fail(BASS_ERROR_ILLTYPE);

        if (!std::isfinite(value))
            return Fail(BASS_ERROR_ILLPARAM);

        // Setting an attribute ends its slide.
        auto& channel = ChannelSlots[index];
        channel.Slides[attribute].IsActive = false;
        SetAttribute(channel, attribute, value);
        return Succeed();
    }

    BOOL BASS_ChannelSlideAttribute(DWORD handle, DWORD attrib, float value, DWORD time)
    {
        auto scope = ApiScope();
        int index = GetChannelIndex(handle);
        if (index < 0)
            return Fail(BASS_ERROR_HANDLE);

        bool isLogarithmic = (attrib & BASS_SLIDE_LOG) != 0;
        int attribute = GetSlideAttribute(attrib & ~BASS_SLIDE_LOG);
        if (attribute < 0)
            return Fail(BASS_ERROR_ILLTYPE);

        if (!std::isfinite(value))
            return Fail(BASS_ERROR_ILLPARAM);

        // A negative volume fades out and then stops the channel.
        auto& channel = ChannelSlots[index];
        bool stopsChannel = (attribute == SLIDE_VOLUME && value < 0.0f);
        float from = GetAttribute(channel, attribute);
        float to = stopsChannel ? 0.0f : value;
        if (attribute == SLIDE_FREQUENCY && to <= 0.0f)
            to = (float)channel.Frequency;

        if (isLogarithmic && !stopsChannel && ((from < 0.0f) != (to < 0.0f)))
            return Fail(BASS_ERROR_ILLPARAM);

        auto& slide = channel.Slides[attribute];
        if (time == 0)
        {
            slide.IsActive = false;
            SetAttribute(channel, attribute, to);
            if (stopsChannel)
                StopChannel(index);

            return Succeed();
        }

        slide.IsActive = true;
        slide.IsLogarithmic = isLogarithmic;
        slide.StopsChannel = stopsChannel;
        slide.From = from;
        slide.To = to;
        slide.Elapsed = 0;
        slide.Duration = std::max(1, (int)((long long)time * OutputRate / 1000));
        return Succeed();
    }

    BOOL BASS_ChannelSet3DAttributes(DWORD handle, int mode, float min, float max, int iangle, int oangle, float outvol)
    {
        auto scope = ApiScope();
        int index = GetChannelIndex(handle);
        if (index < 0)
            return Fail(BASS_ERROR_HANDLE);

        auto& channel = ChannelSlots[index];
        if (!(channel.Flags & BASS_SAMPLE_3D))
            return Fail(BASS_ERROR_NO3D);

        if (mode != -1 && (mode < BASS_3DMODE_NORMAL || mode > BASS_3DMODE_OFF))
            return Fail(BASS_ERROR_ILLPARAM);

        bool setsAngles = (iangle != -1 || oangle != -1);
        if (setsAngles && (iangle < 0 || iangle > FULL_CONE_ANGLE || oangle < 0 || oangle > FULL_CONE_ANGLE))
            return Fail(BASS_ERROR_ILLPARAM);

        // -1, or 0 or less for distances, keeps the current setting. Cones are stored but not applied.
        if (mode != -1)
            channel.Mode3D = mode;

        if (std::isfinite(min) && min > 0.0f)
            channel.MinDistance = min;

        if (std::isfinite(max) && max > 0.0f)
            channel.MaxDistance = max;

        if (setsAngles)
        {
            channel.InsideAngle = iangle;
            channel.OutsideAngle = oangle;
        }

        if (std::isfinite(outvol) && outvol >= 0.0f)
            channel.OutsideVolume = std::min(outvol, 1.0f);

        return Succeed();
    }

    BOOL BASS_ChannelSet3DPosition(DWORD handle, const BASS_3DVECTOR* pos, const BASS_3DVECTOR* orient, const BASS_3DVECTOR* vel)
    {
        auto scope = ApiScope();
        int index = GetChannelIndex(handle);
        if (index < 0)
            return Fail(BASS_ERROR_HANDLE);

        auto& channel = ChannelSlots[index];
        if (!(channel.Flags & BASS_SAMPLE_3D))
            return Fail(BASS_ERROR_NO3D);

        if (pos != nullptr && IsFinite(*pos))
            channel.Position3D = ToVector3(*pos);

        if (orient != nullptr && IsFinite(*orient))
            channel.Orientation3D = ToVector3(*orient);

        if (vel != nullptr && IsFinite(*vel))
            channel.Velocity3D = ToVector3(*vel);

        return Succeed();
    }

    QWORD BASS_ChannelGetLength(DWORD handle, DWORD mode)
    {
        auto scope = ApiScope();
        int index = GetChannelIndex(handle);
        if (index < 0)
            return FailPosition(BASS_ERROR_HANDLE);

        if ((mode & POSITION_MODE_MASK) != BASS_POS_BYTE)
            return FailPosition(BASS_ERROR_NOTAVAIL);

        const auto& channel = ChannelSlots[index];
        long long frames = -1;
        if (channel.Type == ChannelType::Sample)
        {
            frames = SampleSlots[channel.SampleIndex].Frames;
        }
        else if (channel.Type == ChannelType::File && channel.Stream->Decoder.TotalFrames > 0)
        {
            frames = channel.Stream->Decoder.TotalFrames;
        }

        if (frames < 0)
            return FailPosition(BASS_ERROR_NOTAVAIL);

        LastError = BASS_OK;
        return (QWORD)frames * (QWORD)GetBytesPerFrame(channel);
    }

    BOOL BASS_ChannelSetPosition(DWORD handle, QWORD pos, DWORD mode)
    {
        long long frame = 0;
        {
            auto scope = ApiScope();
            int index = GetChannelIndex(handle);
            if (index < 0)
                return Fail(BASS_ERROR_HANDLE);

            if ((mode & POSITION_MODE_MASK) != BASS_POS_BYTE)
                return Fail(BASS_ERROR_NOTAVAIL);

            // Byte positions are rounded down to whole frames.
            auto& channel = ChannelSlots[index];
            frame = (long long)(pos / (QWORD)GetBytesPerFrame(channel));

            switch (channel.Type)
            {
            case ChannelType::Sample:
                if (frame >= SampleSlots[channel.SampleIndex].Frames)
                    return Fail(BASS_ERROR_POSITION);

                channel.Position = (double)frame;
                channel.HasEnded = false;
                return Succeed();

            case ChannelType::Push:
                if (pos != 0)
                    return Fail(BASS_ERROR_NOTAVAIL);

                ClearPushQueue(channel);
                return Succeed();

            case ChannelType::Device3D:
                return Fail(BASS_ERROR_NOTAVAIL);

            case ChannelType::File:
                if (channel.Stream->Decoder.TotalFrames > 0 && frame >= channel.Stream->Decoder.TotalFrames)
                    return Fail(BASS_ERROR_POSITION);

                break;
            }
        }

        return SeekFileStream(handle, frame);
    }

    QWORD BASS_ChannelGetPosition(DWORD handle, DWORD mode)
    {
        auto scope = ApiScope();
        int index = GetChannelIndex(handle);
        if (index < 0)
            return FailPosition(BASS_ERROR_HANDLE);

        if ((mode & POSITION_MODE_MASK) != BASS_POS_BYTE)
            return FailPosition(BASS_ERROR_NOTAVAIL);

        const auto& channel = ChannelSlots[index];
        long long frame = 0;
        if (channel.Type == ChannelType::Sample)
        {
            frame = (long long)channel.Position;
        }
        else if (channel.Type == ChannelType::File || channel.Type == ChannelType::Push)
        {
            frame = GetStreamFrame(channel);
            if (channel.Type == ChannelType::File && channel.Stream->Decoder.TotalFrames > 0)
                frame = std::min(frame, channel.Stream->Decoder.TotalFrames);
        }

        LastError = BASS_OK;
        return (QWORD)frame * (QWORD)GetBytesPerFrame(channel);
    }

    BOOL BASS_ChannelGetLevelEx(DWORD handle, float* levels, float length, DWORD flags)
    {
        auto scope = ApiScope();
        int index = GetChannelIndex(handle);
        if (index < 0)
            return Fail(BASS_ERROR_HANDLE);

        if (levels == nullptr || !(length > 0.0f) || length > MAX_LEVEL_SECONDS)
            return Fail(BASS_ERROR_ILLPARAM);

        const auto& channel = ChannelSlots[index];
        if (channel.Playback != PlaybackState::Playing)
            return Fail(BASS_ERROR_NOPLAY);

        // Sum the most recent blocks that cover the requested length.
        int wantedFrames = (int)ceilf(length * (float)OutputRate);
        auto total = LevelBlock{};
        for (int i = 0; i < channel.LevelCount && total.Frames < wantedFrames; i++)
        {
            const auto& block = channel.Levels[(channel.LevelNext - 1 - i + LEVEL_BLOCK_COUNT) % LEVEL_BLOCK_COUNT];
            total.SumLeft += block.SumLeft;
            total.SumRight += block.SumRight;
            total.PeakLeft = std::max(total.PeakLeft, block.PeakLeft);
            total.PeakRight = std::max(total.PeakRight, block.PeakRight);
            total.Frames += block.Frames;
        }

        bool isRms = (flags & BASS_LEVEL_RMS) != 0;
        float left = 0.0f;
        float right = 0.0f;
        if (total.Frames > 0)
        {
            left = isRms ? sqrtf(total.SumLeft / (float)total.Frames) : total.PeakLeft;
            right = isRms ? sqrtf(total.SumRight / (float)total.Frames) : total.PeakRight;
        }

        if (flags & BASS_LEVEL_VOLPAN)
        {
            left *= channel.Volume * ((channel.Pan > 0.0f) ? (1.0f - channel.Pan) : 1.0f);
            right *= channel.Volume * ((channel.Pan < 0.0f) ? (1.0f + channel.Pan) : 1.0f);
        }

        if (flags & BASS_LEVEL_MONO)
        {
            levels[0] = isRms ? sqrtf((left * left + right * right) / 2.0f) : std::max(left, right);
        }
        else if ((flags & BASS_LEVEL_STEREO) || channel.Channels > 1)
        {
            levels[0] = left;
            levels[1] = right;
        }
        else
        {
            levels[0] = left;
        }

        return Succeed();
    }

    HSYNC BASS_ChannelSetSync(DWORD handle, DWORD type, QWORD param, SYNCPROC* proc, void* user)
    {
        auto scope = ApiScope();
        if (GetChannelIndex(handle) < 0)
            return FailHandle(BASS_ERROR_HANDLE);

        // Syncs are called from the mixer as their events happen (all at mix time).
        DWORD syncType = type & ~SYNC_FLAGS;
        if (syncType != BASS_SYNC_END && syncType != BASS_SYNC_FREE && syncType != BASS_SYNC_SLIDE)
            return FailHandle(BASS_ERROR_ILLTYPE);

        if (proc == nullptr)
            return FailHandle(BASS_ERROR_ILLPARAM);

        for (int i = 0; i < MAX_SYNC_SLOTS; i++)
        {
            auto& sync = SyncSlots[i];
            if (sync.State != SlotState::Free)
                continue;

            sync.State = SlotState::Live;
            sync.ChannelIndex = GetChannelIndex(handle);
            sync.Type = syncType;
            sync.IsOneTime = (type & BASS_SYNC_ONETIME) != 0;
            sync.Proc = proc;
            sync.User = user;

            LastError = BASS_OK;
            return MakeHandle(HANDLE_KIND_SYNC, sync.Generation, i);
        }

        return FailHandle(BASS_ERROR_MEM);
    }

    HFX BASS_ChannelSetFX(DWORD handle, DWORD type, int priority)
    {
        if (type != BASS_FX_BFX_FREEVERB && type != BASS_FX_BFX_COMPRESSOR2)
            return FailHandle(BASS_ERROR_ILLTYPE);

        // Allocate the effect's buffers without the lock.
        int rate = DEFAULT_OUTPUT_RATE;
        {
            auto scope = ApiScope();
            if (GetChannelIndex(handle) < 0)
                return FailHandle(BASS_ERROR_HANDLE);

            rate = OutputRate;
        }

        auto processor = CreateEffectProcessor(type, rate);

        auto scope = ApiScope();
        int index = GetChannelIndex(handle);
        if (index < 0)
            return FailHandle(BASS_ERROR_HANDLE);

        // Like BASS, plain sample channels (HCHANNEL) take no FX.
        auto& channel = ChannelSlots[index];
        if (channel.Type == ChannelType::Sample && !channel.IsSampleStream)
            return FailHandle(BASS_ERROR_HANDLE);

        if (channel.EffectCount >= MAX_CHANNEL_EFFECTS)
            return FailHandle(BASS_ERROR_MEM);

        for (int i = 0; i < MAX_EFFECT_SLOTS; i++)
        {
            auto& effect = EffectSlots[i];
            if (effect.State != SlotState::Free)
                continue;

            effect.State = SlotState::Live;
            effect.ChannelIndex = index;
            effect.Priority = priority;
            effect.Processor = processor.release();
            InsertChannelEffect(channel, i, priority);

            LastError = BASS_OK;
            return MakeHandle(HANDLE_KIND_EFFECT, effect.Generation, i);
        }

        return FailHandle(BASS_ERROR_MEM);
    }

    BOOL BASS_FXSetParameters(HFX handle, const void* params)
    {
        auto scope = ApiScope();
        int index = GetEffectIndex(handle);
        if (index < 0)
            return Fail(BASS_ERROR_HANDLE);

        if (params == nullptr)
            return Fail(BASS_ERROR_ILLPARAM);

        auto* processor = EffectSlots[index].Processor;
        if (processor->Type == BASS_FX_BFX_FREEVERB)
        {
            SetFreeverbParameters(processor->Reverb, *(const BASS_BFX_FREEVERB*)params);
        }
        else
        {
            SetCompressorParameters(processor->Compressor, *(const BASS_BFX_COMPRESSOR2*)params, OutputRate);
        }

        return Succeed();
    }

    DWORD BASS_FX_GetVersion()
    {
        LastError = BASS_OK;
        return FX_VERSION;
    }
}

#endif
