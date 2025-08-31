#pragma once

// Keep existing macros and structure.
#define LockAudioDevice()   pthread_mutex_lock(&RSDK::AudioDevice::mutex)
#define UnlockAudioDevice() pthread_mutex_unlock(&RSDK::AudioDevice::mutex)

#include <oboe/Oboe.h>
#include <pthread.h>
#include <atomic>
#include <cstdint>
#include <RSDK/Core/RetroEngine.hpp>

namespace RSDK
{
struct ChannelInfo;
struct AudioDeviceBase;

/**
 * Android Oboe-backed audio device.
 * Resilient to process freezer + resume by:
 *  - Fully closing the stream on background.
 *  - Rebuilding after resume once window/focus settle (debounced).
 *  - Priming with a few bursts of silence to avoid first-frame underrun.
 *  - Restarting from engine thread (not from Oboe callback thread).
 */
struct AudioDevice : public AudioDeviceBase,
                     public oboe::AudioStreamDataCallback,
                     public oboe::AudioStreamErrorCallback
{
    // Engine entry points
    static bool32 Init();
    static void   Release();
    static void   FrameInit();
    static void   HandleStreamLoad(ChannelInfo *channel, bool32 async);

    // Lifecycle hints from platform glue (optional but recommended)
    static void NotifyAppBackground();       // e.g., APP_CMD_PAUSE / STOP / TERM_WINDOW
    static void NotifyAppForeground();       // e.g., APP_CMD_RESUME
    static void NotifyWindowAvailable(bool available); // APP_CMD_INIT_WINDOW / TERM_WINDOW
    static void NotifyFocusChanged(bool hasFocus);     // Window focus

    // Oboe callbacks
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* oboeStream,
                                          void* audioData, int32_t numFrames) override;
    void onErrorBeforeClose(oboe::AudioStream* oboeStream, oboe::Result error) override;
    void onErrorAfterClose (oboe::AudioStream* oboeStream, oboe::Result error) override;

    static pthread_mutex_t mutex;

private:
    // ---- Engine-owned state ----
    static uint8_t              contextInitialized;
    static oboe::Result         status;
    static oboe::AudioStream*   stream;
    static AudioDevice*         audioDevice; // data/error callback target

    static void InitAudioChannels();
    static void InitMixBuffer() {}

    static void *LoadStreamASync(void *channel);

    // Stream control
    static bool createStream();
    static bool shutdownStream();

    // ---- Self-healing + lifecycle gating ----
    static std::atomic<bool> sNeedsRestart;       // set by callbacks / detector, handled in FrameInit
    static std::atomic<int>  sErrorGen;
    static int               sLastHandledErrorGen;

    // Lifecycle flags (set by Notify* helpers)
    static std::atomic<bool> sIsResumed;
    static std::atomic<bool> sHasWindow;
    static std::atomic<bool> sHasFocus;

    // Timing
    static std::atomic<long long> sLastCallbackMs;     // last onAudioReady time
    static std::atomic<long long> sDebounceUntilMs;    // don't rebuild before this time
    static std::atomic<int>       sWarmupsRemaining;   // number of silence callbacks after start

    // Called from engine thread once per frame to recover audio if needed.
    static void EnsureAlive();

    // Helpers
    static long long NowMs();
};
} // namespace RSDK
