#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#endif

#include <xbox.h>

#include "../debug_mode.h"
#include "../kernel/guestcall.h"
#include "../kernel/heap.h"
#include "../kernel/klog.h"
#include "../kernel/memory.h"
#include "../cpu/guest_thread.h"
#include "xaudio.h"
#include "xma_decoder.h"

extern "C" void MojoRecompHostPollWaitUs(uint32_t microseconds);

namespace {

constexpr uint32_t kStatusSuccess = 0;
constexpr uint32_t kStatusInvalidParameter = 0xC000000Du;
constexpr uint32_t kStatusNoMemory = 0xC0000017u;
constexpr uint32_t kDriverHandle = 0x41550000u;
constexpr uint32_t kXmaMmioBase = 0x7FEA0000u;
constexpr uint32_t kXmaContextArrayRegister = kXmaMmioBase + 0x1800u;
constexpr uint32_t kXmaCurrentContextRegister = kXmaMmioBase + 0x1818u;
constexpr uint32_t kXmaNextContextRegister = kXmaMmioBase + 0x181Cu;
constexpr uint32_t kXmaKickBase = kXmaMmioBase + 0x1940u;
constexpr uint32_t kXmaLockBase = kXmaMmioBase + 0x1A40u;
constexpr uint32_t kXmaClearBase = kXmaMmioBase + 0x1A80u;
// The title also addresses the XMA register file through the physical MMIO
// alias at 0x1FFA0000. Register indices are byte-addressed here: 0x650 is
// kick, 0x690 lock and 0x6A0 clear (10 context groups each).
constexpr uint32_t kXmaPhysicalAliasBase = 0x1FFA0000u;
constexpr uint32_t kXmaAliasKickBase = kXmaPhysicalAliasBase + 0x0650u;
constexpr uint32_t kXmaAliasLockBase = kXmaPhysicalAliasBase + 0x0690u;
constexpr uint32_t kXmaAliasClearBase = kXmaPhysicalAliasBase + 0x06A0u;
constexpr uint32_t kXmaContextCount = 320;
constexpr uint32_t kXmaContextSize = 64;
constexpr uint32_t kRenderFrequency = 48000;
constexpr uint32_t kRenderChannels = 6;
constexpr uint32_t kChannelSamples = 256;
constexpr uint32_t kRenderFrameSamples = kRenderChannels * kChannelSamples;
constexpr uint32_t kOutputChannels = 2;
constexpr uint32_t kOutputFrameSamples = kOutputChannels * kChannelSamples;
constexpr uint32_t kAudioQueueDepth = 8;

std::atomic<uint32_t> g_clientCallback{0};
std::atomic<uint32_t> g_clientContext{0};
std::atomic<uint64_t> g_framesSubmitted{0};
std::atomic<uint64_t> g_audioConsumedFrames{0};
std::once_flag g_audioPumpOnce;
std::mutex g_xmaMutex;
std::unordered_map<uint32_t, void*> g_xmaContexts;
std::array<bool, kXmaContextCount> g_xmaAllocated{};
std::array<bool, kXmaContextCount> g_xmaEnabled{};
uint32_t g_xmaContextArrayGuest = 0;
std::once_flag g_xmaInitOnce;
std::atomic<uint32_t> g_mmioReports{0};
std::atomic<uint32_t> g_traceMmioAfterCreate{0};
std::atomic<uint32_t> g_xmaRoutineReports{0};
std::atomic<uint64_t> g_xmaKickCommands{0};
std::atomic<uint64_t> g_xmaLockCommands{0};
std::atomic<uint64_t> g_xmaClearCommands{0};

// The old ReXGlue backend queued eight 5.33 ms render frames. Its audio device
// released one semaphore token whenever a frame finished playing, and that token
// allowed the guest render callback to produce the next frame. Preserve that
// producer/consumer relationship here instead of free-running the callback from
// a host timer when a real output device is available.
std::mutex g_audioCreditMutex;
std::condition_variable g_audioCreditCv;
uint32_t g_audioCredits = 0;
std::atomic<bool> g_audioDevicePacing{false};

// F6 debug pause must freeze the entire audio timeline, not just mute it. A
// mute-only pause would still consume queued buffers / XMA data and leave the
// cutscene video behind the audio clock after resume.
std::atomic<bool> g_debugAudioPaused{false};
std::mutex g_debugAudioPauseMutex;
std::condition_variable g_debugAudioPauseCv;

#ifdef _WIN32
struct HostAudioFrame
{
    WAVEHDR header{};
    std::array<int16_t, kOutputFrameSamples> samples{};
};

std::once_flag g_hostAudioOnce;
std::atomic<bool> g_hostAudioReady{false};
HWAVEOUT g_waveOut = nullptr;
HANDLE g_waveEvent = nullptr;
std::mutex g_hostAudioMutex;
std::vector<std::unique_ptr<HostAudioFrame>> g_hostAudioFrames;
#endif

bool AudioPumpFixedPeriodEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_AUDIO_PUMP_FIXED_PERIOD");
        // Correct by default. Set MOJORECOMP_AUDIO_PUMP_FIXED_PERIOD=0 only for
        // regression comparisons with the old callback+sleep scheduling.
        return !value || !*value || value[0] != '0';
    }();
    return enabled;
}

bool AudioPumpTraceEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_AUDIO_PUMP_TRACE");
        return value && *value && value[0] != '0';
    }();
    return enabled;
}

bool HostAudioOutputAllowed()
{
#ifdef _WIN32
    const char* output = std::getenv("MOJORECOMP_AUDIO_OUTPUT");
    if (output && *output && output[0] == '0')
        return false;

    // Validation runs must not depend on a physical audio endpoint. A visible
    // run uses the real device by default; headless can opt in explicitly.
    const char* headless = std::getenv("MOJORECOMP_HEADLESS");
    const char* force = std::getenv("MOJORECOMP_AUDIO_FORCE_OUTPUT");
    if (headless && *headless && headless[0] != '0' &&
        !(force && *force && force[0] != '0'))
        return false;
    return true;
#else
    return false;
#endif
}

bool HostAudioTraceEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_AUDIO_OUTPUT_TRACE");
        return value && *value && value[0] != '0';
    }();
    return enabled;
}

bool HostAudioChannelTraceEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_AUDIO_CHANNEL_TRACE");
        return value && *value && value[0] != '0';
    }();
    return enabled;
}

bool HostAudioCenterOnlyEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_AUDIO_CENTER_ONLY");
        return value && *value && value[0] != '0';
    }();
    return enabled;
}

bool XmaDiagnosticsEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_XMA_DIAGNOSTICS");
        return value && *value && value[0] != '0';
    }();
    return enabled;
}

void ResetAudioCredits()
{
    {
        std::lock_guard lock(g_audioCreditMutex);
        g_audioCredits = kAudioQueueDepth;
    }
    g_audioCreditCv.notify_all();
}

void GrantAudioCredit()
{
    {
        std::lock_guard lock(g_audioCreditMutex);
        if (g_audioCredits < kAudioQueueDepth)
            ++g_audioCredits;
    }
    g_audioCreditCv.notify_one();
}

bool ConsumeAudioCredit()
{
    std::unique_lock lock(g_audioCreditMutex);
    g_audioCreditCv.wait_for(lock, std::chrono::milliseconds(250), [] {
        return g_audioCredits != 0 ||
               g_clientCallback.load(std::memory_order_acquire) == 0 ||
               g_debugAudioPaused.load(std::memory_order_acquire);
    });
    if (g_clientCallback.load(std::memory_order_acquire) == 0)
        return false;
    if (g_debugAudioPaused.load(std::memory_order_acquire))
        return false;
    if (!g_audioCredits)
        return false;
    --g_audioCredits;
    return true;
}

void WaitForDebugAudioResume()
{
    std::unique_lock lock(g_debugAudioPauseMutex);
    g_debugAudioPauseCv.wait(lock, [] {
        return !g_debugAudioPaused.load(std::memory_order_acquire);
    });
}

float GuestRenderSample(const be<uint32_t>* frame, uint32_t channel, uint32_t sample)
{
    const uint32_t bits = frame[channel * kChannelSamples + sample];
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value) ? value : 0.0f;
}

int16_t FloatToPcm16(float value)
{
    value = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lrintf(value * 32767.0f));
}

#ifdef _WIN32
void HostAudioReaperThread()
{
    for (;;)
    {
        if (!g_waveEvent)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        WaitForSingleObject(g_waveEvent, 1000);
        std::vector<std::unique_ptr<HostAudioFrame>> completed;
        {
            std::lock_guard lock(g_hostAudioMutex);
            for (auto it = g_hostAudioFrames.begin(); it != g_hostAudioFrames.end();)
            {
                if (((*it)->header.dwFlags & WHDR_DONE) == 0)
                {
                    ++it;
                    continue;
                }
                completed.push_back(std::move(*it));
                it = g_hostAudioFrames.erase(it);
            }
        }

        for (auto& frame : completed)
        {
            if (g_waveOut)
                waveOutUnprepareHeader(g_waveOut, &frame->header, sizeof(frame->header));
            const uint64_t consumedFrames =
                g_audioConsumedFrames.fetch_add(1, std::memory_order_relaxed) + 1;
            if (HostAudioTraceEnabled() &&
                (consumedFrames <= 8 || (consumedFrames % 188u) == 0))
            {
                const double hostSeconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                const uint64_t consumedSamples = consumedFrames * kChannelSamples;
                const double audioSeconds = double(consumedSamples) / double(kRenderFrequency);
                KLOG("XAudio clock host=%.6f frames=%llu samples=%llu audio=%.6f\n",
                     hostSeconds,
                     static_cast<unsigned long long>(consumedFrames),
                     static_cast<unsigned long long>(consumedSamples),
                     audioSeconds);
            }
            GrantAudioCredit();
        }
    }
}

bool EnsureHostAudioOutput()
{
    if (!HostAudioOutputAllowed())
        return false;

    std::call_once(g_hostAudioOnce, [] {
        g_waveEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_waveEvent)
        {
            KLOG("XAudio host output: CreateEvent failed (%lu)\n", GetLastError());
            return;
        }

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = kOutputChannels;
        format.nSamplesPerSec = kRenderFrequency;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        const MMRESULT result = waveOutOpen(
            &g_waveOut, WAVE_MAPPER, &format,
            reinterpret_cast<DWORD_PTR>(g_waveEvent), 0, CALLBACK_EVENT);
        if (result != MMSYSERR_NOERROR)
        {
            KLOG("XAudio host output: waveOutOpen failed (%u); using timer pacing\n",
                 static_cast<unsigned>(result));
            CloseHandle(g_waveEvent);
            g_waveEvent = nullptr;
            g_waveOut = nullptr;
            return;
        }

        g_hostAudioReady.store(true, std::memory_order_release);
        std::thread(HostAudioReaperThread).detach();
        KLOG("XAudio host output: WinMM stereo PCM16 %u Hz, guest 6ch x %u samples, queue=%u\n",
             kRenderFrequency, kChannelSamples, kAudioQueueDepth);
    });

    return g_hostAudioReady.load(std::memory_order_acquire);
}

bool SubmitHostAudioFrame(const be<uint32_t>* frame)
{
    if (!frame || !g_hostAudioReady.load(std::memory_order_acquire) || !g_waveOut)
        return false;

    auto output = std::make_unique<HostAudioFrame>();
    // XAudio's render callback is normally paced by the physical 48 kHz
    // endpoint: one 256-sample buffer takes ~5.33 ms to play, and completion
    // releases the credit for the next guest callback. That is also the
    // cutscene audio/master clock in Crash. During F3 turbo, compress each
    // guest render quantum 4:1 so the host buffer lasts ~1.33 ms instead. The
    // completed-buffer credit then advances the guest audio timeline at the
    // same 4x rate as the debug guest clock and vblank pump. At 1x this path is
    // bit-for-bit the same length/cadence as before.
    const uint32_t speedDivisor =
        (mojorecomp::debug::FastForward() && !mojorecomp::debug::Paused()) ? 4u : 1u;
    const uint32_t outputSamplesPerChannel = kChannelSamples / speedDivisor;
    const uint32_t outputFrameSamples = outputSamplesPerChannel * kOutputChannels;
    constexpr float kCenter = 0.70710678f;
    constexpr float kSurround = 0.70710678f;
    constexpr float kLfe = 0.0f;
    constexpr float kScale = 0.58578644f;
    std::array<float, kChannelSamples> subtitleCenter{};
    for (uint32_t sample = 0; sample < kChannelSamples; ++sample)
        subtitleCenter[sample] = GuestRenderSample(frame, 2, sample);
    mojorecomp::audio::XmaDecoderObserveRenderCenter(
        subtitleCenter.data(), kChannelSamples, kRenderFrequency);

    float peak = 0.0f;
    uint32_t nonZero = 0;
    std::array<double, kRenderChannels> sumSquares{};
    std::array<float, kRenderChannels> channelPeak{};
    const bool centerOnly = HostAudioCenterOnlyEnabled();

    for (uint32_t sample = 0; sample < outputSamplesPerChannel; ++sample)
    {
        // Box-filter the source block before decimation. This keeps 4x audio
        // intelligible and avoids the worst aliasing while still deliberately
        // raising pitch/tempo like an emulator fast-forward.
        float averaged[kRenderChannels]{};
        const uint32_t sourceBase = sample * speedDivisor;
        for (uint32_t offset = 0; offset < speedDivisor; ++offset)
        {
            for (uint32_t channel = 0; channel < kRenderChannels; ++channel)
                averaged[channel] += GuestRenderSample(frame, channel, sourceBase + offset);
        }
        const float invDivisor = 1.0f / float(speedDivisor);
        for (float& value : averaged)
            value *= invDivisor;

        const float fl = averaged[0];
        const float fr = averaged[1];
        const float fc = averaged[2];
        const float lf = averaged[3];
        const float bl = averaged[4];
        const float br = averaged[5];
        const float channels[kRenderChannels] = {fl, fr, fc, lf, bl, br};
        for (uint32_t channel = 0; channel < kRenderChannels; ++channel)
        {
            const float value = channels[channel];
            sumSquares[channel] += double(value) * double(value);
            channelPeak[channel] = std::max(channelPeak[channel], std::abs(value));
        }

        float left = 0.0f;
        float right = 0.0f;
        if (centerOnly)
        {
            left = right = fc;
        }
        else
        {
            const float mid = fc * kCenter + lf * kLfe;
            left = (fl + mid + bl * kSurround) * kScale;
            right = (fr + mid + br * kSurround) * kScale;
        }
        peak = std::max(peak, std::max(std::abs(left), std::abs(right)));
        const int16_t leftPcm = FloatToPcm16(left);
        const int16_t rightPcm = FloatToPcm16(right);
        output->samples[sample * 2 + 0] = leftPcm;
        output->samples[sample * 2 + 1] = rightPcm;
        nonZero += leftPcm != 0;
        nonZero += rightPcm != 0;
    }

    if (HostAudioTraceEnabled())
    {
        static std::atomic<uint64_t> sequence{0};
        static std::atomic<bool> audibleReported{false};
        const uint64_t n = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        const bool firstAudible = nonZero && !audibleReported.exchange(true, std::memory_order_relaxed);
        if (n <= 16 || (n & 0x1FFu) == 0 || firstAudible)
            KLOG("XAudio output frame #%llu peak=%.6f nonzero=%u/%u speed=%ux samples=%u\n",
                 static_cast<unsigned long long>(n), double(peak), nonZero,
                 outputFrameSamples, speedDivisor, outputSamplesPerChannel);
    }

    if (HostAudioChannelTraceEnabled())
    {
        static std::atomic<uint64_t> channelSequence{0};
        const uint64_t n = channelSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        // 48000 / 256 = 187.5 render frames per second. Log approximately once
        // per second so cutscene channel balance can be inspected without
        // perturbing the audio thread with per-frame I/O.
        if (n <= 8 || (n % 188u) == 0)
        {
            std::array<double, kRenderChannels> rms{};
            for (uint32_t channel = 0; channel < kRenderChannels; ++channel)
                rms[channel] = std::sqrt(sumSquares[channel] /
                                         double(outputSamplesPerChannel));
            KLOG("XAudio 5.1 frame #%llu "
                 "FL{rms=%.6f peak=%.6f} FR{rms=%.6f peak=%.6f} "
                 "FC{rms=%.6f peak=%.6f} LFE{rms=%.6f peak=%.6f} "
                 "BL{rms=%.6f peak=%.6f} BR{rms=%.6f peak=%.6f} center_only=%u\n",
                 static_cast<unsigned long long>(n),
                 rms[0], double(channelPeak[0]), rms[1], double(channelPeak[1]),
                 rms[2], double(channelPeak[2]), rms[3], double(channelPeak[3]),
                 rms[4], double(channelPeak[4]), rms[5], double(channelPeak[5]),
                 centerOnly ? 1u : 0u);
        }
    }

    output->header.lpData = reinterpret_cast<LPSTR>(output->samples.data());
    output->header.dwBufferLength = static_cast<DWORD>(outputFrameSamples * sizeof(int16_t));
    output->header.dwFlags = 0;
    output->header.dwLoops = 0;

    MMRESULT result = waveOutPrepareHeader(g_waveOut, &output->header, sizeof(output->header));
    if (result != MMSYSERR_NOERROR)
    {
        KLOG("XAudio host output: waveOutPrepareHeader failed (%u)\n",
             static_cast<unsigned>(result));
        return false;
    }

    HostAudioFrame* raw = output.get();
    {
        std::lock_guard lock(g_hostAudioMutex);
        g_hostAudioFrames.push_back(std::move(output));
        result = waveOutWrite(g_waveOut, &raw->header, sizeof(raw->header));
        if (result != MMSYSERR_NOERROR)
        {
            for (auto it = g_hostAudioFrames.begin(); it != g_hostAudioFrames.end(); ++it)
            {
                if (it->get() == raw)
                {
                    auto failed = std::move(*it);
                    g_hostAudioFrames.erase(it);
                    waveOutUnprepareHeader(g_waveOut, &failed->header, sizeof(failed->header));
                    break;
                }
            }
        }
    }

    if (result != MMSYSERR_NOERROR)
    {
        KLOG("XAudio host output: waveOutWrite failed (%u)\n",
             static_cast<unsigned>(result));
        return false;
    }
    return true;
}
#else
bool EnsureHostAudioOutput() { return false; }
bool SubmitHostAudioFrame(const be<uint32_t>*) { return false; }
#endif

uint32_t& XmaMmioRaw(uint32_t address)
{
    return *reinterpret_cast<uint32_t*>(g_guestMemory.Translate(address));
}

uint32_t XmaContextLoadWord(uint32_t contextId, uint32_t word);

void XmaDeviceThread()
{
    uint32_t current = 1;
    uint64_t sweepCount = 0;
    std::array<bool, kXmaContextCount> reportedWorkingContext{};
    std::array<uint64_t, kXmaContextCount> reportedConfigSignature{};
    uint32_t configReports = 0;
    for (;;)
    {
        if (g_debugAudioPaused.load(std::memory_order_acquire))
        {
            WaitForDebugAudioResume();
            continue;
        }

        bool didWork = false;

        // ReXGlue's worker sweeps all contexts, but XmaContext::Work disables a
        // context at the beginning of each work pass. A kick therefore grants
        // exactly one complete pass; it does not leave the decoder permanently
        // active. XmaDecoderWork now drains that full pass internally.
        for (uint32_t contextId = 0; contextId < kXmaContextCount; ++contextId)
        {
            bool worked = false;
            {
                // MMIO lock/clear commands use the same mutex. Holding it across
                // one decoder work item gives XMADisableContext the old runtime's
                // Block(false) semantics: once the lock command returns, no host
                // decode is still modifying that guest context.
                std::lock_guard guard(g_xmaMutex);
                if (!g_xmaEnabled[contextId])
                    continue;

                g_xmaEnabled[contextId] = false;

                XmaMmioRaw(kXmaCurrentContextRegister) = contextId;
                XmaMmioRaw(kXmaNextContextRegister) = (contextId + 1) % kXmaContextCount;
                const uint32_t contextGuest =
                    g_xmaContextArrayGuest + contextId * kXmaContextSize;
                worked = mojorecomp::audio::XmaDecoderWork(contextId, contextGuest);
                if (worked && XmaDiagnosticsEnabled() && !reportedWorkingContext[contextId])
                {
                    reportedWorkingContext[contextId] = true;
                    const uint32_t d1 = XmaContextLoadWord(contextId, 1);
                    static constexpr uint32_t sampleRates[4] = {24000, 32000, 44100, 48000};
                    const uint32_t rateId = (d1 >> 27) & 3u;
                    const uint32_t stereo = (d1 >> 29) & 1u;
                    const uint32_t subframes = (d1 >> 20) & 0xFu;
                    KLOG("XMA worker first-work context=%u rate=%u channels=%u subframes=%u\n",
                         contextId, sampleRates[rateId], stereo ? 2u : 1u, subframes);
                }
                if (worked && XmaDiagnosticsEnabled() && configReports < 128)
                {
                    const uint32_t d0 = XmaContextLoadWord(contextId, 0);
                    const uint32_t d1 = XmaContextLoadWord(contextId, 1);
                    const uint32_t d3 = XmaContextLoadWord(contextId, 3);
                    const uint32_t d4 = XmaContextLoadWord(contextId, 4);
                    const uint64_t signature =
                        (uint64_t(d0 & 0x000FF000u) << 32) |
                        uint64_t(d1 & 0x3FFFF000u) ^
                        (uint64_t(d3 & 0x03FFFFFFu) << 6) ^
                        uint64_t(d4 & 0x03FFFFFFu);
                    if (reportedConfigSignature[contextId] != signature)
                    {
                        reportedConfigSignature[contextId] = signature;
                        ++configReports;
                        static constexpr uint32_t sampleRates[4] = {24000, 32000, 44100, 48000};
                        const uint32_t rateId = (d1 >> 27) & 3u;
                        KLOG("XMA config context=%u rate=%u channels=%u sdc=%u pad=%u loops=%u loop_start=%u loop_end=%u loop_skip=%u loop_subend=%u\n",
                             contextId, sampleRates[rateId], ((d1 >> 29) & 1u) ? 2u : 1u,
                             (d1 >> 20) & 0xFu, (d1 >> 24) & 7u,
                             (d0 >> 12) & 0xFFu, d3 & 0x03FFFFFFu,
                             d4 & 0x03FFFFFFu, (d1 >> 17) & 7u,
                             (d1 >> 12) & 3u);
                    }
                }
            }
            didWork = didWork || worked;
        }

        // Even with no enabled/ready context, keep the hardware progress indices
        // rotating because the title polls them while acquiring XMA locks.
        if (!didWork)
        {
            XmaMmioRaw(kXmaCurrentContextRegister) = current;
            current = (current + 1) % kXmaContextCount;
            XmaMmioRaw(kXmaNextContextRegister) = current;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        else if ((++sweepCount & 0x3Fu) == 0)
            std::this_thread::yield();
    }
}

uint32_t XmaContextLoadWord(uint32_t contextId, uint32_t word)
{
    auto* words = reinterpret_cast<const uint32_t*>(
        g_guestMemory.Translate(g_xmaContextArrayGuest + contextId * kXmaContextSize));
    return _byteswap_ulong(words[word]);
}

void XmaContextStoreWord(uint32_t contextId, uint32_t word, uint32_t value)
{
    auto* words = reinterpret_cast<uint32_t*>(
        g_guestMemory.Translate(g_xmaContextArrayGuest + contextId * kXmaContextSize));
    words[word] = _byteswap_ulong(value);
}

void XmaClearContextLocked(uint32_t contextId)
{
    uint32_t d0 = XmaContextLoadWord(contextId, 0);
    uint32_t d1 = XmaContextLoadWord(contextId, 1);
    uint32_t d2 = XmaContextLoadWord(contextId, 2);
    uint32_t d9 = XmaContextLoadWord(contextId, 9);

    // Match Xenia/ReXGlue XmaContext::ClearLocked: invalidate both input
    // buffers and the output buffer, restart after the 32-bit packet header,
    // and reset the output ring offsets. Preserve stream configuration fields.
    d0 &= ~((1u << 20) | (1u << 21));
    d0 &= ~(0x1Fu << 27);
    d1 &= ~(1u << 31);
    d2 = (d2 & ~0x03FFFFFFu) | 32u;
    d9 &= ~0x1Fu;

    XmaContextStoreWord(contextId, 0, d0);
    XmaContextStoreWord(contextId, 1, d1);
    XmaContextStoreWord(contextId, 2, d2);
    XmaContextStoreWord(contextId, 9, d9);
    g_xmaEnabled[contextId] = false;
}

void ApplyXmaCommand(bool kick, bool lock, bool clear, uint32_t group, uint32_t mask)
{
    static std::atomic<uint32_t> detailedKickReports{0};
    std::lock_guard guard(g_xmaMutex);
    const uint32_t first = group * 32u;
    for (uint32_t bit = 0; bit < 32u && first + bit < kXmaContextCount; ++bit)
    {
        if ((mask & (1u << bit)) == 0)
            continue;
        const uint32_t contextId = first + bit;
        if (clear)
        {
            XmaClearContextLocked(contextId);
            mojorecomp::audio::XmaDecoderReset(contextId);
        }
        else if (lock)
            g_xmaEnabled[contextId] = false;
        else if (kick)
        {
            g_xmaEnabled[contextId] = true;
            // While F6 is paused, remember the kick but do not advance decoder
            // state. The worker above will service the enabled context after
            // resume. This avoids consuming compressed cutscene audio while the
            // guest/video timeline is frozen.
            if (g_debugAudioPaused.load(std::memory_order_acquire))
                continue;
            const uint32_t contextGuest = g_xmaContextArrayGuest + contextId * kXmaContextSize;
            uint32_t pre[10]{};
            const bool detailedCandidate = XmaDiagnosticsEnabled() && contextId < 3;
            if (detailedCandidate)
            {
                for (uint32_t word = 0; word < 10; ++word)
                    pre[word] = XmaContextLoadWord(contextId, word);
            }
            // Frontend audio can issue hundreds of kicks before gameplay. Only
            // spend the detailed trace budget on the streamed cutscene blocks;
            // these live in the later physical streaming arena (0x0Fxxxxxx in
            // Crash OTT) and use contexts 0..2 together.
            const uint32_t preIn0 = pre[5] & 0x1FFFFFFFu;
            const uint32_t preIn1 = pre[6] & 0x1FFFFFFFu;
            const bool detailedReport = detailedCandidate &&
                (preIn0 >= 0x0F000000u || preIn1 >= 0x0F000000u) &&
                detailedKickReports.load(std::memory_order_relaxed) < 512;
            static std::atomic<uint32_t> decodeReports{0};
            const bool worked = mojorecomp::audio::XmaDecoderWork(contextId, contextGuest);
            // XmaContext::Work in ReXGlue consumes the enable at entry. Mirror
            // that one-shot behavior for the inline low-latency kick path.
            g_xmaEnabled[contextId] = false;
            const uint32_t report = decodeReports.fetch_add(1, std::memory_order_relaxed);
            if (report < 24)
                KLOG("XMA decode kick context=%u worked=%u backend=%u\n",
                     contextId, worked ? 1u : 0u,
                     mojorecomp::audio::XmaDecoderAvailable() ? 1u : 0u);
            if (detailedReport)
            {
                const uint32_t detail = detailedKickReports.fetch_add(1, std::memory_order_relaxed);
                if (detail < 512)
                {
                    uint32_t post[10]{};
                    for (uint32_t word = 0; word < 10; ++word)
                        post[word] = XmaContextLoadWord(contextId, word);
                    const uint32_t preRate = (pre[1] >> 27) & 3u;
                    const uint32_t postRate = (post[1] >> 27) & 3u;
                    static constexpr uint32_t sampleRates[4] = {24000, 32000, 44100, 48000};
                    KLOG("XMA kick-state n=%u ctx=%u worked=%u "
                         "pre{valid=%u%u cur=%u p0=%u p1=%u read=%u rate=%u ch=%u sdc=%u outv=%u oblocks=%u ow=%u or=%u in0=%08X in1=%08X out=%08X} "
                         "post{valid=%u%u cur=%u read=%u rate=%u ch=%u outv=%u ow=%u or=%u err=%u}\n",
                         detail + 1, contextId, worked ? 1u : 0u,
                         (pre[0] >> 20) & 1u, (pre[0] >> 21) & 1u,
                         pre[4] >> 31, pre[0] & 0xFFFu, pre[1] & 0xFFFu,
                         pre[2] & 0x03FFFFFFu, sampleRates[preRate],
                         ((pre[1] >> 29) & 1u) ? 2u : 1u, (pre[1] >> 20) & 0xFu,
                         pre[1] >> 31, (pre[0] >> 22) & 0x1Fu,
                         (pre[0] >> 27) & 0x1Fu, pre[9] & 0x1Fu,
                         pre[5] & 0x1FFFFFFFu, pre[6] & 0x1FFFFFFFu,
                         pre[7] & 0x1FFFFFFFu,
                         (post[0] >> 20) & 1u, (post[0] >> 21) & 1u,
                         post[4] >> 31, post[2] & 0x03FFFFFFu,
                         sampleRates[postRate], ((post[1] >> 29) & 1u) ? 2u : 1u,
                         post[1] >> 31, (post[0] >> 27) & 0x1Fu,
                         post[9] & 0x1Fu, (post[2] >> 26) & 0x1Fu);
                }
            }
        }
    }
    if (kick) g_xmaKickCommands.fetch_add(1, std::memory_order_relaxed);
    if (lock) g_xmaLockCommands.fetch_add(1, std::memory_order_relaxed);
    if (clear) g_xmaClearCommands.fetch_add(1, std::memory_order_relaxed);
}

void EnsureXmaDevice()
{
    std::call_once(g_xmaInitOnce, [] {
        void* block = g_guestHeap.AllocPhysical(
            kXmaContextCount * kXmaContextSize, 256);
        if (!block)
        {
            KLOG("XMA device: failed to allocate context array\n");
            return;
        }

        g_xmaContextArrayGuest = g_guestMemory.MapVirtual(block);
        std::memset(block, 0, kXmaContextCount * kXmaContextSize);
        const uint32_t physical = g_xmaContextArrayGuest & 0x1FFFFFFFu;

        // XMA MMIO is little-endian relative to the PPC. The translated title
        // uses lwbrx/stwbrx here, so store logical register values as raw host
        // little-endian words rather than through PPC_STORE_U32.
        XmaMmioRaw(kXmaContextArrayRegister) = physical;
        XmaMmioRaw(kXmaCurrentContextRegister) = 1;
        XmaMmioRaw(kXmaNextContextRegister) = 1;
        KLOG("XMA device: contexts=%08X phys=%08X count=%u\n",
             g_xmaContextArrayGuest, physical, kXmaContextCount);

        std::thread(XmaDeviceThread).detach();
    });
}

void EnsureAudioCallbackPump()
{
    std::call_once(g_audioPumpOnce, [] {
        std::thread([] {
            // XAudio passes a pointer to a one-word wrapper containing the title's
            // registration argument, not the argument value itself. This matches
            // the Xbox/ReXGlue render-driver worker ABI.
            void* wrapperHost = g_guestHeap.Alloc(4, 4);
            if (!wrapperHost)
            {
                KLOG("XAudio worker: failed to allocate callback wrapper\n");
                return;
            }
            const uint32_t wrapperGuest = g_guestMemory.MapVirtual(wrapperHost);
            auto* thread = new GuestThreadContext(3, 0x40000, 64, 0xFA0u);
            uint32_t reports = 0;
            uint64_t pumpCalls = 0;
            bool haveDeadline = false;
            auto nextDeadline = std::chrono::steady_clock::now();
            auto previousStart = nextDeadline;

            for (;;)
            {
                if (g_debugAudioPaused.load(std::memory_order_acquire))
                {
                    // Reset timer pacing after resume so the worker does not try
                    // to repay the wall-clock time spent paused as a burst of
                    // catch-up callbacks.
                    haveDeadline = false;
                    WaitForDebugAudioResume();
                    previousStart = std::chrono::steady_clock::now();
                    continue;
                }

                const uint32_t callback = g_clientCallback.load(std::memory_order_acquire);
                const uint32_t context = g_clientContext.load(std::memory_order_relaxed);
                if (!callback)
                {
                    haveDeadline = false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }

                PPCFunc* host = g_guestMemory.FindFunction(callback);
                if (!host)
                {
                    haveDeadline = false;
                    if (!reports++)
                        KLOG("XAudio worker: callback %08X is not translated\n", callback);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }

                const bool devicePaced = g_audioDevicePacing.load(std::memory_order_acquire);
                if (devicePaced)
                {
                    haveDeadline = false;
                    if (!ConsumeAudioCredit())
                        continue;
                    // F6 may have been pressed while waiting for a device credit.
                    // Return the credit and stop before dispatching another guest
                    // render callback so audio state cannot advance while paused.
                    if (g_debugAudioPaused.load(std::memory_order_acquire))
                    {
                        GrantAudioCredit();
                        continue;
                    }
                }

                *reinterpret_cast<be<uint32_t>*>(wrapperHost) = context;
                thread->ppc.r3.u64 = wrapperGuest;
                thread->ppc.lr = 0;
                g_ppcContext = &thread->ppc;
                if (reports < 8)
                {
                    KLOG("XAudio worker: dispatch callback=%08X wrapper=%08X context=%08X\n",
                         callback, wrapperGuest, context);
                    ++reports;
                }

                const auto callbackStart = std::chrono::steady_clock::now();
                try
                {
                    host(thread->ppc, g_guestMemory.base);
                }
                catch (const GuestThreadExit& exit)
                {
                    KLOG("XAudio worker: guest callback thread exit %08X\n", exit.code);
                    return;
                }

                const auto callbackEnd = std::chrono::steady_clock::now();
                ++pumpCalls;
                if (AudioPumpTraceEnabled() &&
                    (pumpCalls <= 16 || (pumpCalls % 256u) == 0))
                {
                    const auto callbackUs = std::chrono::duration_cast<std::chrono::microseconds>(
                        callbackEnd - callbackStart).count();
                    const auto periodUs = std::chrono::duration_cast<std::chrono::microseconds>(
                        callbackStart - previousStart).count();
                    KLOG("XAudio pump #%llu callback_us=%lld period_us=%lld fixed=%u\n",
                         static_cast<unsigned long long>(pumpCalls),
                         static_cast<long long>(callbackUs),
                         static_cast<long long>(periodUs),
                         AudioPumpFixedPeriodEnabled() ? 1u : 0u);
                }
                previousStart = callbackStart;

                // With a real host endpoint, the old ReXGlue/Xenia behavior was
                // consumption-driven: finishing one 256-sample device buffer
                // releases the next guest render callback. Don't add a second
                // timer on top of that device clock.
                if (devicePaced)
                    continue;

                // 48 kHz / 256 samples is the native 360 render quantum (~5.33 ms).
                constexpr auto kRenderQuantum = std::chrono::microseconds(5333);
                if (!AudioPumpFixedPeriodEnabled())
                {
                    std::this_thread::sleep_for(kRenderQuantum);
                    continue;
                }

                // Keep callback starts on a fixed 5.33 ms cadence. A full sleep
                // after guest work makes callback execution time part of the
                // render period and slows the title's audio-derived master clock.
                if (!haveDeadline)
                {
                    nextDeadline = callbackStart + kRenderQuantum;
                    haveDeadline = true;
                }
                else
                {
                    nextDeadline += kRenderQuantum;
                }

                const auto now = std::chrono::steady_clock::now();
                if (now < nextDeadline)
                {
                    const auto remainingUs = std::chrono::duration_cast<std::chrono::microseconds>(
                        nextDeadline - now).count();
                    if (remainingUs > 0)
                        MojoRecompHostPollWaitUs(static_cast<uint32_t>(remainingUs));
                }
                else if (now - nextDeadline > std::chrono::microseconds(21332))
                {
                    // Reset after an exceptional stall instead of building an
                    // unbounded catch-up queue.
                    nextDeadline = now;
                }
            }
        }).detach();
    });
}

uint32_t XAudioRegisterRenderDriverClient_x(be<uint32_t>* callbackPair,
                                            be<uint32_t>* driverHandle)
{
    if (!callbackPair || !driverHandle)
        return kStatusInvalidParameter;

    const uint32_t callback = callbackPair[0];
    const uint32_t context = callbackPair[1];
    *driverHandle = kDriverHandle;
    const bool devicePaced = EnsureHostAudioOutput();
    g_audioDevicePacing.store(devicePaced, std::memory_order_release);
    if (devicePaced)
        ResetAudioCredits();
    g_clientContext.store(context, std::memory_order_relaxed);
    g_clientCallback.store(callback, std::memory_order_release);
    KLOG("XAudioRegisterRenderDriverClient callback=%08X context=%08X -> %08X device_paced=%u\n",
         callback, context, kDriverHandle, devicePaced ? 1u : 0u);
    EnsureAudioCallbackPump();
    return kStatusSuccess;
}

uint32_t XAudioUnregisterRenderDriverClient_x(uint32_t handle)
{
    KLOG("XAudioUnregisterRenderDriverClient(%08X) frames=%llu\n", handle,
         static_cast<unsigned long long>(g_framesSubmitted.load()));
    g_clientCallback.store(0, std::memory_order_release);
    g_clientContext.store(0, std::memory_order_relaxed);
    g_audioDevicePacing.store(false, std::memory_order_release);
    g_audioCreditCv.notify_all();
    return kStatusSuccess;
}

uint32_t XAudioSubmitRenderDriverFrame_x(uint32_t handle, be<uint32_t>* frame)
{
    const uint64_t submitted = g_framesSubmitted.fetch_add(1, std::memory_order_relaxed) + 1;
    if (submitted <= 8)
        KLOG("XAudioSubmitRenderDriverFrame #%llu handle=%08X frame=%08X\n",
             static_cast<unsigned long long>(submitted), handle,
             frame ? g_guestMemory.MapVirtual(frame) : 0u);

    if (g_audioDevicePacing.load(std::memory_order_acquire))
    {
        if (!SubmitHostAudioFrame(frame))
        {
            // A consumed producer credit must always be returned if the host
            // backend rejects the corresponding frame, otherwise eight such
            // failures would permanently stall the guest audio worker.
            GrantAudioCredit();
        }
    }
    return kStatusSuccess;
}

uint32_t XAudioGetVoiceCategoryVolumeChangeMask_x(uint32_t driver,
                                                  be<uint32_t>* maskOut)
{
    (void)driver;
    if (!maskOut)
        return kStatusInvalidParameter;
    *maskOut = 0;
    return kStatusSuccess;
}

uint32_t XAudioGetVoiceCategoryVolume_x(uint32_t category, be<uint32_t>* volumeOut)
{
    (void)category;
    if (!volumeOut)
        return kStatusInvalidParameter;
    const float one = 1.0f;
    uint32_t bits = 0;
    std::memcpy(&bits, &one, sizeof(bits));
    *volumeOut = bits;
    return kStatusSuccess;
}

uint32_t XMACreateContext_x(be<uint32_t>* contextOut)
{
    if (!contextOut)
        return kStatusInvalidParameter;

    EnsureXmaDevice();
    if (!g_xmaContextArrayGuest)
    {
        *contextOut = 0;
        return kStatusNoMemory;
    }

    uint32_t guest = 0;
    void* host = nullptr;
    {
        std::lock_guard lock(g_xmaMutex);
        uint32_t index = 0;
        while (index < kXmaContextCount && g_xmaAllocated[index])
            ++index;
        if (index == kXmaContextCount)
        {
            *contextOut = 0;
            return kStatusNoMemory;
        }
        g_xmaAllocated[index] = true;
        guest = g_xmaContextArrayGuest + index * kXmaContextSize;
        host = g_guestMemory.Translate(guest);
        std::memset(host, 0, kXmaContextSize);
        g_xmaContexts.emplace(guest, host);
    }
    *contextOut = guest;
    g_traceMmioAfterCreate.store(8, std::memory_order_release);
    KLOG_DIAG("XMACreateContext -> %08X slot=%u\n", guest,
              (guest - g_xmaContextArrayGuest) / kXmaContextSize);
    return kStatusSuccess;
}

void XMAReleaseContext_x(uint32_t context)
{
    if (!context)
        return;
    void* host = nullptr;
    static std::atomic<uint32_t> releaseReports{0};
    {
        std::lock_guard lock(g_xmaMutex);
        auto it = g_xmaContexts.find(context);
        if (it == g_xmaContexts.end())
        {
            KLOG("XMAReleaseContext unknown=%08X\n", context);
            return;
        }
        host = it->second;
        g_xmaContexts.erase(it);
        const uint32_t index = (context - g_xmaContextArrayGuest) / kXmaContextSize;
        if (index < kXmaContextCount)
        {
            g_xmaAllocated[index] = false;
            g_xmaEnabled[index] = false;
            mojorecomp::audio::XmaDecoderRelease(index);
        }
        const uint32_t report = releaseReports.fetch_add(1, std::memory_order_relaxed);
        if (report < 12)
        {
            const auto* words = static_cast<const uint32_t*>(host);
            KLOG("XMA release state slot=%u raw=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X "
                 "%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X current=%08X next=%08X\n",
                 index,
                 words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7],
                 words[8], words[9], words[10], words[11], words[12], words[13], words[14], words[15],
                 XmaMmioRaw(kXmaCurrentContextRegister), XmaMmioRaw(kXmaNextContextRegister));
        }
        KLOG_DIAG("XMAReleaseContext %08X slot=%u\n", context, index);
    }
    std::memset(host, 0, kXmaContextSize);
}

} // namespace

namespace mojorecomp::audio {

void SetDebugPaused(bool paused) noexcept
{
    const bool previous = g_debugAudioPaused.exchange(paused, std::memory_order_acq_rel);
    if (previous == paused)
        return;

#ifdef _WIN32
    // waveOutPause preserves the current sample position and all queued headers;
    // waveOutRestart continues from that exact point. Do not reset/flush here.
    if (g_hostAudioReady.load(std::memory_order_acquire) && g_waveOut)
    {
        const MMRESULT result = paused ? waveOutPause(g_waveOut) : waveOutRestart(g_waveOut);
        if (result != MMSYSERR_NOERROR)
        {
            KLOG("XAudio debug pause: waveOut%s failed (%u)\n",
                 paused ? "Pause" : "Restart", static_cast<unsigned>(result));
        }
    }
#endif

    // Wake both pacing paths so they immediately observe the new pause state.
    g_audioCreditCv.notify_all();
    g_debugAudioPauseCv.notify_all();
    KLOG("XAudio debug pause=%u\n", paused ? 1u : 0u);
}

} // namespace mojorecomp::audio

extern "C" double MojoRecompHostAudioSeconds()
{
    return double(g_audioConsumedFrames.load(std::memory_order_relaxed) * kChannelSamples) /
           double(kRenderFrequency);
}

extern "C" void MojoRecompEnsureXmaDevice()
{
    EnsureXmaDevice();
}

extern "C" void MojoRecompMmioStoreU32(uint8_t* base, uint32_t address, uint32_t value)
{
    // Preserve the previous PPC_MM_STORE_U32 byte semantics exactly: PPC stores
    // are big-endian, so the host backing receives a byte-swapped logical value.
    const uint32_t raw = _byteswap_ulong(value);
    *reinterpret_cast<volatile uint32_t*>(base + address) = raw;

    uint32_t trace = g_traceMmioAfterCreate.load(std::memory_order_acquire);
    while (trace && !g_traceMmioAfterCreate.compare_exchange_weak(
                        trace, trace - 1, std::memory_order_acq_rel))
    {
    }
    if (trace)
        KLOG_DIAG("XMA post-create MMIO addr=%08X raw=%08X ppc=%08X\n", address, raw, value);

    {
        static std::atomic<uint32_t> anyMmioReports{0};
        const uint32_t report = anyMmioReports.fetch_add(1, std::memory_order_relaxed);
        if (report < 48)
            KLOG("MMIO store addr=%08X raw=%08X ppc=%08X\n", address, raw, value);
    }

    if ((address & 0xFFFF0000u) == kXmaPhysicalAliasBase)
    {
        static std::atomic<uint32_t> aliasReports{0};
        const uint32_t report = aliasReports.fetch_add(1, std::memory_order_relaxed);
        if (report < 48)
            KLOG("XMA alias store addr=%08X raw=%08X ppc=%08X\n", address, raw, value);
    }

    const bool canonicalKick = address >= kXmaKickBase && address < kXmaKickBase + 10u * 4u;
    const bool canonicalLock = address >= kXmaLockBase && address < kXmaLockBase + 10u * 4u;
    const bool canonicalClear = address >= kXmaClearBase && address < kXmaClearBase + 10u * 4u;
    const bool aliasKick = address >= kXmaAliasKickBase && address < kXmaAliasKickBase + 10u * 4u;
    const bool aliasLock = address >= kXmaAliasLockBase && address < kXmaAliasLockBase + 10u * 4u;
    const bool aliasClear = address >= kXmaAliasClearBase && address < kXmaAliasClearBase + 10u * 4u;
    const bool kick = canonicalKick || aliasKick;
    const bool lock = canonicalLock || aliasLock;
    const bool clear = canonicalClear || aliasClear;
    if (kick || lock || clear)
    {
        const uint32_t commandBase = kick ? (aliasKick ? kXmaAliasKickBase : kXmaKickBase)
                                         : lock ? (aliasLock ? kXmaAliasLockBase : kXmaLockBase)
                                                : (aliasClear ? kXmaAliasClearBase : kXmaClearBase);
        const uint32_t group = (address - commandBase) / 4u;

        // Mirror both register apertures into the canonical backing. The guest
        // writes these registers with stwbrx, so `raw` is already the logical
        // little-endian hardware mask expected by the XMA device.
        const uint32_t canonicalAddress = (kick ? kXmaKickBase : lock ? kXmaLockBase : kXmaClearBase) + group * 4u;
        XmaMmioRaw(canonicalAddress) = raw;
        ApplyXmaCommand(kick, lock, clear, group, raw);
        // Command registers are edge-triggered. State has already been consumed
        // synchronously above, so don't leave a stale command visible to later work.
        XmaMmioRaw(canonicalAddress) = 0;

        const uint32_t report = g_mmioReports.fetch_add(1, std::memory_order_relaxed);
        if (report < 32)
        {
            const char* kind = kick ? "kick" : lock ? "lock" : "clear";
            KLOG("XMA MMIO %s group=%u raw=%08X ppc=%08X addr=%08X%s\n",
                 kind, group, raw, value, address,
                 (aliasKick || aliasLock || aliasClear) ? " alias" : "");
        }
    }
}

extern "C" void MojoRecompXmaRoutineProbe(uint32_t kind, uint32_t object)
{
    if (!std::getenv("MOJORECOMP_XMA_DIAGNOSTICS"))
        return;
    const uint32_t report = g_xmaRoutineReports.fetch_add(1, std::memory_order_relaxed);
    if (report >= 48 || !object)
        return;
    auto* words = reinterpret_cast<be<uint32_t>*>(g_guestMemory.Translate(object));
    KLOG("XMA routine kind=%u obj=%08X count=%u flags=%08X entries=%08X\n",
         kind, object, uint32_t(words[0]), uint32_t(words[1]), uint32_t(words[2]));
}

GUEST_FUNCTION_HOOK(__imp__XAudioRegisterRenderDriverClient,
                    XAudioRegisterRenderDriverClient_x)
GUEST_FUNCTION_HOOK(__imp__XAudioUnregisterRenderDriverClient,
                    XAudioUnregisterRenderDriverClient_x)
GUEST_FUNCTION_HOOK(__imp__XAudioSubmitRenderDriverFrame, XAudioSubmitRenderDriverFrame_x)
GUEST_FUNCTION_HOOK(__imp__XAudioGetVoiceCategoryVolumeChangeMask,
                    XAudioGetVoiceCategoryVolumeChangeMask_x)
GUEST_FUNCTION_HOOK(__imp__XAudioGetVoiceCategoryVolume, XAudioGetVoiceCategoryVolume_x)
GUEST_FUNCTION_HOOK(__imp__XMACreateContext, XMACreateContext_x)
GUEST_FUNCTION_HOOK(__imp__XMAReleaseContext, XMAReleaseContext_x)
