#include "OboeAudioDevice.hpp"
#include "../Audio.hpp" // AudioDeviceBase, ProcessAudioMixing, etc.

#include <cstring>
#include <chrono>

using namespace std::chrono;

namespace RSDK
{

// ---- Statics ----
uint8_t            AudioDevice::contextInitialized = 0;
oboe::Result       AudioDevice::status             = oboe::Result::OK;
oboe::AudioStream* AudioDevice::stream             = nullptr;
pthread_mutex_t    AudioDevice::mutex;

AudioDevice*       AudioDevice::audioDevice        = nullptr;

// Lifecycle / healing
std::atomic<bool>  AudioDevice::sNeedsRestart{false};
std::atomic<int>   AudioDevice::sErrorGen{0};
int                AudioDevice::sLastHandledErrorGen = 0;

std::atomic<bool>  AudioDevice::sIsResumed{true};
std::atomic<bool>  AudioDevice::sHasWindow{true};
std::atomic<bool>  AudioDevice::sHasFocus{true};

std::atomic<long long> AudioDevice::sLastCallbackMs{0};
std::atomic<long long> AudioDevice::sDebounceUntilMs{0};
std::atomic<int>       AudioDevice::sWarmupsRemaining{0};

// ---- Internal helpers ----
static inline const char* convertToText(oboe::Result result)
{
    return oboe::convertToText(result);
}

long long AudioDevice::NowMs()
{
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void *AudioDevice::LoadStreamASync(void *channel)
{
    LoadStream((ChannelInfo *)channel);
    pthread_exit(NULL);
    return nullptr;
}

// ---- Stream management ----
static void ConfigureBuilderCommon(oboe::AudioStreamBuilder& b)
{
    b.setDirection(oboe::Direction::Output);
    b.setSampleRate(AUDIO_FREQUENCY);
    b.setChannelCount(AUDIO_CHANNELS);
    b.setFormat(oboe::AudioFormat::Float);
    b.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    b.setUsage(oboe::Usage::Game);
    b.setContentType(oboe::ContentType::Music);
    // Exclusive is best when available; some devices need Shared. Keep Exclusive by default.
    b.setSharingMode(oboe::SharingMode::Exclusive);
}

bool AudioDevice::createStream()
{
    oboe::AudioStreamBuilder builder;
    ConfigureBuilderCommon(builder);
    builder.setDataCallback(audioDevice);
    builder.setErrorCallback(audioDevice);

    oboe::Result result = builder.openStream(&stream);
    if (result != oboe::Result::OK || !stream) {
        PrintLog(PRINT_NORMAL, "Oboe: Failed to open stream: (%s)", convertToText(result));
        return false;
    }

    // Slightly enlarge buffer to reduce initial underruns.
    int32_t burst = stream->getFramesPerBurst();
    if (burst > 0) {
        stream->setBufferSizeInFrames(burst * 2);
        sWarmupsRemaining.store(3, std::memory_order_relaxed); // prime with 2-3 silent callbacks
    } else {
        sWarmupsRemaining.store(2, std::memory_order_relaxed);
    }

    // Start immediately; we will output silence during warmup callbacks.
    stream->requestStart();

    return true;
}

bool AudioDevice::shutdownStream()
{
    if (stream) {
        // Stop/start may fail depending on backend state; ignore errors.
        (void)stream->stop();
        (void)stream->close();
    }
    stream = nullptr;
    return true;
}

// ---- Engine entry points ----
bool32 AudioDevice::Init()
{
    if (!contextInitialized) {
        contextInitialized = 1;
        InitAudioChannels();
        audioDevice = new AudioDevice();
        pthread_mutex_init(&mutex, nullptr);
    }

    // Build a stream if not present.
    if (!stream && !createStream())
        return false;

    return true;
}

void AudioDevice::Release()
{
    LockAudioDevice();
    shutdownStream();
    UnlockAudioDevice();

    AudioDeviceBase::Release();
    pthread_mutex_destroy(&mutex);

    delete audioDevice;
    audioDevice = nullptr;
    contextInitialized = 0;
}

// Called once per game frame on engine thread.
void AudioDevice::FrameInit()
{
    // Attempt recovery if a previous underrun/disconnect occurred.
    EnsureAlive();

    // (No call to AudioDeviceBase::FrameInit() here)

    if (status != oboe::Result::OK) {
        stream->requestStop();
        stream->close();
        Init();
    }
}

void AudioDevice::HandleStreamLoad(ChannelInfo *channel, bool32 async)
{
    if (async) {
        pthread_t loadThread;
        pthread_create(&loadThread, NULL, LoadStreamASync, channel);
        pthread_detach(loadThread);
    }
    else {
        LoadStream(channel);
    }
}

// ---- Lifecycle hints ----
void AudioDevice::NotifyAppBackground()
{
    // Suppress restarts for a moment and fully close stream to avoid zombie state.
    sIsResumed.store(false, std::memory_order_relaxed);
    sDebounceUntilMs.store(NowMs() + 1500, std::memory_order_relaxed);

    LockAudioDevice();
    shutdownStream();
    UnlockAudioDevice();
}

void AudioDevice::NotifyAppForeground()
{
    sIsResumed.store(true, std::memory_order_relaxed);
    // Debounce a little to let device routing/focus settle
    sDebounceUntilMs.store(NowMs() + 300, std::memory_order_relaxed);
    sNeedsRestart.store(true, std::memory_order_relaxed);
    sErrorGen.fetch_add(1, std::memory_order_relaxed);
}

void AudioDevice::NotifyWindowAvailable(bool available)
{
    sHasWindow.store(available, std::memory_order_relaxed);
    // If window just appeared, allow a small grace period before starting audio
    if (available) {
        long long t = NowMs();
        if (t + 200 > sDebounceUntilMs.load())
            sDebounceUntilMs.store(t + 200, std::memory_order_relaxed);
    }
}

void AudioDevice::NotifyFocusChanged(bool hasFocus)
{
    sHasFocus.store(hasFocus, std::memory_order_relaxed);
    if (!hasFocus) {
        sDebounceUntilMs.store(NowMs() + 300, std::memory_order_relaxed);
    }
}

// ---- Oboe callbacks ----
oboe::DataCallbackResult AudioDevice::onAudioReady(oboe::AudioStream *s, void *data, int32_t numFrames)
{
    if (s != stream || data == nullptr || numFrames <= 0)
        return oboe::DataCallbackResult::Stop;

    sLastCallbackMs.store(NowMs(), std::memory_order_relaxed);

    // During warmup or while we're scheduled for restart, feed silence to avoid pops/underrun spam.
    if (sWarmupsRemaining.load(std::memory_order_relaxed) > 0
        || sNeedsRestart.load(std::memory_order_relaxed))
    {
        std::memset(data, 0, (size_t)numFrames * AUDIO_CHANNELS * sizeof(float));
        int w = sWarmupsRemaining.load();
        if (w > 0) sWarmupsRemaining.store(w - 1, std::memory_order_relaxed);
        return oboe::DataCallbackResult::Continue;
    }

    LockAudioDevice();
    AudioDevice::ProcessAudioMixing(data, numFrames * AUDIO_CHANNELS);
    UnlockAudioDevice();
    return oboe::DataCallbackResult::Continue;
}

void AudioDevice::onErrorBeforeClose(oboe::AudioStream* /*s*/, oboe::Result error)
{
    PrintLog(PRINT_NORMAL, "Oboe: onErrorBeforeClose(%s)", convertToText(error));
}

void AudioDevice::onErrorAfterClose(oboe::AudioStream* /*s*/, oboe::Result error)
{
    PrintLog(PRINT_NORMAL, "Oboe: onErrorAfterClose(%s) → scheduling restart", convertToText(error));
    sNeedsRestart.store(true, std::memory_order_relaxed);
    sErrorGen.fetch_add(1, std::memory_order_relaxed);
    // Back off a touch to avoid thrash while system routes devices on resume.
    sDebounceUntilMs.store(NowMs() + 250, std::memory_order_relaxed);
}

// ---- Recovery loop ----
void AudioDevice::EnsureAlive()
{
    const long long now = NowMs();

    // 1) Passive "stuck" detection based on callback activity.
    const long long lastCb = sLastCallbackMs.load(std::memory_order_relaxed);
    if (stream && lastCb > 0) {
        const long long dt = now - lastCb;
        if (dt > 2000) { // 2s without callbacks while stream exists → restart
            PrintLog(PRINT_NORMAL, "Oboe: stream stuck (> %lld ms) — scheduling restart", dt);
            sNeedsRestart.store(true, std::memory_order_relaxed);
            sErrorGen.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // 2) If marked, rebuild — but only when app/window/focus are settled and debounce has elapsed.
    if (!sNeedsRestart.load(std::memory_order_relaxed))
        return;

    // Deduplicate multiple callback bursts.
    const int gen = sErrorGen.load(std::memory_order_relaxed);
    if (gen == sLastHandledErrorGen)
        return;

    if (!sIsResumed.load(std::memory_order_relaxed)
        || !sHasWindow.load(std::memory_order_relaxed)
        || !sHasFocus.load(std::memory_order_relaxed)
        || now < sDebounceUntilMs.load(std::memory_order_relaxed))
    {
        // Keep waiting; will retry next frame.
        return;
    }

    sLastHandledErrorGen = gen;

    LockAudioDevice();
    PrintLog(PRINT_NORMAL, "Oboe: rebuilding audio stream (gen=%d)", gen);
    shutdownStream();
    if (createStream()) {
        sNeedsRestart.store(false, std::memory_order_relaxed);
        PrintLog(PRINT_NORMAL, "Oboe: audio stream restarted");
    } else {
        // Keep the flag set; we’ll retry next frame.
        PrintLog(PRINT_NORMAL, "Oboe: restart failed; will retry");
    }
    UnlockAudioDevice();
}

// ---- AudioDeviceBase hooks ----
void AudioDevice::InitAudioChannels()
{
    pthread_mutex_init(&mutex, NULL);
    AudioDeviceBase::InitAudioChannels();
}

} // namespace RSDK
