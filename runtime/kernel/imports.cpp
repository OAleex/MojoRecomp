#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <xbox.h>
#include <xex.h>

#include "../debug_mode.h"
#include "../config/runtime_config.h"
#include "../cpu/timebase.h"
#include "../cpu/guest_thread.h"
#include "../cpu/guest_fiber.h"
#include "../gpu/pm4.h"
#include "../host/input.h"
#include "guestcall.h"
#include "heap.h"
#include "memory.h"
#include "unimplemented.h"
#include "vfs.h"
#include "xex_loader.h"

namespace {
struct MojoLastIndirectCallRecord
{
    uint32_t target = 0;
    uint32_t lr = 0;
    uint32_t object = 0;
    uint64_t sequence = 0;
};

thread_local MojoLastIndirectCallRecord g_mojoLastIndirectCallRecord;
}

extern "C" void MojoRecompTraceIndirectCall(uint32_t target, uint32_t lr, uint32_t object)
{
    // Keep an exact per-host-thread breadcrumb for the crash reporter. This is
    // intentionally always-on and extremely cheap; it lets a release build
    // report the guest indirect target that immediately preceded a host crash
    // without enabling the much noisier profiling/tracing paths below.
    g_mojoLastIndirectCallRecord.target = target;
    g_mojoLastIndirectCallRecord.lr = lr;
    g_mojoLastIndirectCallRecord.object = object;
    ++g_mojoLastIndirectCallRecord.sequence;

    static const bool indirectProfile = [] {
        const char* value = std::getenv("MOJORECOMP_INDIRECT_PROFILE");
        return value && *value && std::strcmp(value, "0") != 0;
    }();
    if (indirectProfile)
    {
        struct Sample
        {
            uint32_t target = 0;
            uint32_t lr = 0;
            uint32_t vtable = 0;
            uint32_t object = 0;
            uint64_t hits = 0;
        };
        thread_local std::array<Sample, 96> samples{};
        thread_local uint64_t calls = 0;
        thread_local uint64_t sampled = 0;
        thread_local auto windowStart = std::chrono::steady_clock::now();

        ++calls;
        if ((calls & 0xFFu) == 0)
        {
            ++sampled;
            uint32_t vtable = 0;
            if (object >= 0x80000000u)
            {
                vtable = *reinterpret_cast<const be<uint32_t>*>(
                    g_guestMemory.Translate(object));
            }
            Sample* slot = nullptr;
            for (auto& sample : samples)
            {
                if (sample.hits && sample.target == target && sample.lr == lr &&
                    sample.vtable == vtable)
                {
                    slot = &sample;
                    break;
                }
                if (!slot && !sample.hits)
                    slot = &sample;
            }
            if (slot)
            {
                if (!slot->hits)
                {
                    slot->target = target;
                    slot->lr = lr;
                    slot->vtable = vtable;
                    slot->object = object;
                }
                ++slot->hits;
            }
        }

        if ((calls & 0x3FFFu) == 0)
        {
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - windowStart).count();
            if (seconds >= 1.0)
            {
                auto sorted = samples;
                std::sort(sorted.begin(), sorted.end(),
                          [](const Sample& a, const Sample& b) { return a.hits > b.hits; });
                const uint32_t pcr = g_ppcContext ? g_ppcContext->r13.u32 : 0u;
                const unsigned cpu = pcr
                    ? unsigned(*reinterpret_cast<const uint8_t*>(
                          g_guestMemory.Translate(pcr + 0x10C)))
                    : 0xFFu;
                std::fprintf(stderr,
                             "[indirect-profile] cpu=%u pcr=%08X rate=%.0f/s sampled=%llu",
                             cpu, pcr, double(calls) / seconds,
                             static_cast<unsigned long long>(sampled));
                for (size_t i = 0; i < 12 && sorted[i].hits; ++i)
                {
                    std::fprintf(stderr, " %08X@%08X/vt=%08X/obj=%08X:%llu",
                                 sorted[i].target, sorted[i].lr, sorted[i].vtable,
                                 sorted[i].object,
                                 static_cast<unsigned long long>(sorted[i].hits));
                }
                std::fprintf(stderr, "\n");
                samples = {};
                calls = 0;
                sampled = 0;
                windowStart = now;
            }
        }
    }

    const bool managerMethod = target == 0x822958A8u || target == 0x822959D8u || target == 0x82296280u;
    const bool stateMethod = target == 0x8228F430u || target == 0x8228F4F8u || target == 0x8228F5B8u ||
                             target == 0x8228F640u || target == 0x8228F7A8u;
    const bool refMethod = target == 0x822926F8u || target == 0x822919C0u;
    if (!managerMethod && !stateMethod && !refMethod)
        return;
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_SAVE_INDIRECT_TRACE");
        return value && *value && std::strcmp(value, "0") != 0;
    }();
    if (!enabled || !object)
        return;

    const auto load32 = [](uint32_t address) -> uint32_t {
        return *reinterpret_cast<const be<uint32_t>*>(g_guestMemory.Translate(address));
    };
    const auto load8 = [](uint32_t address) -> uint32_t {
        return *reinterpret_cast<const uint8_t*>(g_guestMemory.Translate(address));
    };

    if (managerMethod) {
        if (load32(object) != 0x8202EF44u)
            return;
        KLOG("[save indirect] target=%08X lr=%08X object=%08X active=%u flags=%u/%u/%u\n",
             target, lr, object, load32(object + 216u), load8(object + 220u),
             load8(object + 221u), load8(object + 222u));
        return;
    }

    if (stateMethod) {
        const uint32_t backing = load32(object + 8u);
        const uint32_t manager = backing ? load32(backing + 12u) : 0u;
        if (!manager || load32(manager) != 0x8202EF44u)
            return;
        KLOG("[save state] target=%08X lr=%08X this=%08X backing=%08X manager=%08X "
             "ref=%u b20=%08X tasks=%u open=%u kind=%u slot=%08X\n",
             target, lr, object, backing, manager,
             load32(backing + 8u), load32(backing + 20u), load32(backing + 24u),
             load8(backing + 28u), load8(backing + 29u), load32(backing + 348u));
        return;
    }

    if (load32(object) != 0x8202EC7Cu)
        return;
    const uint32_t manager = load32(object + 12u);
    if (!manager || load32(manager) != 0x8202EF44u)
        return;
    KLOG("[save ref] %s target=%08X lr=%08X this=%08X manager=%08X ref(before)=%u tasks=%u open=%u kind=%u\n",
         target == 0x822926F8u ? "AddRef" : "Release", target, lr, object, manager,
         load32(object + 8u), load32(object + 24u), load8(object + 28u), load8(object + 29u));
}

extern "C" void MojoRecompGetLastIndirectCall(uint32_t* target, uint32_t* lr,
                                              uint32_t* object, uint64_t* sequence)
{
    if (target)
        *target = g_mojoLastIndirectCallRecord.target;
    if (lr)
        *lr = g_mojoLastIndirectCallRecord.lr;
    if (object)
        *object = g_mojoLastIndirectCallRecord.object;
    if (sequence)
        *sequence = g_mojoLastIndirectCallRecord.sequence;
}

namespace {

bool SaveDiagnosticsEnabled();

constexpr uint32_t kStatusSuccess = 0x00000000u;
constexpr uint32_t kStatusUnsuccessful = 0xC0000001u;
constexpr uint32_t kStatusInvalidHandle = 0xC0000008u;
constexpr uint32_t kStatusInvalidParameter = 0xC000000Du;
constexpr uint32_t kStatusNoMemory = 0xC0000017u;
constexpr uint32_t kStatusTimeout = 0x00000102u;
constexpr uint32_t kStatusSemaphoreLimitExceeded = 0xC0000047u;
constexpr uint32_t kStatusMutantNotOwned = 0xC0000046u;
constexpr uint32_t kStatusNotImplemented = 0xC0000002u;
constexpr uint32_t kStatusNoSuchFile = 0xC000000Fu;
constexpr uint32_t kStatusEndOfFile = 0xC0000011u;
constexpr uint32_t kStatusAccessDenied = 0xC0000022u;
constexpr uint32_t kStatusObjectNameCollision = 0xC0000035u;
constexpr uint32_t kStatusObjectPathNotFound = 0xC000003Au;
constexpr uint32_t WAIT_INFINITE = 0xFFFFFFFFu;

void HostSleepPrecise(uint32_t milliseconds)
{
    if (milliseconds == 0)
    {
        std::this_thread::yield();
        return;
    }

    // Guest delays are expressed at 100 ns precision. Windows' ordinary
    // sleep_for may overshoot a 16 ms request by a scheduler quantum, which is
    // enough to make a 30 Hz title visibly run at roughly half speed. Prefer a
    // process-local high-resolution waitable timer and keep a safe fallback.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    thread_local HANDLE timer = []() -> HANDLE {
        HANDLE value = CreateWaitableTimerExW(nullptr, nullptr,
                                              CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!value)
            value = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        return value;
    }();

    if (timer)
    {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(milliseconds) * 10000ll;
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(milliseconds);
    if (milliseconds > 1)
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds - 1));
    while (std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
}

extern "C" void MojoRecompHostPollWaitUs(uint32_t microseconds)
{
    if (!microseconds)
        return;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    thread_local HANDLE timer = []() -> HANDLE {
        HANDLE value = CreateWaitableTimerExW(nullptr, nullptr,
                                              CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!value)
            value = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        return value;
    }();
    if (timer)
    {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(microseconds) * 10ll;
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    std::this_thread::yield();
}

uint32_t GuestTimeoutToMs(const be<int64_t>* timeout)
{
    if (!timeout)
        return WAIT_INFINITE;
    const int64_t value = *timeout;
    if (value >= 0)
        return 0;
    const uint64_t ticks100ns = static_cast<uint64_t>(-value);
    return static_cast<uint32_t>(std::min<uint64_t>(ticks100ns / 10000, 0xFFFFFFFEu));
}

struct GuestVaListReader
{
    uint8_t* base = nullptr;
    uint32_t cursor = 0;

    uint64_t Next()
    {
        const uint64_t value = *reinterpret_cast<const be<uint64_t>*>(base + cursor);
        cursor += 8;
        return value;
    }
};

struct GuestRegisterVaReader
{
    PPCContext& ctx;
    uint8_t* base = nullptr;
    uint32_t index = 0;

    uint64_t Next()
    {
        switch (index++)
        {
        case 0: return ctx.r5.u64;
        case 1: return ctx.r6.u64;
        case 2: return ctx.r7.u64;
        case 3: return ctx.r8.u64;
        case 4: return ctx.r9.u64;
        case 5: return ctx.r10.u64;
        default:
            // PPC parameter save area: r3 begins at sp+16 and each slot is 8
            // bytes, so the first argument beyond r10 is at sp+80.
            return *reinterpret_cast<const be<uint64_t>*>(
                base + ctx.r1.u32 + 80 + (index - 7) * 8);
        }
    }
};

template <typename T>
bool AppendHostPrintf(std::string& output, const std::string& spec, T value)
{
    char local[512]{};
    const int required = std::snprintf(local, sizeof(local), spec.c_str(), value);
    if (required < 0)
        return false;
    if (static_cast<size_t>(required) < sizeof(local))
    {
        output.append(local, static_cast<size_t>(required));
        return true;
    }

    std::vector<char> large(static_cast<size_t>(required) + 1);
    const int second = std::snprintf(large.data(), large.size(), spec.c_str(), value);
    if (second < 0)
        return false;
    output.append(large.data(), static_cast<size_t>(second));
    return true;
}

template <typename Reader>
int GuestFormatPrintf(uint8_t* base, const char* format, Reader& args, std::string& output)
{
    if (!base || !format)
        return -1;

    output.clear();
    for (size_t i = 0; format[i]; ++i)
    {
        if (format[i] != '%')
        {
            output.push_back(format[i]);
            continue;
        }

        if (format[i + 1] == '%')
        {
            output.push_back('%');
            ++i;
            continue;
        }

        std::string spec("%");
        ++i;

        // Flags.
        while (format[i] && std::strchr("-+ #0'", format[i]))
            spec.push_back(format[i++]);

        // Width. Resolve '*' here so the host formatter only receives the
        // actual conversion value and never consumes host-side varargs.
        if (format[i] == '*')
        {
            const int32_t width = static_cast<int32_t>(args.Next());
            if (width < 0)
            {
                if (spec.find('-') == std::string::npos)
                    spec.push_back('-');
                const int64_t positive = -static_cast<int64_t>(width);
                spec += std::to_string(positive);
            }
            else
            {
                spec += std::to_string(width);
            }
            ++i;
        }
        else
        {
            while (format[i] >= '0' && format[i] <= '9')
                spec.push_back(format[i++]);
        }

        // Precision.
        if (format[i] == '.')
        {
            ++i;
            if (format[i] == '*')
            {
                const int32_t precision = static_cast<int32_t>(args.Next());
                if (precision >= 0)
                {
                    spec.push_back('.');
                    spec += std::to_string(precision);
                }
                ++i;
            }
            else
            {
                spec.push_back('.');
                while (format[i] >= '0' && format[i] <= '9')
                    spec.push_back(format[i++]);
            }
        }

        enum class Length { Default, HH, H, L, LL, LongDouble };
        Length length = Length::Default;
        if (format[i] == 'h' && format[i + 1] == 'h')
        {
            length = Length::HH;
            spec += "hh";
            i += 2;
        }
        else if (format[i] == 'h')
        {
            length = Length::H;
            spec.push_back('h');
            ++i;
        }
        else if (format[i] == 'l' && format[i + 1] == 'l')
        {
            length = Length::LL;
            spec += "ll";
            i += 2;
        }
        else if (format[i] == 'l')
        {
            length = Length::L;
            spec.push_back('l');
            ++i;
        }
        else if (format[i] == 'I' && format[i + 1] == '6' && format[i + 2] == '4')
        {
            // Normalize Microsoft's guest I64 modifier to portable host ll.
            length = Length::LL;
            spec += "ll";
            i += 3;
        }
        else if (format[i] == 'j')
        {
            length = Length::LL;
            spec += "ll";
            ++i;
        }
        else if (format[i] == 'z' || format[i] == 't')
        {
            // Xbox 360 size_t/ptrdiff_t are 32-bit. Dropping the host modifier
            // avoids accidentally formatting them as 64-bit host values.
            ++i;
        }
        else if (format[i] == 'L')
        {
            length = Length::LongDouble;
            spec.push_back('L');
            ++i;
        }

        const char conversion = format[i];
        if (!conversion)
            break;
        spec.push_back(conversion);

        const uint64_t raw = conversion == 'n' ? args.Next() :
                             conversion == '%' ? 0 : args.Next();
        bool ok = true;
        switch (conversion)
        {
        case 'd':
        case 'i':
            if (length == Length::LL)
                ok = AppendHostPrintf(output, spec, static_cast<long long>(static_cast<int64_t>(raw)));
            else if (length == Length::L)
                ok = AppendHostPrintf(output, spec, static_cast<long>(static_cast<int32_t>(raw)));
            else
                ok = AppendHostPrintf(output, spec, static_cast<int>(static_cast<int32_t>(raw)));
            break;

        case 'u':
        case 'o':
        case 'x':
        case 'X':
            if (length == Length::LL)
                ok = AppendHostPrintf(output, spec, static_cast<unsigned long long>(raw));
            else if (length == Length::L)
                ok = AppendHostPrintf(output, spec, static_cast<unsigned long>(static_cast<uint32_t>(raw)));
            else
                ok = AppendHostPrintf(output, spec, static_cast<unsigned int>(static_cast<uint32_t>(raw)));
            break;

        case 'c':
            if (length == Length::L)
                ok = AppendHostPrintf(output, spec, static_cast<wint_t>(static_cast<uint32_t>(raw)));
            else
                ok = AppendHostPrintf(output, spec, static_cast<int>(static_cast<uint32_t>(raw)));
            break;

        case 's':
        {
            const uint32_t guest = static_cast<uint32_t>(raw);
            if (length == Length::L)
            {
                const auto* value = guest ? reinterpret_cast<const wchar_t*>(base + guest) : L"(null)";
                ok = AppendHostPrintf(output, spec, value);
            }
            else
            {
                const auto* value = guest ? reinterpret_cast<const char*>(base + guest) : "(null)";
                ok = AppendHostPrintf(output, spec, value);
            }
            break;
        }

        case 'p':
            ok = AppendHostPrintf(output, spec,
                                  reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(raw))));
            break;

        case 'f': case 'F': case 'e': case 'E':
        case 'g': case 'G': case 'a': case 'A':
        {
            double value = std::bit_cast<double>(raw);
            if (length == Length::LongDouble)
                ok = AppendHostPrintf(output, spec, static_cast<long double>(value));
            else
                ok = AppendHostPrintf(output, spec, value);
            break;
        }

        case 'n':
        {
            const uint32_t guest = static_cast<uint32_t>(raw);
            if (!guest)
                break;
            if (length == Length::HH)
                *reinterpret_cast<int8_t*>(base + guest) = static_cast<int8_t>(output.size());
            else if (length == Length::H)
                *reinterpret_cast<be<int16_t>*>(base + guest) = static_cast<int16_t>(output.size());
            else if (length == Length::LL)
                *reinterpret_cast<be<int64_t>*>(base + guest) = static_cast<int64_t>(output.size());
            else
                *reinterpret_cast<be<int32_t>*>(base + guest) = static_cast<int32_t>(output.size());
            break;
        }

        default:
            // Preserve unknown conversion text visibly rather than silently
            // consuming a value and manufacturing unrelated output.
            output += spec;
            break;
        }

        if (!ok)
            return -1;
    }

    return output.size() > static_cast<size_t>(INT32_MAX)
        ? INT32_MAX : static_cast<int>(output.size());
}

void ReportBlockingWait(const char* name, uint32_t requestedMs,
                        std::chrono::steady_clock::time_point start,
                        uint32_t status)
{
    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();

    // Optional low-overhead aggregate profiler for long-running title timing
    // investigations. The old bounded log below is useful during boot, but it
    // exhausts its 96 entries well before gameplay/cinematics. Aggregate by
    // caller and wait primitive and emit at most once per second per host thread
    // so profiling remains usable without materially perturbing scheduling.
    static const bool profileWaits = [] {
        const char* value = std::getenv("MOJORECOMP_WAIT_PROFILE");
        return value && *value && value[0] != '0';
    }();
    if (profileWaits)
    {
        struct WaitSample
        {
            uint32_t lr = 0;
            const char* name = nullptr;
            uint64_t calls = 0;
            uint64_t requestedMs = 0;
            uint64_t actualUs = 0;
            uint64_t maxUs = 0;
        };
        struct WaitProfile
        {
            std::array<WaitSample, 64> samples{};
            std::chrono::steady_clock::time_point nextReport =
                std::chrono::steady_clock::now() + std::chrono::seconds(1);
        };
        thread_local WaitProfile profile;
        const uint32_t lr = g_ppcContext ? static_cast<uint32_t>(g_ppcContext->lr) : 0u;
        WaitSample* sample = nullptr;
        for (auto& candidate : profile.samples)
        {
            if (candidate.calls && candidate.lr == lr && candidate.name == name)
            {
                sample = &candidate;
                break;
            }
            if (!sample && !candidate.calls)
                sample = &candidate;
        }
        if (sample)
        {
            if (!sample->calls)
            {
                sample->lr = lr;
                sample->name = name;
            }
            ++sample->calls;
            if (requestedMs != WAIT_INFINITE)
                sample->requestedMs += requestedMs;
            sample->actualUs += static_cast<uint64_t>(std::max<int64_t>(elapsedUs, 0));
            sample->maxUs = std::max<uint64_t>(
                sample->maxUs, static_cast<uint64_t>(std::max<int64_t>(elapsedUs, 0)));
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= profile.nextReport)
        {
            auto sorted = profile.samples;
            std::sort(sorted.begin(), sorted.end(), [](const WaitSample& a, const WaitSample& b) {
                return a.actualUs > b.actualUs;
            });
            const uint32_t profilePcr = g_ppcContext ? g_ppcContext->r13.u32 : 0u;
            const unsigned profileCpu = profilePcr
                ? unsigned(*reinterpret_cast<const uint8_t*>(
                      g_guestMemory.Translate(profilePcr + 0x10C)))
                : 0xFFu;
            KLOG("[wait profile] begin cpu=%u pcr=%08X\n", profileCpu, profilePcr);
            for (size_t i = 0; i < 12 && sorted[i].calls; ++i)
            {
                const auto& s = sorted[i];
                KLOG("[wait profile] %s lr=%08X calls=%llu req=%.2f ms actual=%.2f ms avg=%.3f ms max=%.3f ms\n",
                     s.name ? s.name : "?", s.lr,
                     static_cast<unsigned long long>(s.calls),
                     double(s.requestedMs), double(s.actualUs) / 1000.0,
                     double(s.actualUs) / 1000.0 / double(s.calls),
                     double(s.maxUs) / 1000.0);
            }
            KLOG("[wait profile] end\n");
            profile.samples = {};
            profile.nextReport = now + std::chrono::seconds(1);
        }
    }

    if (elapsedUs < 2000)
        return;
    static std::atomic<uint32_t> reports{0};
    const uint32_t report = reports.fetch_add(1, std::memory_order_relaxed);
    if (report >= 96)
        return;
    const uint32_t lr = g_ppcContext ? static_cast<uint32_t>(g_ppcContext->lr) : 0u;
    KLOG("guest wait #%u %s lr=%08X requested=%s%u ms actual=%.3f ms status=%08X\n",
         report + 1, name, lr,
         requestedMs == WAIT_INFINITE ? "inf/" : "",
         requestedMs == WAIT_INFINITE ? 0u : requestedMs,
         double(elapsedUs) / 1000.0, status);
}

uint32_t CurrentGuestThreadId()
{
    if (g_ppcContext && g_ppcContext->r13.u32)
        return g_ppcContext->r13.u32;
    static std::atomic<uint32_t> next{1};
    thread_local uint32_t id = next.fetch_add(1, std::memory_order_relaxed);
    return id;
}

// ---------------------------------------------------------------------------
// Virtual / physical memory
// ---------------------------------------------------------------------------

uint32_t NtAllocateVirtualMemory_x(be<uint32_t>* baseAddress, be<uint32_t>* regionSize,
                                   uint32_t allocType, uint32_t protect, uint32_t debugMemory)
{
    (void)protect;
    (void)debugMemory;
    if (!baseAddress || !regionSize)
        return kStatusInvalidParameter;

    uint32_t size = (uint32_t(*regionSize) + 0xFFFu) & ~0xFFFu;
    const uint32_t requested = *baseAddress;
    if (requested)
    {
        constexpr uint32_t kReserve = 0x2000u;
        if ((allocType & kReserve) && requested >= 0x40000000u && requested < 0x7FE00000u)
        {
            const uint32_t got = g_guestHeap.ReserveVirtualAt(requested, size);
            if (!got)
                return kStatusNoMemory;
            *baseAddress = got;
            *regionSize = (size + 0xFFFFu) & ~0xFFFFu;
            return kStatusSuccess;
        }

        // The host already reserves the whole 4 GB guest map. A commit inside an
        // existing reservation therefore only needs to reproduce the guest-visible
        // page rounding.
        const uint32_t gran =
            requested >= 0x40000000u && requested < 0x7FE00000u ? 0x10000u : 0x1000u;
        const uint32_t first = requested & ~(gran - 1);
        const uint64_t end = (uint64_t(requested) + size + gran - 1) & ~uint64_t(gran - 1);
        *regionSize = static_cast<uint32_t>(end - first);
        return kStatusSuccess;
    }

    constexpr uint32_t kLargePages = 0x20000000u;
    void* ptr = g_guestHeap.AllocVirtual(size, (allocType & kLargePages) != 0);
    if (!ptr)
        return kStatusNoMemory;
    *baseAddress = g_guestMemory.MapVirtual(ptr);
    *regionSize = size;
    return kStatusSuccess;
}

uint32_t NtFreeVirtualMemory_x(be<uint32_t>* baseAddress, be<uint32_t>* regionSize,
                               uint32_t freeType)
{
    (void)regionSize;
    constexpr uint32_t kRelease = 0x8000u;
    if (baseAddress && *baseAddress && (freeType & kRelease))
        g_guestHeap.Free(g_guestMemory.Translate(*baseAddress));
    return kStatusSuccess;
}

uint32_t NtQueryVirtualMemory_x(uint32_t address, be<uint32_t>* info, uint32_t infoLength)
{
    if (!info || infoLength < 28)
        return kStatusInvalidParameter;
    uint32_t regionBase = 0, regionSize = 0;
    if (!g_guestHeap.QueryRegion(address, regionBase, regionSize))
    {
        info[0] = address & ~0xFFFFu;
        info[1] = 0;
        info[2] = 0;
        info[3] = 0x10000;
        info[4] = 0x10000; // MEM_FREE
        info[5] = 0x01;    // PAGE_NOACCESS
        info[6] = 0;
        return kStatusSuccess;
    }

    const uint32_t page = address & ~0xFFFu;
    info[0] = page;
    info[1] = regionBase;
    info[2] = 0x04;
    info[3] = regionBase + regionSize - page;
    info[4] = 0x1000;  // MEM_COMMIT
    info[5] = 0x04;    // PAGE_READWRITE
    info[6] = 0x20000; // MEM_PRIVATE
    return kStatusSuccess;
}

uint32_t MmAllocatePhysicalMemoryEx_x(uint32_t flags, uint32_t size, uint32_t protect,
                                      uint32_t minAddress, uint32_t maxAddress,
                                      uint32_t alignment)
{
    (void)flags;
    (void)protect;
    (void)minAddress;
    (void)maxAddress;
    void* ptr = g_guestHeap.AllocPhysical(size, alignment);
    return ptr ? g_guestMemory.MapVirtual(ptr) : 0;
}

void MmFreePhysicalMemory_x(uint32_t type, uint32_t address)
{
    (void)type;
    if (address)
        g_guestHeap.Free(g_guestMemory.Translate(address));
}

uint32_t MmGetPhysicalAddress_x(uint32_t address)
{
    return address >= 0xA0000000u ? address & 0x1FFFFFFFu : address;
}

uint32_t MmQueryAddressProtect_x(uint32_t address)
{
    (void)address;
    return 0x04; // PAGE_READWRITE
}

struct MmStatisticsSection
{
    be<uint32_t> availablePages;
    be<uint32_t> totalVirtualMemoryBytes;
    be<uint32_t> reservedVirtualMemoryBytes;
    be<uint32_t> physicalPages;
    be<uint32_t> poolPages;
    be<uint32_t> stackPages;
    be<uint32_t> imagePages;
    be<uint32_t> heapPages;
    be<uint32_t> virtualPages;
    be<uint32_t> pageTablePages;
    be<uint32_t> cachePages;
};

struct MmStatistics
{
    be<uint32_t> size;
    be<uint32_t> totalPhysicalPages;
    be<uint32_t> kernelPages;
    MmStatisticsSection title;
    MmStatisticsSection system;
    be<uint32_t> highestPhysicalPage;
};

static_assert(sizeof(MmStatistics) == 104);

uint32_t MmQueryStatistics_x(MmStatistics* stats)
{
    constexpr uint32_t kStatusBufferTooSmall = 0xC0000023u;
    if (!stats)
        return kStatusInvalidParameter;
    if (uint32_t(stats->size) != sizeof(MmStatistics))
        return kStatusBufferTooSmall;

    std::memset(stats, 0, sizeof(*stats));
    stats->size = sizeof(MmStatistics);
    stats->totalPhysicalPages = 0x00020000; // 512 MB / 4 KB
    stats->kernelPages = 0x00000300;

    // This is the same guest-visible shape Xenia exposes. The title converts the
    // first few page counters into bytes and uses them for its own memory budget;
    // they therefore need to describe a healthy 512 MB console, not the host PC.
    stats->title.availablePages = 0x0001B000;
    stats->title.totalVirtualMemoryBytes = 0x2FFF0000;
    stats->title.reservedVirtualMemoryBytes = 0x00160000;
    stats->title.physicalPages = 0x00001000;
    stats->title.poolPages = 0x00000010;
    stats->title.stackPages = 0x00000100;
    stats->title.imagePages = 0x00000100;
    stats->title.heapPages = 0x00000100;
    stats->title.virtualPages = 0x00000100;
    stats->title.pageTablePages = 0x00000100;
    stats->title.cachePages = 0x00000100;
    stats->highestPhysicalPage = 0x0001FFFF;
    return kStatusSuccess;
}

uint32_t MmCreateKernelStack_x(uint32_t stackSize, uint32_t unknown)
{
    (void)unknown;
    const uint32_t aligned = (stackSize + 0xFFFu) & ~0xFFFu;
    const uint32_t alignment = (stackSize & 0xF000u) ? 0x1000u : 0x10000u;
    void* host = g_guestHeap.AllocKernelStack(aligned, alignment);
    if (!host)
        return 0;
    const uint32_t low = g_guestMemory.MapVirtual(host);
    return low + stackSize;
}

uint32_t MmDeleteKernelStack_x(uint32_t stackBase, uint32_t stackEnd)
{
    (void)stackBase;
    if (!stackEnd)
        return kStatusInvalidParameter;
    mojorecomp::fiber::ReleaseStack(stackEnd);
    g_guestHeap.Free(g_guestMemory.Translate(stackEnd));
    return kStatusSuccess;
}

// ---------------------------------------------------------------------------
// Critical sections / spinlocks
// ---------------------------------------------------------------------------

std::mutex g_csMapMutex;
std::unordered_map<uint32_t, std::shared_ptr<std::recursive_mutex>> g_csMap;

bool CriticalSectionCacheEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_CS_CACHE");
        return value && *value && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

bool CriticalSectionProfileEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_CS_PROFILE");
        return value && *value && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

bool MutantFastPollEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_MUTANT_FAST_POLL");
        return value && *value && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

void ProfileCriticalSection(XRTL_CRITICAL_SECTION* cs)
{
    if (!CriticalSectionProfileEnabled())
        return;

    struct Sample
    {
        uint32_t lr = 0;
        uint32_t key = 0;
        uint64_t hits = 0;
    };
    thread_local uint64_t calls = 0;
    thread_local std::array<Sample, 64> samples{};
    ++calls;

    // Sample 1 in 65,536 calls. The title can issue over a million enters per
    // second, so exact per-call accounting would perturb the very loop we are
    // trying to identify.
    if ((calls & 0xFFFFu) == 0)
    {
        const uint32_t lr = g_ppcContext ? static_cast<uint32_t>(g_ppcContext->lr) : 0u;
        const uint32_t key = cs ? g_guestMemory.MapVirtual(cs) : 0u;
        Sample* slot = nullptr;
        for (auto& sample : samples)
        {
            if (sample.hits && sample.lr == lr && sample.key == key)
            {
                slot = &sample;
                break;
            }
            if (!slot && !sample.hits)
                slot = &sample;
        }
        if (slot)
        {
            if (!slot->hits)
            {
                slot->lr = lr;
                slot->key = key;
            }
            ++slot->hits;
        }
    }

    if ((calls & 0xFFFFFFu) == 0)
    {
        auto sorted = samples;
        std::sort(sorted.begin(), sorted.end(),
                  [](const Sample& a, const Sample& b) { return a.hits > b.hits; });
        std::fprintf(stderr, "[cs-profile] enter calls=%llu",
                     static_cast<unsigned long long>(calls));
        for (size_t i = 0; i < 8 && sorted[i].hits; ++i)
        {
            std::fprintf(stderr, " lr=%08X cs=%08X sample=%llu",
                         sorted[i].lr, sorted[i].key,
                         static_cast<unsigned long long>(sorted[i].hits));
        }
        std::fprintf(stderr, "\n");
    }
}

std::shared_ptr<std::recursive_mutex> CriticalSectionLock(XRTL_CRITICAL_SECTION* cs)
{
    const uint32_t key = g_guestMemory.MapVirtual(cs);
    std::lock_guard guard(g_csMapMutex);
    auto& slot = g_csMap[key];
    if (!slot)
        slot = std::make_shared<std::recursive_mutex>();
    return slot;
}

std::recursive_mutex* CriticalSectionLockCached(XRTL_CRITICAL_SECTION* cs)
{
    if (!CriticalSectionCacheEnabled())
        return CriticalSectionLock(cs).get();

    struct CacheEntry
    {
        uint32_t key = 0;
        std::recursive_mutex* lock = nullptr;
    };
    thread_local std::array<CacheEntry, 32> cache{};

    const uint32_t key = g_guestMemory.MapVirtual(cs);
    CacheEntry& entry = cache[(key >> 4) & (cache.size() - 1)];
    if (entry.lock && entry.key == key)
        return entry.lock;

    // g_csMap never erases entries, so the mutex object owned by the shared_ptr
    // remains alive for the lifetime of the title. Cache only the raw pointer on
    // the hot path; cache misses still use the exact same synchronized lookup.
    auto lock = CriticalSectionLock(cs);
    entry.key = key;
    entry.lock = lock.get();
    return entry.lock;
}

uint32_t RtlInitializeCriticalSection_x(XRTL_CRITICAL_SECTION* cs)
{
    if (!cs)
        return kStatusInvalidParameter;
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = 0;
    (void)CriticalSectionLock(cs);
    return kStatusSuccess;
}

void RtlEnterCriticalSection_x(XRTL_CRITICAL_SECTION* cs)
{
    if (!cs)
        return;
    ProfileCriticalSection(cs);
    auto* lock = CriticalSectionLockCached(cs);
    lock->lock();
    if (cs->RecursionCount == 0)
        cs->OwningThread = CurrentGuestThreadId();
    ++cs->RecursionCount;
}

void RtlLeaveCriticalSection_x(XRTL_CRITICAL_SECTION* cs)
{
    if (!cs)
        return;
    auto* lock = CriticalSectionLockCached(cs);
    if (cs->RecursionCount > 0 && --cs->RecursionCount == 0)
        cs->OwningThread = 0;
    lock->unlock();
}

void SpinAcquire(uint32_t* word)
{
    if (!word)
        return;
    const uint32_t self = CurrentGuestThreadId();
    for (;;)
    {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(word, &expected, self, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return;
        std::this_thread::yield();
    }
}

void SpinRelease(uint32_t* word)
{
    if (word)
        __atomic_store_n(word, 0u, __ATOMIC_RELEASE);
}

void KfAcquireSpinLock_x(uint32_t* word) { SpinAcquire(word); }
void KfReleaseSpinLock_x(uint32_t* word) { SpinRelease(word); }
void KeAcquireSpinLockAtRaisedIrql_x(uint32_t* word) { SpinAcquire(word); }
void KeReleaseSpinLockFromRaisedIrql_x(uint32_t* word) { SpinRelease(word); }
void KeEnterCriticalRegion_x() {}
void KeLeaveCriticalRegion_x() {}

// ---------------------------------------------------------------------------
// Handle objects: events, semaphores, notification listener
// ---------------------------------------------------------------------------

struct KernelObject
{
    virtual ~KernelObject() = default;
    virtual uint32_t Wait(uint32_t timeoutMs) = 0;
};

struct Event final : KernelObject
{
    std::mutex mutex;
    std::condition_variable cv;
    bool manualReset = false;
    bool signaled = false;

    Event(bool manual, bool initial) : manualReset(manual), signaled(initial) {}

    uint32_t Wait(uint32_t timeoutMs) override
    {
        std::unique_lock lock(mutex);
        const auto ready = [&] { return signaled; };
        if (timeoutMs == WAIT_INFINITE)
            cv.wait(lock, ready);
        else if (!cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready))
            return kStatusTimeout;
        if (!manualReset)
            signaled = false;
        return kStatusSuccess;
    }

    uint32_t Set()
    {
        std::lock_guard lock(mutex);
        const uint32_t previous = signaled ? 1u : 0u;
        signaled = true;
        cv.notify_all();
        return previous;
    }
};

struct Semaphore final : KernelObject
{
    std::mutex mutex;
    std::condition_variable cv;
    uint32_t count;
    uint32_t maximum;

    Semaphore(uint32_t initial, uint32_t limit) : count(initial), maximum(limit) {}

    uint32_t Wait(uint32_t timeoutMs) override
    {
        std::unique_lock lock(mutex);
        const auto ready = [&] { return count != 0; };
        if (timeoutMs == WAIT_INFINITE)
            cv.wait(lock, ready);
        else if (!cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready))
            return kStatusTimeout;
        --count;
        return kStatusSuccess;
    }

    uint32_t Release(uint32_t amount, uint32_t* previous)
    {
        std::lock_guard lock(mutex);
        if (amount > maximum || count > maximum - amount)
            return kStatusSemaphoreLimitExceeded;
        if (previous)
            *previous = count;
        count += amount;
        cv.notify_all();
        return kStatusSuccess;
    }
};

struct Mutant final : KernelObject
{
    std::mutex mutex;
    std::condition_variable cv;
    uint32_t owner = 0;
    uint32_t recursion = 0;

    explicit Mutant(bool initialOwner)
    {
        if (initialOwner)
        {
            owner = CurrentGuestThreadId();
            recursion = 1;
        }
    }

    uint32_t Wait(uint32_t timeoutMs) override
    {
        const uint32_t self = CurrentGuestThreadId();
        std::unique_lock lock(mutex);
        if (owner == self)
        {
            ++recursion;
            return kStatusSuccess;
        }
        const auto ready = [&] { return owner == 0; };
        if (timeoutMs == WAIT_INFINITE)
            cv.wait(lock, ready);
        else if (!cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready))
            return kStatusTimeout;
        owner = self;
        recursion = 1;
        return kStatusSuccess;
    }

    uint32_t TryWait()
    {
        const uint32_t self = CurrentGuestThreadId();
        std::lock_guard lock(mutex);
        if (owner == self)
        {
            ++recursion;
            return kStatusSuccess;
        }
        if (owner != 0)
            return kStatusTimeout;
        owner = self;
        recursion = 1;
        return kStatusSuccess;
    }

    uint32_t Release()
    {
        std::lock_guard lock(mutex);
        if (owner != CurrentGuestThreadId() || recursion == 0)
            return kStatusMutantNotOwned;
        if (--recursion == 0)
        {
            owner = 0;
            cv.notify_one();
        }
        return kStatusSuccess;
    }
};

struct NotifyListener final : KernelObject
{
    struct Notification
    {
        uint32_t id;
        uint32_t param;
    };

    uint64_t mask;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Notification> pending;

    explicit NotifyListener(uint64_t value) : mask(value) {}

    uint32_t Wait(uint32_t timeoutMs) override
    {
        std::unique_lock lock(mutex);
        const auto ready = [&] { return !pending.empty(); };
        if (timeoutMs == WAIT_INFINITE)
            cv.wait(lock, ready);
        else if (!cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready))
            return kStatusTimeout;
        return kStatusSuccess;
    }

    void Push(uint32_t id, uint32_t param)
    {
        std::lock_guard lock(mutex);
        pending.push_back({id, param});
        cv.notify_all();
    }

    bool Pop(uint32_t matchId, Notification* out)
    {
        std::lock_guard lock(mutex);
        auto it = pending.begin();
        if (matchId != 0)
        {
            it = std::find_if(pending.begin(), pending.end(), [&](const Notification& n) {
                // Version-0/system XNIDs have their public index in the low 16 bits.
                // Accept either a complete XNID or the index used by retail callers.
                return n.id == matchId || (n.id & 0xFFFFu) == (matchId & 0xFFFFu);
            });
        }
        if (it == pending.end())
            return false;
        if (out)
            *out = *it;
        pending.erase(it);
        return true;
    }
};

struct ContentEnumerator final : KernelObject
{
    std::vector<XCONTENT_DATA> items;
    size_t cursor = 0;

    explicit ContentEnumerator(std::vector<XCONTENT_DATA> values)
        : items(std::move(values)) {}

    uint32_t Wait(uint32_t timeoutMs) override
    {
        (void)timeoutMs;
        return kStatusTimeout;
    }

    uint32_t Read(void* buffer, uint32_t bufferBytes, uint32_t* countOut)
    {
        if (countOut)
            *countOut = 0;
        if (!buffer || bufferBytes < sizeof(XCONTENT_DATA))
            return 87u; // ERROR_INVALID_PARAMETER
        if (cursor >= items.size())
            return 18u; // ERROR_NO_MORE_FILES

        const size_t capacity = bufferBytes / sizeof(XCONTENT_DATA);
        const size_t count = std::min(capacity, items.size() - cursor);
        std::memcpy(buffer, items.data() + cursor, count * sizeof(XCONTENT_DATA));
        cursor += count;
        if (countOut)
            *countOut = static_cast<uint32_t>(count);
        return 0;
    }
};

struct FileObject final : KernelObject
{
    FILE* fp = nullptr;
    std::string guestPath;
    std::string hostPath;
    bool directory = false;
    bool writable = false;
    uint64_t size = 0;
    std::mutex io;

    ~FileObject() override
    {
        if (fp)
            std::fclose(fp);
    }

    uint32_t Wait(uint32_t timeoutMs) override
    {
        (void)timeoutMs;
        return kStatusInvalidHandle;
    }
};

struct ThreadObject final : KernelObject, std::enable_shared_from_this<ThreadObject>
{
    std::mutex mutex;
    std::condition_variable cv;
    uint32_t function = 0;
    uint32_t arg0 = 0;
    uint32_t arg1 = 0;
    uint32_t flags = 0;
    uint32_t stackSize = 0;
    uint32_t threadId = 0;
    std::atomic<uint32_t> guestCpu{0};
    std::atomic<uint32_t> livePcr{0};
    bool suspended = false;
    bool finished = false;
    uint32_t exitCode = 0;
    std::thread hostThread;

    ThreadObject(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t createFlags,
                 uint32_t stack, uint32_t tid)
        : function(fn), arg0(a0), arg1(a1), flags(createFlags), stackSize(stack),
          threadId(tid), suspended((createFlags & 1u) != 0)
    {
        const uint8_t mask = static_cast<uint8_t>(createFlags >> 24);
        if (mask)
        {
            for (uint32_t i = 0; i < 6; ++i)
                if (mask & (1u << i))
                    guestCpu.store(i, std::memory_order_relaxed);
        }
    }

    ~ThreadObject() override
    {
        if (hostThread.joinable())
            hostThread.detach();
    }

    void Start()
    {
        auto self = shared_from_this();
        hostThread = std::thread([self] {
            {
                std::unique_lock lock(self->mutex);
                self->cv.wait(lock, [&] { return !self->suspended; });
            }

            const uint32_t cpu = self->guestCpu.load(std::memory_order_acquire);

            uint32_t code = 0;
            try
            {
                GuestThreadContext context(cpu, self->stackSize, 64, self->threadId);
                self->livePcr.store(context.pcr, std::memory_order_release);
                // Close the race with SetGuestCpu between the pre-construction
                // guestCpu load above and publishing the PCR.
                SetGuestThreadLogicalCpu(
                    context.pcr, self->guestCpu.load(std::memory_order_acquire));
                context.ppc.r3.u64 = self->arg0;
                context.ppc.r4.u64 = self->arg1;
                PPCFunc* fn = g_guestMemory.FindFunction(self->function);
                if (!fn)
                {
                    KLOG("thread %X entry %08X is not recompiled\n", self->threadId,
                         self->function);
                }
                else
                {
                    KLOG("thread %X start entry=%08X cpu=%u stack=%X\n", self->threadId,
                         self->function, cpu, self->stackSize);
                    fn(context.ppc, g_guestMemory.base);
                    code = context.ppc.r3.u32;
                }
                self->livePcr.store(0, std::memory_order_release);
            }
            catch (const GuestThreadExit& e)
            {
                code = e.code;
            }
            catch (const MojoRecompUnimplementedImport& e)
            {
                KLOG("thread %X stopped at missing import %s (lr=%08X)\n",
                     self->threadId, e.name, e.lr);
                code = 0xC0000002u;
            }

            // The GuestThreadContext is gone on every path above at this point.
            // Never leave an affinity update targeting a stale PCR.
            self->livePcr.store(0, std::memory_order_release);

            {
                std::lock_guard lock(self->mutex);
                self->exitCode = code;
                self->finished = true;
            }
            self->cv.notify_all();
            KLOG("thread %X ended r3=%08X\n", self->threadId, code);
        });
    }

    uint32_t Resume()
    {
        std::lock_guard lock(mutex);
        const uint32_t previous = suspended ? 1u : 0u;
        suspended = false;
        cv.notify_all();
        return previous;
    }

    uint32_t SetGuestCpu(uint32_t cpu)
    {
        const uint32_t previous = guestCpu.exchange(cpu, std::memory_order_acq_rel);
        const uint32_t pcr = livePcr.load(std::memory_order_acquire);
        if (pcr)
            SetGuestThreadLogicalCpu(pcr, cpu);
        return previous;
    }

    uint32_t Wait(uint32_t timeoutMs) override
    {
        std::unique_lock lock(mutex);
        const auto done = [&] { return finished; };
        if (timeoutMs == WAIT_INFINITE)
            cv.wait(lock, done);
        else if (!cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), done))
            return kStatusTimeout;
        return kStatusSuccess;
    }
};

std::mutex g_handleMutex;
std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_handles;
std::atomic<uint32_t> g_nextHandle{0x80000100u};
std::atomic<uint32_t> g_nextThreadId{0xF04u};
std::mutex g_terminateMutex;
std::vector<std::pair<uint32_t, uint32_t>> g_terminateNotifications;

uint32_t AddHandle(std::shared_ptr<KernelObject> object)
{
    const uint32_t handle = g_nextHandle.fetch_add(4, std::memory_order_relaxed);
    std::lock_guard guard(g_handleMutex);
    g_handles.emplace(handle, std::move(object));
    return handle;
}

std::shared_ptr<KernelObject> FindHandle(uint32_t handle)
{
    std::lock_guard guard(g_handleMutex);
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? nullptr : it->second;
}

std::shared_ptr<FileObject> FindFile(uint32_t handle)
{
    return std::dynamic_pointer_cast<FileObject>(FindHandle(handle));
}

std::string ObjectPath(XOBJECT_ATTRIBUTES* attrs)
{
    if (!attrs || !attrs->Name)
        return {};
    XANSI_STRING* name = attrs->Name;
    if (!name || !name->Buffer)
        return {};
    return std::string(name->Buffer.get(), static_cast<uint16_t>(name->Length));
}

struct PendingApc
{
    uint32_t routine;
    uint32_t context;
    uint32_t iosb;
};

thread_local std::vector<PendingApc> g_pendingApcs;

struct PendingXamCompletion
{
    uint32_t hostThreadId;
    uint32_t routine;
    uint32_t error;
    uint32_t length;
    uint32_t overlapped;
};

std::mutex g_xamCompletionMutex;
std::vector<PendingXamCompletion> g_pendingXamCompletions;

void QueueCurrentThreadApc(uint32_t routine, uint32_t context, XIO_STATUS_BLOCK* iosb)
{
    if (!routine)
        return;
    g_pendingApcs.push_back({routine, context, iosb ? g_guestMemory.MapVirtual(iosb) : 0});
}

void QueueXamCompletion(uint32_t hostThreadId, uint32_t routine, uint32_t error,
                        uint32_t length, uint32_t overlapped)
{
    if (!routine)
        return;
    std::lock_guard lock(g_xamCompletionMutex);
    g_pendingXamCompletions.push_back(
        {hostThreadId, routine, error, length, overlapped});
}

bool DrainCurrentThreadApcs()
{
    if (!g_ppcContext)
        return false;
    auto queue = std::move(g_pendingApcs);
    g_pendingApcs.clear();

    std::vector<PendingXamCompletion> xamQueue;
    const uint32_t hostThreadId = GetCurrentThreadId();
    {
        std::lock_guard lock(g_xamCompletionMutex);
        auto it = g_pendingXamCompletions.begin();
        while (it != g_pendingXamCompletions.end())
        {
            if (it->hostThreadId == hostThreadId)
            {
                xamQueue.push_back(*it);
                it = g_pendingXamCompletions.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    if (queue.empty() && xamQueue.empty())
        return false;

    bool ran = false;
    for (const PendingApc& apc : queue)
    {
        const uint32_t routine = apc.routine & ~1u;
        PPCFunc* fn = g_guestMemory.FindFunction(routine);
        if (!fn)
        {
            KLOG("APC routine %08X is not recompiled\n", routine);
            continue;
        }
        g_ppcContext->r3.u64 = apc.context;
        g_ppcContext->r4.u64 = apc.iosb;
        g_ppcContext->r5.u64 = 0;
        fn(*g_ppcContext, g_guestMemory.base);
        ran = true;
    }

    for (const PendingXamCompletion& completion : xamQueue)
    {
        const uint32_t routine = completion.routine & ~1u;
        PPCFunc* fn = g_guestMemory.FindFunction(routine);
        if (!fn)
        {
            KLOG("XAM completion routine %08X is not recompiled\n", routine);
            continue;
        }

        // PXOVERLAPPED_COMPLETION_ROUTINE(error, bytesTransferred, pOverlapped).
        g_ppcContext->r3.u64 = completion.error;
        g_ppcContext->r4.u64 = completion.length;
        g_ppcContext->r5.u64 = completion.overlapped;
        fn(*g_ppcContext, g_guestMemory.base);
        ran = true;
    }
    return ran;
}

void SignalHandleEvent(uint32_t handle)
{
    if (!handle)
        return;
    auto event = std::dynamic_pointer_cast<Event>(FindHandle(handle));
    if (event)
        event->Set();
}

int Seek64(FILE* fp, int64_t offset, int origin)
{
#if defined(_WIN32)
    return _fseeki64(fp, offset, origin);
#else
    return fseeko(fp, offset, origin);
#endif
}

int64_t Tell64(FILE* fp)
{
#if defined(_WIN32)
    return _ftelli64(fp);
#else
    return ftello(fp);
#endif
}

bool ResizeFile64(FILE* fp, uint64_t size)
{
    if (!fp || size > uint64_t(INT64_MAX))
        return false;
#if defined(_WIN32)
    return _chsize_s(_fileno(fp), static_cast<__int64>(size)) == 0;
#else
    return ftruncate(fileno(fp), static_cast<off_t>(size)) == 0;
#endif
}

enum : uint32_t
{
    kFileSupersede = 0,
    kFileOpen = 1,
    kFileCreate = 2,
    kFileOpenIf = 3,
    kFileOverwrite = 4,
    kFileOverwriteIf = 5,
};

bool WantsWrite(uint32_t desiredAccess, uint32_t disposition)
{
    constexpr uint32_t kGenericWrite = 0x40000000u;
    constexpr uint32_t kGenericAll = 0x10000000u;
    constexpr uint32_t kFileWriteData = 0x00000002u;
    constexpr uint32_t kFileAppendData = 0x00000004u;
    return (desiredAccess & (kGenericWrite | kGenericAll | kFileWriteData | kFileAppendData)) ||
           disposition == kFileSupersede || disposition == kFileCreate ||
           disposition == kFileOverwrite || disposition == kFileOverwriteIf;
}

uint32_t NtCreateFile_x(be<uint32_t>* handleOut, uint32_t desiredAccess,
                        XOBJECT_ATTRIBUTES* attrs, XIO_STATUS_BLOCK* iosb,
                        be<uint64_t>* allocationSize, uint32_t fileAttributes,
                        uint32_t shareAccess, uint32_t createDisposition,
                        uint32_t createOptions)
{
    (void)allocationSize;
    (void)fileAttributes;
    (void)shareAccess;
    if (handleOut)
        *handleOut = 0;
    if (iosb)
    {
        iosb->Status = kStatusNoSuchFile;
        iosb->Information = 0;
    }
    if (!handleOut || !iosb)
        return kStatusInvalidParameter;

    const std::string guestPath = ObjectPath(attrs);
    if (guestPath.empty())
        return kStatusInvalidParameter;

    namespace fs = std::filesystem;
    std::string hostPath = VfsResolveExisting(guestPath);
    std::error_code ec;
    const bool existed = !hostPath.empty();
    const bool wantsWrite = WantsWrite(desiredAccess, createDisposition);
    const bool mayCreate = createDisposition == kFileSupersede ||
                           createDisposition == kFileCreate ||
                           createDisposition == kFileOpenIf ||
                           createDisposition == kFileOverwriteIf;

    if (existed && createDisposition == kFileCreate)
    {
        iosb->Status = kStatusObjectNameCollision;
        return kStatusObjectNameCollision;
    }
    if (wantsWrite && !VfsDeviceWritable(guestPath))
    {
        KLOG("NtCreateFile('%s'): write refused on read-only game device\n", guestPath.c_str());
        iosb->Status = kStatusAccessDenied;
        return kStatusAccessDenied;
    }

    if (!existed)
    {
        if (!mayCreate)
        {
            if (guestPath.find("movies\\") != std::string::npos && g_ppcContext)
            {
                uint32_t attrsGuest = attrs ? g_guestMemory.MapVirtual(attrs) : 0;
                uint32_t nameGuest = 0;
                uint32_t pathGuest = 0;
                if (attrs && attrs->Name)
                {
                    nameGuest = g_guestMemory.MapVirtual(attrs->Name.get());
                    XANSI_STRING* movieName = attrs->Name;
                    if (movieName && movieName->Buffer)
                        pathGuest = g_guestMemory.MapVirtual(movieName->Buffer.get());
                }
                KLOG("movie open miss: lr=%08X sp=%08X r3=%08X r4=%08X r5=%08X r6=%08X\n",
                     static_cast<uint32_t>(g_ppcContext->lr), g_ppcContext->r1.u32, g_ppcContext->r3.u32,
                     g_ppcContext->r4.u32, g_ppcContext->r5.u32,
                     g_ppcContext->r6.u32);
                KLOG("movie object path: attrs=%08X name=%08X buffer=%08X len=%u\n",
                     attrsGuest, nameGuest, pathGuest,
                     attrs && attrs->Name ? static_cast<uint16_t>(attrs->Name->Length) : 0);
                const uint32_t sp = g_ppcContext->r1.u32;
                KLOG("movie stack words:");
                for (uint32_t i = 0; i < 128; ++i)
                {
                    const uint32_t value = *reinterpret_cast<const be<uint32_t>*>(
                        g_guestMemory.Translate(sp + i * 4));
                    if (value >= 0x82000000u && value < 0x83000000u)
                        std::fprintf(stderr, " +%02X=%08X", i * 4, value);
                }
                std::fprintf(stderr, "\n");

                KLOG("movie PPC backchain:");
                uint32_t frame = sp;
                for (uint32_t depth = 0; depth < 12; ++depth)
                {
                    const uint32_t callerSp = static_cast<uint32_t>(
                        *reinterpret_cast<const be<uint32_t>*>(g_guestMemory.Translate(frame)));
                    if (!callerSp || callerSp <= frame)
                        break;
                    const uint32_t savedLr = static_cast<uint32_t>(
                        *reinterpret_cast<const be<uint32_t>*>(g_guestMemory.Translate(callerSp - 8)));
                    std::fprintf(stderr, " #%u sp=%08X lr=%08X", depth, callerSp, savedLr);
                    frame = callerSp;
                }
                std::fprintf(stderr, "\n");

                // sub_82291960 dispatches this open through the global file/movie
                // manager at 0x824B4F94. Record the concrete vcall target so the
                // title-side selection logic can be inspected without guessing.
                const uint32_t manager =
                    *reinterpret_cast<const be<uint32_t>*>(g_guestMemory.Translate(0x824B4F94u + 28));
                if (manager)
                {
                    const uint32_t vtable =
                        *reinterpret_cast<const be<uint32_t>*>(g_guestMemory.Translate(manager));
                    uint32_t openTarget = 0;
                    if (vtable)
                        openTarget = static_cast<uint32_t>(
                            *reinterpret_cast<const be<uint32_t>*>(g_guestMemory.Translate(vtable + 16)));
                    KLOG("movie manager: obj=%08X vtable=%08X vcall+10=%08X\n",
                         manager, vtable, openTarget);
                }
            }
            KLOG("NtCreateFile('%s') -> not found\n", guestPath.c_str());
            return kStatusNoSuchFile;
        }
        hostPath = VfsTranslate(guestPath, true);
        if (hostPath.empty())
        {
            iosb->Status = kStatusObjectPathNotFound;
            return kStatusObjectPathNotFound;
        }
        fs::create_directories(fs::path(hostPath).parent_path(), ec);
        VfsForget(guestPath);
    }

    const bool isDirectory = existed && fs::is_directory(hostPath, ec);
    constexpr uint32_t kDirectory = 0x00000001u;
    constexpr uint32_t kNonDirectory = 0x00000040u;
    if (isDirectory && (createOptions & kNonDirectory))
        return kStatusObjectPathNotFound;
    if (!isDirectory && (createOptions & kDirectory))
        return kStatusObjectPathNotFound;

    const bool truncate = !existed || createDisposition == kFileSupersede ||
                          createDisposition == kFileOverwrite ||
                          createDisposition == kFileOverwriteIf;
    auto file = std::make_shared<FileObject>();
    file->guestPath = guestPath;
    file->hostPath = hostPath;
    file->directory = isDirectory;
    file->writable = wantsWrite;

    if (!isDirectory)
    {
        const char* mode = !wantsWrite ? "rb" : (truncate ? "w+b" : "r+b");
        file->fp = std::fopen(hostPath.c_str(), mode);
        if (!file->fp)
        {
            iosb->Status = kStatusUnsuccessful;
            KLOG("NtCreateFile('%s'): host open failed for %s\n", guestPath.c_str(),
                 hostPath.c_str());
            return kStatusUnsuccessful;
        }
        file->size = truncate ? 0 : static_cast<uint64_t>(fs::file_size(hostPath, ec));
    }

    const uint32_t handle = AddHandle(file);
    *handleOut = handle;
    iosb->Status = kStatusSuccess;
    iosb->Information = !existed ? 2u : (truncate ? 3u : 1u);
    KLOG("NtCreateFile('%s') -> %08X (%llu bytes%s)\n", guestPath.c_str(), handle,
         static_cast<unsigned long long>(file->size), isDirectory ? ", dir" : "");
    return kStatusSuccess;
}

uint32_t NtOpenFile_x(be<uint32_t>* handleOut, uint32_t desiredAccess,
                      XOBJECT_ATTRIBUTES* attrs, XIO_STATUS_BLOCK* iosb,
                      uint32_t shareAccess, uint32_t openOptions)
{
    return NtCreateFile_x(handleOut, desiredAccess, attrs, iosb, nullptr, 0x80,
                          shareAccess, kFileOpen, openOptions);
}

uint32_t NtReadFile_x(uint32_t handle, uint32_t event, uint32_t apcRoutine,
                       uint32_t apcContext, XIO_STATUS_BLOCK* iosb, uint8_t* buffer,
                       uint32_t length, be<uint64_t>* byteOffset)
{
    if (iosb)
    {
        iosb->Status = kStatusUnsuccessful;
        iosb->Information = 0;
    }
    auto file = FindFile(handle);
    if (!file || !file->fp)
        return kStatusInvalidHandle;
    if (!buffer || !iosb)
        return kStatusInvalidParameter;

    std::lock_guard io(file->io);
    if (byteOffset)
    {
        const uint64_t offset = *byteOffset;
        if (offset != 0xFFFFFFFFFFFFFFFEull)
            Seek64(file->fp, static_cast<int64_t>(offset), SEEK_SET);
    }
    const size_t got = std::fread(buffer, 1, length, file->fp);
    const uint32_t status = got == 0 && length != 0 ? kStatusEndOfFile : kStatusSuccess;
    iosb->Status = status;
    iosb->Information = static_cast<uint32_t>(got);
    SignalHandleEvent(event);
    QueueCurrentThreadApc(apcRoutine, apcContext, iosb);
    return status;
}

uint32_t NtWriteFile_x(uint32_t handle, uint32_t event, uint32_t apcRoutine,
                       uint32_t apcContext, XIO_STATUS_BLOCK* iosb, const uint8_t* buffer,
                       uint32_t length, be<uint64_t>* byteOffset)
{
    if (iosb)
    {
        iosb->Status = kStatusUnsuccessful;
        iosb->Information = 0;
    }
    auto file = FindFile(handle);
    if (!file || !file->fp)
        return kStatusInvalidHandle;
    if (!file->writable)
        return kStatusAccessDenied;
    if (!buffer || !iosb)
        return kStatusInvalidParameter;

    std::lock_guard io(file->io);
    if (byteOffset)
    {
        const uint64_t offset = *byteOffset;
        if (offset != 0xFFFFFFFFFFFFFFFEull)
            Seek64(file->fp, static_cast<int64_t>(offset), SEEK_SET);
    }
    const size_t put = std::fwrite(buffer, 1, length, file->fp);
    std::fflush(file->fp);
    const int64_t position = Tell64(file->fp);
    if (position > 0)
        file->size = std::max<uint64_t>(file->size, static_cast<uint64_t>(position));
    const uint32_t status = put == length ? kStatusSuccess : kStatusUnsuccessful;
    iosb->Status = status;
    iosb->Information = static_cast<uint32_t>(put);
    if (file->guestPath.rfind("s0:", 0) == 0)
    {
        KLOG("NtWriteFile('%s') len=%u put=%zu pos=%lld size=%llu status=%08X\n",
             file->guestPath.c_str(), length, put,
             static_cast<long long>(position),
             static_cast<unsigned long long>(file->size), status);
    }
    SignalHandleEvent(event);
    QueueCurrentThreadApc(apcRoutine, apcContext, iosb);
    return status;
}

uint32_t NtQueryInformationFile_x(uint32_t handle, XIO_STATUS_BLOCK* iosb,
                                  be<uint32_t>* info, uint32_t length, uint32_t infoClass)
{
    if (iosb)
    {
        iosb->Status = kStatusUnsuccessful;
        iosb->Information = 0;
    }
    auto file = FindFile(handle);
    if (!file)
        return kStatusInvalidHandle;
    if (!iosb || !info || !length)
        return kStatusInvalidParameter;
    std::memset(info, 0, length);

    auto put64 = [&](size_t dword, uint64_t value) {
        if ((dword + 2) * sizeof(uint32_t) <= length)
        {
            info[dword] = static_cast<uint32_t>(value >> 32);
            info[dword + 1] = static_cast<uint32_t>(value);
        }
    };
    switch (infoClass)
    {
    case 0x05: // FileStandardInformation
        put64(0, (file->size + 0x7FFu) & ~uint64_t(0x7FFu));
        put64(2, file->size);
        if (length >= 22)
            reinterpret_cast<uint8_t*>(info)[21] = file->directory ? 1 : 0;
        break;
    case 0x0E: // FilePositionInformation
        put64(0, file->fp ? static_cast<uint64_t>(Tell64(file->fp)) : 0);
        break;
    case 0x22: // FileNetworkOpenInformation
        put64(8, (file->size + 0x7FFu) & ~uint64_t(0x7FFu));
        put64(10, file->size);
        if (length >= 52)
            info[12] = file->directory ? 0x10u : 0x20u;
        break;
    default:
        KLOG("NtQueryInformationFile('%s'): class %X len=%u not implemented\n",
             file->guestPath.c_str(), infoClass, length);
        return kStatusNotImplemented;
    }
    iosb->Status = kStatusSuccess;
    iosb->Information = length;
    return kStatusSuccess;
}

uint32_t NtSetInformationFile_x(uint32_t handle, XIO_STATUS_BLOCK* iosb,
                                be<uint32_t>* info, uint32_t length, uint32_t infoClass)
{
    if (iosb)
    {
        iosb->Status = kStatusUnsuccessful;
        iosb->Information = 0;
    }
    auto file = FindFile(handle);
    if (!file)
        return kStatusInvalidHandle;
    if (!iosb || !info || length < 8 || !file->fp)
        return kStatusInvalidParameter;

    const uint64_t value = (uint64_t(uint32_t(info[0])) << 32) | uint32_t(info[1]);
    std::lock_guard io(file->io);

    switch (infoClass)
    {
    case 0x0E: // FilePositionInformation
        if (value > uint64_t(INT64_MAX) ||
            Seek64(file->fp, static_cast<int64_t>(value), SEEK_SET) != 0)
        {
            iosb->Status = kStatusUnsuccessful;
            return kStatusUnsuccessful;
        }
        break;

    case 0x13: // FileAllocationInformation
        if (!file->writable || file->directory)
        {
            iosb->Status = kStatusAccessDenied;
            return kStatusAccessDenied;
        }
        // Allocation size is capacity, not logical EOF. If it is reduced below
        // EOF, however, Windows truncates EOF to the new allocation size. For a
        // larger request the host filesystem may allocate lazily; reporting
        // success preserves the Xbox contract without artificially growing EOF.
        if (value < file->size)
        {
            std::fflush(file->fp);
            if (!ResizeFile64(file->fp, value))
            {
                iosb->Status = kStatusUnsuccessful;
                return kStatusUnsuccessful;
            }
            file->size = value;
        }
        if (file->guestPath.rfind("s0:", 0) == 0)
            KLOG("NtSetInformationFile('%s') allocation=%llu eof=%llu\n",
                 file->guestPath.c_str(), static_cast<unsigned long long>(value),
                 static_cast<unsigned long long>(file->size));
        break;

    case 0x14: // FileEndOfFileInformation
        if (!file->writable || file->directory)
        {
            iosb->Status = kStatusAccessDenied;
            return kStatusAccessDenied;
        }
        std::fflush(file->fp);
        if (!ResizeFile64(file->fp, value))
        {
            iosb->Status = kStatusUnsuccessful;
            return kStatusUnsuccessful;
        }
        file->size = value;
        if (file->guestPath.rfind("s0:", 0) == 0)
            KLOG("NtSetInformationFile('%s') eof=%llu\n", file->guestPath.c_str(),
                 static_cast<unsigned long long>(value));
        break;

    default:
        KLOG("NtSetInformationFile('%s'): class %X len=%u not implemented\n",
             file->guestPath.c_str(), infoClass, length);
        iosb->Status = kStatusNotImplemented;
        return kStatusNotImplemented;
    }

    iosb->Status = kStatusSuccess;
    iosb->Information = length;
    return kStatusSuccess;
}

uint32_t NtFlushBuffersFile_x(uint32_t handle, XIO_STATUS_BLOCK* iosb)
{
    auto file = FindFile(handle);
    if (!file || !file->fp)
        return kStatusInvalidHandle;
    const int result = std::fflush(file->fp);
    if (iosb)
    {
        iosb->Status = result == 0 ? kStatusSuccess : kStatusUnsuccessful;
        iosb->Information = 0;
    }
    return result == 0 ? kStatusSuccess : kStatusUnsuccessful;
}

uint32_t NtQueryVolumeInformationFile_x(uint32_t handle, XIO_STATUS_BLOCK* iosb,
                                        be<uint32_t>* info, uint32_t length,
                                        uint32_t infoClass)
{
    auto file = FindFile(handle);
    if (!file)
        return kStatusInvalidHandle;
    if (!iosb || !info || length == 0)
        return kStatusInvalidParameter;
    std::memset(info, 0, length);

    // FileFsSizeInformation (3): total/allocation units and sector sizes. Report a
    // console-like 8 GB volume; the title only uses this to budget/cache, never to
    // address host storage directly.
    if (infoClass == 3 && length >= 24)
    {
        info[0] = 0;
        info[1] = 0x00400000; // total allocation units
        info[2] = 0;
        info[3] = 0x00300000; // available units
        info[4] = 1;          // sectors per allocation unit
        info[5] = 0x800;      // bytes per sector
    }
    iosb->Status = kStatusSuccess;
    iosb->Information = length;
    return kStatusSuccess;
}

uint32_t NtCreateEvent_x(be<uint32_t>* handle, void* attrs, uint32_t eventType,
                         uint32_t initialState)
{
    (void)attrs;
    if (!handle)
        return kStatusInvalidParameter;
    *handle = AddHandle(std::make_shared<Event>(eventType == 0, initialState != 0));
    return kStatusSuccess;
}

uint32_t NtSetEvent_x(uint32_t handle, be<uint32_t>* previousState)
{
    auto event = std::dynamic_pointer_cast<Event>(FindHandle(handle));
    if (!event)
        return kStatusInvalidHandle;
    const uint32_t previous = event->Set();
    if (previousState)
        *previousState = previous;
    return kStatusSuccess;
}

uint32_t NtCreateSemaphore_x(be<uint32_t>* handle, XOBJECT_ATTRIBUTES* attrs,
                             uint32_t initialCount, uint32_t maximumCount)
{
    (void)attrs;
    if (!handle || maximumCount == 0 || initialCount > maximumCount)
        return kStatusInvalidParameter;
    *handle = AddHandle(std::make_shared<Semaphore>(initialCount, maximumCount));
    return kStatusSuccess;
}

uint32_t NtCreateMutant_x(be<uint32_t>* handle, XOBJECT_ATTRIBUTES* attrs,
                          uint32_t initialOwner)
{
    (void)attrs;
    if (!handle)
        return kStatusInvalidParameter;
    *handle = AddHandle(std::make_shared<Mutant>(initialOwner != 0));
    return kStatusSuccess;
}

uint32_t NtReleaseMutant_x(uint32_t handle, uint32_t unknown)
{
    (void)unknown;
    auto object = std::dynamic_pointer_cast<Mutant>(FindHandle(handle));
    return object ? object->Release() : kStatusInvalidHandle;
}

uint32_t NtReleaseSemaphore_x(uint32_t handle, uint32_t releaseCount,
                              be<int32_t>* previousCount)
{
    auto object = std::dynamic_pointer_cast<Semaphore>(FindHandle(handle));
    if (!object)
        return kStatusInvalidHandle;
    uint32_t previous = 0;
    const uint32_t status = object->Release(releaseCount, &previous);
    if (status == kStatusSuccess && previousCount)
        *previousCount = static_cast<int32_t>(previous);
    return status;
}

uint32_t NtWaitForSingleObjectEx_x(uint32_t handle, uint32_t mode, uint32_t alertable,
                                   be<int64_t>* timeout)
{
    (void)mode;
    constexpr uint32_t kStatusUserApc = 0x000000C0u;
    if (alertable && DrainCurrentThreadApcs())
        return kStatusUserApc;
    auto object = FindHandle(handle);
    if (!object)
        return kStatusInvalidHandle;
    const uint32_t timeoutMs = GuestTimeoutToMs(timeout);
    if (MutantFastPollEnabled() && !alertable && timeoutMs == 0)
    {
        if (auto mutant = std::dynamic_pointer_cast<Mutant>(object))
            return mutant->TryWait();
    }

    const auto start = std::chrono::steady_clock::now();
    const uint32_t status = object->Wait(timeoutMs);
    static const bool handleWaitProfile = [] {
        const char* value = std::getenv("MOJORECOMP_HANDLE_WAIT_PROFILE");
        return value && *value && value[0] != '0';
    }();
    if (handleWaitProfile && g_ppcContext && g_ppcContext->lr == 0x82268BE8u)
    {
        const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsedUs >= 10000)
        {
            const char* type = "KernelObject";
            if (std::dynamic_pointer_cast<Event>(object)) type = "Event";
            else if (std::dynamic_pointer_cast<Semaphore>(object)) type = "Semaphore";
            else if (std::dynamic_pointer_cast<Mutant>(object)) type = "Mutant";
            else if (std::dynamic_pointer_cast<ThreadObject>(object)) type = "Thread";
            else if (std::dynamic_pointer_cast<NotifyListener>(object)) type = "NotifyListener";
            else if (std::dynamic_pointer_cast<ContentEnumerator>(object)) type = "ContentEnumerator";
            else if (std::dynamic_pointer_cast<FileObject>(object)) type = "File";
            KLOG("[handle-wait] lr=82268BE8 handle=%08X type=%s timeout=%u alertable=%u elapsed=%.3fms status=%08X cpu=%u pcr=%08X\n",
                 handle, type, timeoutMs, alertable,
                 double(elapsedUs) / 1000.0, status,
                 unsigned(*reinterpret_cast<const uint8_t*>(
                     g_guestMemory.Translate(g_ppcContext->r13.u32 + 0x10C))),
                 g_ppcContext->r13.u32);
        }
    }
    ReportBlockingWait("NtWaitForSingleObjectEx", timeoutMs, start, status);
    if (alertable && DrainCurrentThreadApcs())
        return kStatusUserApc;
    return status;
}

void BroadcastXamNotification(uint32_t id, uint32_t param)
{
    std::vector<std::shared_ptr<NotifyListener>> listeners;
    {
        std::lock_guard guard(g_handleMutex);
        listeners.reserve(g_handles.size());
        for (const auto& [handle, object] : g_handles)
        {
            (void)handle;
            if (auto listener = std::dynamic_pointer_cast<NotifyListener>(object))
                listeners.push_back(std::move(listener));
        }
    }
    for (const auto& listener : listeners)
        listener->Push(id, param);
}

uint32_t NtWaitForMultipleObjectsEx_x(uint32_t count, be<uint32_t>* handles,
                                      uint32_t waitType, uint32_t waitMode,
                                      uint32_t alertable, be<int64_t>* timeout)
{
    (void)waitMode;
    constexpr uint32_t kStatusUserApc = 0x000000C0u;
    if (!count || !handles || count > 64 || waitType > 1)
        return kStatusInvalidParameter;

    std::vector<std::shared_ptr<KernelObject>> objects;
    objects.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        auto object = FindHandle(uint32_t(handles[i]));
        if (!object)
            return kStatusInvalidParameter;
        objects.push_back(std::move(object));
    }

    const uint32_t timeoutMs = GuestTimeoutToMs(timeout);
    const auto start = std::chrono::steady_clock::now();
    auto finish = [&](uint32_t status) {
        ReportBlockingWait("NtWaitForMultipleObjectsEx", timeoutMs, start, status);
        return status;
    };
    static std::atomic<uint32_t> reports{0};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
        KLOG("NtWaitForMultipleObjectsEx count=%u type=%u alertable=%u timeout=%u\n",
             count, waitType, alertable, timeoutMs);

    if (waitType == 0) // WaitAll
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            if (alertable && DrainCurrentThreadApcs())
                return finish(kStatusUserApc);

            uint32_t remaining = timeoutMs;
            if (timeoutMs != WAIT_INFINITE)
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (elapsed >= timeoutMs)
                    return finish(kStatusTimeout);
                remaining = timeoutMs - static_cast<uint32_t>(elapsed);
            }

            const uint32_t status = objects[i]->Wait(remaining);
            if (status != kStatusSuccess)
                return finish(status);
        }
        return finish(kStatusSuccess);
    }

    // WaitAny. Poll with zero-timeout waits so semaphore/event/mutant acquisition
    // is performed by the object itself exactly when it becomes selected.
    for (;;)
    {
        if (alertable && DrainCurrentThreadApcs())
            return finish(kStatusUserApc);

        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t status = objects[i]->Wait(0);
            if (status == kStatusSuccess)
                return finish(i); // STATUS_WAIT_0 + i
            if (status != kStatusTimeout)
                return finish(status);
        }

        if (timeoutMs == 0)
            return finish(kStatusTimeout);
        if (timeoutMs != WAIT_INFINITE &&
            std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(timeoutMs))
            return finish(kStatusTimeout);
        HostSleepPrecise(1);
    }
}

uint32_t NtClose_x(uint32_t handle)
{
    std::lock_guard guard(g_handleMutex);
    return g_handles.erase(handle) ? kStatusSuccess : kStatusInvalidHandle;
}

uint32_t ObReferenceObjectByHandle_x(uint32_t handle, uint32_t objectType,
                                     be<uint32_t>* object)
{
    (void)objectType;
    if (!object)
        return kStatusInvalidParameter;

    // 0xFFFFFFFE is GetCurrentThread's pseudo-handle. The early CRT wrappers only
    // pass the returned object to priority/affinity helpers, so the PCR is a stable
    // guest-resident identity until the full KTHREAD scheduler lands.
    if (handle == 0xFFFFFFFEu)
        *object = g_ppcContext ? g_ppcContext->r13.u32 : 0;
    else
        *object = handle;
    return kStatusSuccess;
}

void ObDereferenceObject_x(uint32_t object)
{
    (void)object;
}

std::string AnsiStringValue(XANSI_STRING* value)
{
    if (!value || !value->Buffer)
        return {};
    return std::string(value->Buffer.get(), static_cast<uint16_t>(value->Length));
}

uint32_t ObCreateSymbolicLink_x(XANSI_STRING* linkName, XANSI_STRING* targetName)
{
    const std::string link = AnsiStringValue(linkName);
    const std::string target = AnsiStringValue(targetName);
    if (link.empty() || target.empty())
        return kStatusInvalidParameter;
    if (!VfsCreateSymbolicLink(link, target))
        return kStatusInvalidParameter;
    KLOG("ObCreateSymbolicLink('%s' -> '%s')\n", link.c_str(), target.c_str());
    return kStatusSuccess;
}

uint32_t ObDeleteSymbolicLink_x(XANSI_STRING* linkName)
{
    const std::string link = AnsiStringValue(linkName);
    if (link.empty())
        return kStatusInvalidParameter;
    const bool removed = VfsDeleteSymbolicLink(link);
    KLOG("ObDeleteSymbolicLink('%s') -> %s\n", link.c_str(), removed ? "removed" : "missing");
    return removed ? kStatusSuccess : kStatusObjectPathNotFound;
}

uint32_t XamNotifyCreateListener_x(uint64_t mask, uint32_t flags)
{
    (void)flags;
    const uint32_t handle = AddHandle(std::make_shared<NotifyListener>(mask));
    KLOG("XamNotifyCreateListener mask=%016llX -> %08X\n",
         static_cast<unsigned long long>(mask), handle);
    return handle;
}

uint32_t XNotifyGetNext_x(uint32_t handle, uint32_t matchId, be<uint32_t>* idOut,
                          be<uint32_t>* paramOut)
{
    auto listener = std::dynamic_pointer_cast<NotifyListener>(FindHandle(handle));
    if (idOut)
        *idOut = 0;
    if (paramOut)
        *paramOut = 0;
    if (!listener)
        return 0;

    NotifyListener::Notification notification{};
    if (!listener->Pop(matchId, &notification))
        return 0;
    if (idOut)
        *idOut = notification.id;
    if (paramOut)
        *paramOut = notification.param;
    KLOG("XNotifyGetNext handle=%08X match=%08X -> id=%08X param=%08X\n",
         handle, matchId, notification.id, notification.param);
    return 1;
}

uint32_t ExCreateThread_x(be<uint32_t>* handle, uint32_t stackSize,
                          be<uint32_t>* threadId, uint32_t xApiThreadStartup,
                          uint32_t startAddress, uint32_t startContext,
                          uint32_t creationFlags)
{
    const uint32_t id = g_nextThreadId.fetch_add(4, std::memory_order_relaxed);
    const uint32_t function = xApiThreadStartup ? xApiThreadStartup : startAddress;
    const uint32_t arg0 = xApiThreadStartup ? startAddress : startContext;
    const uint32_t arg1 = xApiThreadStartup ? startContext : 0;

    auto thread = std::make_shared<ThreadObject>(function, arg0, arg1, creationFlags,
                                                 stackSize, id);
    const uint32_t guestHandle = AddHandle(thread);
    if (handle)
        *handle = guestHandle;
    if (threadId)
        *threadId = id;

    KLOG("ExCreateThread handle=%08X tid=%X wrapper=%08X entry=%08X ctx=%08X flags=%08X stack=%X\n",
         guestHandle, id, xApiThreadStartup, startAddress, startContext, creationFlags,
         stackSize);
    thread->Start();
    return kStatusSuccess;
}

void ExRegisterTitleTerminateNotification_x(uint32_t registration, uint32_t create)
{
    if (!registration)
        return;
    const uint32_t routine =
        *reinterpret_cast<const be<uint32_t>*>(g_guestMemory.Translate(registration));
    std::lock_guard lock(g_terminateMutex);
    if (create)
    {
        g_terminateNotifications.emplace_back(registration, routine);
        KLOG("title terminate + %08X routine=%08X\n", registration, routine);
    }
    else
    {
        std::erase_if(g_terminateNotifications,
                      [&](const auto& n) { return n.first == registration; });
        KLOG("title terminate - %08X\n", registration);
    }
}

void ExTerminateThread_x(uint32_t code)
{
    throw GuestThreadExit{code};
}

uint32_t NtResumeThread_x(uint32_t handle, be<uint32_t>* suspendCount)
{
    auto thread = std::dynamic_pointer_cast<ThreadObject>(FindHandle(handle));
    if (!thread)
    {
        if (suspendCount)
            *suspendCount = 0;
        return kStatusInvalidHandle;
    }
    const uint32_t previous = thread->Resume();
    if (suspendCount)
        *suspendCount = previous;
    return kStatusSuccess;
}

uint32_t KeResumeThread_x(uint32_t object)
{
    auto thread = std::dynamic_pointer_cast<ThreadObject>(FindHandle(object));
    return thread ? thread->Resume() : 0;
}

// Header-level dispatcher helpers. They cover the simple event/semaphore cases
// used by the CRT before full guest-thread scheduling exists.
//
// These headers live in guest memory but are touched concurrently by several
// native host threads. Plain read/modify/write access to SignalState is a C++
// data race and can also lose semaphore releases when a waiter consumes the
// state at the same time. Serialize the small dispatcher-state transactions so
// guest-visible signaling has kernel-like atomicity while keeping guest memory
// as the source of truth.
std::mutex g_dispatcherHeaderMutex;
std::condition_variable g_dispatcherHeaderCv;

uint32_t KeSetEvent_x(XKEVENT* event, uint32_t increment, uint32_t wait)
{
    (void)increment;
    (void)wait;
    if (!event)
        return 0;
    uint32_t previous = 0;
    {
        std::lock_guard<std::mutex> lock(g_dispatcherHeaderMutex);
        previous = event->SignalState;
        event->SignalState = 1;
    }
    // Raw dispatcher objects live in guest memory, but titles expect a kernel
    // signal to wake blocked threads immediately. The previous implementation
    // only let waiters notice this on their next 1 ms polling pass, which can
    // accumulate across chains of Titanium-engine jobs.
    g_dispatcherHeaderCv.notify_all();
    return previous;
}

uint32_t KeResetEvent_x(XKEVENT* event)
{
    if (!event)
        return 0;
    std::lock_guard<std::mutex> lock(g_dispatcherHeaderMutex);
    const uint32_t previous = event->SignalState;
    event->SignalState = 0;
    return previous;
}

void KeInitializeSemaphore_x(XKSEMAPHORE* sem, int32_t count, int32_t limit)
{
    if (!sem)
        return;
    std::lock_guard<std::mutex> lock(g_dispatcherHeaderMutex);
    std::memset(sem, 0, sizeof(*sem));
    // NT/Xbox KOBJECTS value for a semaphore. Wait logic distinguishes
    // synchronization objects (which consume SignalState) from notification
    // events by this field, so leaving the zeroed EventNotificationObject value
    // here turns every semaphore into a manual-reset event.
    sem->Header.Type = 5; // SemaphoreObject
    sem->Header.Size = static_cast<uint8_t>(sizeof(*sem) / sizeof(uint32_t));
    sem->Header.SignalState = count;
    sem->Limit = limit;
}

uint32_t KeReleaseSemaphore_x(XKSEMAPHORE* sem, uint32_t increment, uint32_t adjustment,
                              uint32_t wait)
{
    (void)increment;
    (void)wait;
    if (!sem)
        return 0;
    uint32_t previous = 0;
    {
        std::lock_guard<std::mutex> lock(g_dispatcherHeaderMutex);
        previous = sem->Header.SignalState;
        const uint32_t limit = sem->Limit;
        sem->Header.SignalState = std::min<uint32_t>(limit, previous + adjustment);
    }
    g_dispatcherHeaderCv.notify_all();
    return previous;
}

uint32_t KeWaitForSingleObject_x(XDISPATCHER_HEADER* object, uint32_t waitReason,
                                 uint32_t processorMode, uint32_t alertable,
                                 be<int64_t>* timeout)
{
    (void)waitReason;
    (void)processorMode;
    (void)alertable;
    if (!object)
        return kStatusInvalidParameter;
    const uint32_t timeoutMs = GuestTimeoutToMs(timeout);
    const auto start = std::chrono::steady_clock::now();
    auto finish = [&](uint32_t status) {
        ReportBlockingWait("KeWaitForSingleObject", timeoutMs, start, status);
        return status;
    };
    std::unique_lock<std::mutex> lock(g_dispatcherHeaderMutex);
    for (;;)
    {
        if (uint32_t(object->SignalState) != 0)
        {
            if (object->Type != 0)
                object->SignalState = uint32_t(object->SignalState) - 1;
            lock.unlock();
            return finish(kStatusSuccess);
        }
        if (timeoutMs == 0)
        {
            lock.unlock();
            return finish(kStatusTimeout);
        }
        if (timeoutMs != WAIT_INFINITE &&
            std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(timeoutMs))
        {
            lock.unlock();
            return finish(kStatusTimeout);
        }

        // KeSetEvent / KeReleaseSemaphore wake this immediately. Keep a 1 ms
        // timed fallback because some XDK/runtime code can modify a dispatcher
        // header directly in guest memory without going through an HLE signal
        // export; the fallback preserves that compatibility without imposing a
        // mandatory millisecond of latency on normal job hand-offs.
        g_dispatcherHeaderCv.wait_for(lock, std::chrono::milliseconds(1));
    }
}

uint32_t KeWaitForMultipleObjects_x(uint32_t count, be<uint32_t>* objects,
                                    uint32_t waitType, uint32_t waitReason,
                                    uint32_t processorMode, uint32_t alertable,
                                    be<int64_t>* timeout, void* waitBlockArray)
{
    (void)waitReason;
    (void)processorMode;
    (void)alertable;
    (void)waitBlockArray;
    if (!count || !objects || count > 64)
        return kStatusInvalidParameter;

    const uint32_t timeoutMs = GuestTimeoutToMs(timeout);
    const auto start = std::chrono::steady_clock::now();
    auto finish = [&](uint32_t status) {
        ReportBlockingWait("KeWaitForMultipleObjects", timeoutMs, start, status);
        return status;
    };
    std::unique_lock<std::mutex> lock(g_dispatcherHeaderMutex);
    for (;;)
    {
        if (waitType == 0) // WaitAll
        {
            bool all = true;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t guest = objects[i];
                auto* object = reinterpret_cast<XDISPATCHER_HEADER*>(
                    guest ? g_guestMemory.Translate(guest) : nullptr);
                if (!object || uint32_t(object->SignalState) == 0)
                {
                    all = false;
                    break;
                }
            }
            if (all)
            {
                for (uint32_t i = 0; i < count; ++i)
                {
                    auto* object = reinterpret_cast<XDISPATCHER_HEADER*>(
                        g_guestMemory.Translate(uint32_t(objects[i])));
                    if (object->Type != 0)
                        object->SignalState = uint32_t(object->SignalState) - 1;
                }
                lock.unlock();
                return finish(kStatusSuccess);
            }
        }
        else // WaitAny
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t guest = objects[i];
                auto* object = reinterpret_cast<XDISPATCHER_HEADER*>(
                    guest ? g_guestMemory.Translate(guest) : nullptr);
                if (object && uint32_t(object->SignalState) != 0)
                {
                    if (object->Type != 0)
                        object->SignalState = uint32_t(object->SignalState) - 1;
                    lock.unlock();
                    return finish(kStatusSuccess + i);
                }
            }
        }

        if (timeoutMs == 0)
        {
            lock.unlock();
            return finish(kStatusTimeout);
        }
        if (timeoutMs != WAIT_INFINITE &&
            std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(timeoutMs))
        {
            lock.unlock();
            return finish(kStatusTimeout);
        }
        g_dispatcherHeaderCv.wait_for(lock, std::chrono::milliseconds(1));
    }
}

// ---------------------------------------------------------------------------
// TLS / timing / basic runtime services
// ---------------------------------------------------------------------------

std::mutex g_tlsMutex;
uint32_t g_tlsNext = 0;
std::vector<uint32_t> g_tlsFree;

uint32_t& TlsSlot(uint32_t index)
{
    thread_local std::vector<uint32_t> slots;
    if (slots.size() <= index)
        slots.resize(index + 1, 0);
    return slots[index];
}

uint32_t KeTlsAlloc_x()
{
    std::lock_guard lock(g_tlsMutex);
    if (!g_tlsFree.empty())
    {
        const uint32_t value = g_tlsFree.back();
        g_tlsFree.pop_back();
        return value;
    }
    return g_tlsNext++;
}

uint32_t KeTlsFree_x(uint32_t index)
{
    std::lock_guard lock(g_tlsMutex);
    g_tlsFree.push_back(index);
    return 1;
}

uint32_t KeTlsGetValue_x(uint32_t index) { return TlsSlot(index); }
uint32_t KeTlsSetValue_x(uint32_t index, uint32_t value)
{
    TlsSlot(index) = value;
    return 1;
}

uint32_t KeGetCurrentProcessType_x() { return 1; }
void KeSetBasePriorityThread_x(uint32_t thread, uint32_t priority)
{
    (void)thread;
    (void)priority;
}

uint32_t KeSetAffinityThread_x(uint32_t thread, uint32_t affinity, be<uint32_t>* previous)
{
    // XSetThreadProcessor passes exactly one of the six Xbox 360 logical CPU
    // bits. ReXGlue always updates the guest CPU identity even when physical
    // host affinity mirroring is disabled (its default).
    if (!affinity || (affinity & ~0x3Fu) || (affinity & (affinity - 1u)))
        return kStatusInvalidParameter;

    uint32_t cpu = 0;
    while ((1u << cpu) != affinity)
        ++cpu;

    uint32_t oldCpu = 0;
    bool changed = false;
    if (auto object = std::dynamic_pointer_cast<ThreadObject>(FindHandle(thread)))
    {
        oldCpu = object->SetGuestCpu(cpu);
        if (previous)
            *previous = 1u << oldCpu;
        changed = true;
    }
    else if (SetGuestThreadLogicalCpu(thread, cpu, &oldCpu))
    {
        if (previous)
            *previous = 1u << oldCpu;
        changed = true;
    }

    if (changed)
    {
        static std::atomic<uint32_t> affinityReports{0};
        if (std::getenv("MOJORECOMP_THREAD_DIAGNOSTICS") &&
            affinityReports.fetch_add(1, std::memory_order_relaxed) < 64)
        {
            KLOG("thread affinity object=%08X cpu=%u->%u mask=%02X lr=%08X\n",
                 thread, oldCpu, cpu, affinity,
                 g_ppcContext ? static_cast<uint32_t>(g_ppcContext->lr) : 0u);
        }
        return kStatusSuccess;
    }

    if (previous)
        *previous = 0;
    return kStatusInvalidHandle;
}

uint64_t KeQueryPerformanceFrequency_x() { return MOJORECOMP_TIMEBASE_HZ; }

void KeQuerySystemTime_x(be<uint64_t>* out)
{
    if (!out)
        return;
    constexpr int64_t epoch = 116444736000000000LL;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const int64_t ticks =
        std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(now)
            .count();
    *out = static_cast<uint64_t>(ticks + epoch);
}

void RtlTimeToTimeFields_x(be<uint64_t>* time, XTIME_FIELDS* fields)
{
    if (!time || !fields)
        return;

    const uint64_t value = uint64_t(*time);
    FILETIME fileTime{};
    fileTime.dwLowDateTime = static_cast<DWORD>(value);
    fileTime.dwHighDateTime = static_cast<DWORD>(value >> 32);
    SYSTEMTIME systemTime{};
    if (!FileTimeToSystemTime(&fileTime, &systemTime))
    {
        std::memset(fields, 0, sizeof(*fields));
        return;
    }

    fields->Year = systemTime.wYear;
    fields->Month = systemTime.wMonth;
    fields->Day = systemTime.wDay;
    fields->Hour = systemTime.wHour;
    fields->Minute = systemTime.wMinute;
    fields->Second = systemTime.wSecond;
    fields->Milliseconds = systemTime.wMilliseconds;
    fields->Weekday = systemTime.wDayOfWeek;
}

uint32_t RtlTimeFieldsToTime_x(XTIME_FIELDS* fields, be<uint64_t>* time)
{
    if (!fields || !time)
        return 0;

    SYSTEMTIME systemTime{};
    systemTime.wYear = uint16_t(fields->Year);
    systemTime.wMonth = uint16_t(fields->Month);
    systemTime.wDay = uint16_t(fields->Day);
    systemTime.wHour = uint16_t(fields->Hour);
    systemTime.wMinute = uint16_t(fields->Minute);
    systemTime.wSecond = uint16_t(fields->Second);
    systemTime.wMilliseconds = uint16_t(fields->Milliseconds);

    FILETIME fileTime{};
    if (!SystemTimeToFileTime(&systemTime, &fileTime))
        return 0;
    const uint64_t value = uint64_t(fileTime.dwLowDateTime) |
                           (uint64_t(fileTime.dwHighDateTime) << 32);
    *time = value;
    return 1;
}

uint32_t KeDelayExecutionThread_x(uint32_t mode, uint32_t alertable, be<int64_t>* interval)
{
    (void)mode;
    constexpr uint32_t kStatusUserApc = 0x000000C0u;
    if (alertable && DrainCurrentThreadApcs())
        return kStatusUserApc;
    const uint32_t ms = GuestTimeoutToMs(interval);
    const auto start = std::chrono::steady_clock::now();
    if (ms == 0)
        std::this_thread::yield();
    else if (ms != WAIT_INFINITE)
        HostSleepPrecise(ms);
    ReportBlockingWait("KeDelayExecutionThread", ms, start, kStatusSuccess);
    if (alertable && DrainCurrentThreadApcs())
        return kStatusUserApc;
    return kStatusSuccess;
}

uint32_t KeRaiseIrqlToDpcLevel_x() { return 0; }
void KfLowerIrql_x(uint32_t irql) { (void)irql; }

// ---------------------------------------------------------------------------
// Rtl / configuration / XAM basics
// ---------------------------------------------------------------------------

void RtlInitAnsiString_x(XANSI_STRING* dst, char* src)
{
    if (!dst)
        return;
    const uint16_t length = src ? static_cast<uint16_t>(std::strlen(src)) : 0;
    dst->Length = length;
    dst->MaximumLength = src ? length + 1 : 0;
    dst->Buffer = src;
}

uint32_t RtlCompareMemoryUlong_x(be<uint32_t>* src, uint32_t length, uint32_t pattern)
{
    if (!src)
        return 0;
    uint32_t matched = 0;
    for (uint32_t i = 0; i < length / 4; ++i)
    {
        if (uint32_t(src[i]) != pattern)
            break;
        matched += 4;
    }
    return matched;
}

void RtlFillMemoryUlong_x(be<uint32_t>* dst, uint32_t length, uint32_t pattern)
{
    if (!dst)
        return;
    for (uint32_t i = 0; i < length / 4; ++i)
        dst[i] = pattern;
}

uint32_t RtlNtStatusToDosError_x(uint32_t status)
{
    switch (status)
    {
    case kStatusSuccess: return 0;
    case kStatusTimeout: return 1460;
    case kStatusNoMemory: return 14;
    case kStatusInvalidHandle: return 6;
    case kStatusInvalidParameter: return 87;
    case kStatusNoSuchFile: return 2;
    case kStatusEndOfFile: return 38;
    case kStatusAccessDenied: return 5;
    case kStatusObjectNameCollision: return 183;
    case kStatusObjectPathNotFound: return 3;
    default: return 317;
    }
}

uint32_t ExGetXConfigSetting_x(uint16_t category, uint16_t setting, void* buffer,
                               uint16_t bufferSize, be<uint32_t>* requiredSize)
{
    uint32_t value = 0;
    if (category == 0x0002 && setting == 0x0002)
        value = 0x00001000; // NTSC / North America AV region
    else if (category == 0x0003)
    {
        switch (setting)
        {
        case 0x0009: value = mojorecomp::config::Get().xboxLanguage; break;
        case 0x000A: value = 0x00040000; break; // widescreen
        case 0x000C: value = 1; break;          // retail
        case 0x000E: value = 103; break;        // country
        default: value = 0; break;
        }
    }
    else
    {
        return kStatusUnsuccessful;
    }

    if (requiredSize)
        *requiredSize = 4;
    if (buffer && bufferSize)
    {
        be<uint32_t> encoded(value);
        std::memcpy(buffer, &encoded, std::min<uint32_t>(bufferSize, 4));
    }
    return kStatusSuccess;
}

uint32_t XexCheckExecutablePrivilege_x(uint32_t privilege)
{
    if (privilege > 31)
        return 0;
    const uint32_t field = XexHeaderField(0, XEX_HEADER_SYSTEM_FLAGS);
    if (!field)
        return 0;
    const uint32_t flags = *reinterpret_cast<const be<uint32_t>*>(g_guestMemory.Translate(field));
    return (flags & (1u << privilege)) ? 1u : 0u;
}

uint32_t RtlImageXexHeaderField_x(uint32_t headerBase, uint32_t key)
{
    return XexHeaderField(headerBase, key);
}

uint32_t XGetLanguage_x() { return mojorecomp::config::Get().xboxLanguage; }
uint32_t XGetAVPack_x() { return 0; }
uint32_t XGetGameRegion_x() { return 0x03FF; }
uint32_t XamGetSystemVersion_x() { return 0; }
uint32_t XamUserGetSigninState_x(uint32_t userIndex) { return userIndex == 0 ? 1u : 0u; }

// XInput ABI structures. Keep these local until the host input backend is split out;
// their layout is guest-visible and therefore explicitly endian-aware.
struct XInputGamepad
{
    be<uint16_t> buttons;
    uint8_t leftTrigger;
    uint8_t rightTrigger;
    be<int16_t> thumbLX;
    be<int16_t> thumbLY;
    be<int16_t> thumbRX;
    be<int16_t> thumbRY;
};
static_assert(sizeof(XInputGamepad) == 12);

struct XInputState
{
    be<uint32_t> packetNumber;
    XInputGamepad gamepad;
};
static_assert(sizeof(XInputState) == 16);

struct XInputVibration
{
    be<uint16_t> leftMotorSpeed;
    be<uint16_t> rightMotorSpeed;
};
static_assert(sizeof(XInputVibration) == 4);

struct XInputCapabilities
{
    uint8_t type;
    uint8_t subType;
    be<uint16_t> flags;
    XInputGamepad gamepad;
    XInputVibration vibration;
};
static_assert(sizeof(XInputCapabilities) == 20);

constexpr uint32_t kErrorDeviceNotConnected = 1167;
constexpr uint32_t kXInputFlagGamepad = 0x01;
constexpr uint32_t kXInputFlagAnyUser = 1u << 30;

uint32_t NormalizeInputUser(uint32_t userIndex, uint32_t flags)
{
    if ((userIndex & 0xFFu) == 0xFFu || (flags & kXInputFlagAnyUser))
        return 0;
    return userIndex;
}

uint32_t XamInputGetCapabilities_x(uint32_t userIndex, uint32_t flags,
                                   XInputCapabilities* caps)
{
    if (!caps)
        return kStatusInvalidParameter;
    std::memset(caps, 0, sizeof(*caps));
    if ((flags & 0xFFu) && !(flags & kXInputFlagGamepad))
        return kErrorDeviceNotConnected;

    userIndex = NormalizeInputUser(userIndex, flags);
    if (userIndex != 0)
        return kErrorDeviceNotConnected;

    // Report one ordinary Xbox 360 gamepad. The boot probe has no host window yet,
    // so state is neutral; the same ABI can later be fed by the native input backend.
    caps->type = 1;       // XINPUT_DEVTYPE_GAMEPAD
    caps->subType = 1;    // XINPUT_DEVSUBTYPE_GAMEPAD
    caps->gamepad.buttons = 0xF3FF;
    caps->gamepad.leftTrigger = 0xFF;
    caps->gamepad.rightTrigger = 0xFF;
    caps->gamepad.thumbLX = 0x7FFF;
    caps->gamepad.thumbLY = 0x7FFF;
    caps->gamepad.thumbRX = 0x7FFF;
    caps->gamepad.thumbRY = 0x7FFF;
    caps->vibration.leftMotorSpeed = 0xFFFF;
    caps->vibration.rightMotorSpeed = 0xFFFF;
    return 0;
}

uint32_t XamInputGetState_x(uint32_t userIndex, uint32_t flags, XInputState* state)
{
    if ((flags & 0xFFu) && !(flags & kXInputFlagGamepad))
        return kErrorDeviceNotConnected;
    userIndex = NormalizeInputUser(userIndex, flags);
    if (userIndex != 0)
    {
        if (state)
            std::memset(state, 0, sizeof(*state));
        return kErrorDeviceNotConnected;
    }
    if (!state)
        return kStatusInvalidParameter;

    static std::atomic<uint32_t> packet{1};
    HostInputState host{};
    HostInput_Poll(host);

    // F2 Unlock All is consumed here because this import is executed on the
    // active guest PPC thread immediately after the host hotkey poll. That lets
    // us call the title's own unlock routine with a valid PPC stack/TLS context
    // instead of mutating derived player state from a host/UI thread.
    if (mojorecomp::debug::ConsumeUnlockAllRequest())
    {
        constexpr uint32_t kMoveUnlockTable = 0x824A8138u;
        constexpr uint32_t kMoveUnlockInit = 0x824A81A4u;
        constexpr uint32_t kMoveCount = 27u;
        constexpr uint32_t kTutorialUnlockTable = 0x824A8278u;
        constexpr uint32_t kTutorialUnlockInit = 0x824A8329u;
        constexpr uint32_t kTutorialTableCapacity = 44u;
        constexpr uint32_t kTutorialCount = 21u;

        auto moveUnlocked = [](uint32_t id) -> bool {
            for (uint32_t slot = 0; slot < kMoveCount; ++slot)
            {
                const int32_t entry = static_cast<int32_t>(
                    *reinterpret_cast<const be<uint32_t>*>(
                        g_guestMemory.Translate(kMoveUnlockTable + slot * 4u)));
                if (entry == static_cast<int32_t>(id))
                    return true;
                if (entry == -1)
                    return false;
            }
            return false;
        };

        auto countUnlocked = [&]() -> uint32_t {
            uint32_t count = 0;
            for (uint32_t id = 0; id < kMoveCount; ++id)
                count += moveUnlocked(id) ? 1u : 0u;
            return count;
        };

        auto tutorialUnlocked = [](uint32_t id) -> bool {
            for (uint32_t slot = 0; slot < kTutorialTableCapacity; ++slot)
            {
                const int32_t entry = static_cast<int32_t>(
                    *reinterpret_cast<const be<uint32_t>*>(
                        g_guestMemory.Translate(kTutorialUnlockTable + slot * 4u)));
                if (entry == static_cast<int32_t>(id))
                    return true;
                if (entry == -1)
                    return false;
            }
            return false;
        };

        auto countTutorialsUnlocked = [&]() -> uint32_t {
            uint32_t count = 0;
            for (uint32_t id = 0; id < kTutorialCount; ++id)
                count += tutorialUnlocked(id) ? 1u : 0u;
            return count;
        };

        if (!g_ppcContext || !g_guestMemory.base)
        {
            mojorecomp::debug::ReportUnlockAllResult(false, 0);
        }
        else
        {
            struct ContextRestore
            {
                PPCContext* previous;
                ~ContextRestore() { g_ppcContext = previous; }
            } restore{g_ppcContext};

            // The move table's native initializer is normally run by the save
            // subsystem. Handle an early F2 safely as well, rather than treating
            // zero-filled guest BSS as 27 copies of move ID 0.
            if (*reinterpret_cast<const uint8_t*>(g_guestMemory.Translate(kMoveUnlockInit)) == 0)
            {
                PPCContext init = *restore.previous;
                g_ppcContext = &init;
                sub_82214868(init, g_guestMemory.base);
                g_ppcContext = restore.previous;
            }

            // Tutorial Stones use a separate persistent ID list. The tutorial
            // menu checks IDs 0..20 with sub_822161A8() to decide whether to
            // render the page contents or LOCKED. Preserve any existing IDs in
            // the 44-slot backing list; initialize it only if the save system
            // has not done so yet.
            if (*reinterpret_cast<const uint8_t*>(g_guestMemory.Translate(kTutorialUnlockInit)) == 0)
            {
                PPCContext init = *restore.previous;
                g_ppcContext = &init;
                sub_82216130(init, g_guestMemory.base);
                g_ppcContext = restore.previous;
            }

            const uint32_t before = countUnlocked();
            const uint32_t tutorialsBefore = countTutorialsUnlocked();
            if (before == kMoveCount && tutorialsBefore == kTutorialCount)
            {
                mojorecomp::debug::ReportUnlockAllResult(true, 0);
            }
            else
            {
                uint32_t movesUnlockedNow = 0;
                uint32_t tutorialsUnlockedNow = 0;
                bool failed = false;

                if (before != kMoveCount)
                {
                    auto* command = static_cast<uint8_t*>(g_guestHeap.Alloc(16));
                    failed = command == nullptr;
                    if (command)
                    {
                        std::memset(command, 0, 16);
                        const uint32_t commandGuest = g_guestMemory.MapVirtual(command);

                        for (uint32_t id = 0; id < kMoveCount; ++id)
                        {
                            if (moveUnlocked(id))
                                continue;

                            // sub_82214DE0 is the native DO_UnlockMove execution path.
                            // Its command object reads the EUnlockableMove value from
                            // +4, inserts it into the serialized 27-entry table and
                            // performs the title's Health/Spin/Slide refresh side effects.
                            *reinterpret_cast<be<uint32_t>*>(command + 4) = id;
                            PPCContext nested = *restore.previous;
                            nested.r3.u64 = commandGuest;
                            g_ppcContext = &nested;
                            sub_82214DE0(nested, g_guestMemory.base);
                            g_ppcContext = restore.previous;

                            if (!moveUnlocked(id))
                            {
                                std::fprintf(stderr,
                                             "[debug] Unlock All: native unlock failed for move id=%u\n",
                                             id);
                                failed = true;
                                break;
                            }
                            ++movesUnlockedNow;
                        }
                        g_guestHeap.Free(command);
                    }
                }

                // Unlock all 21 Tutorial Stone pages. sub_82216160 is the
                // title's native insertion routine for the same persistent list
                // queried by the tutorial menu's sub_822161A8(index) check.
                if (!failed)
                {
                    for (uint32_t id = 0; id < kTutorialCount; ++id)
                    {
                        if (tutorialUnlocked(id))
                            continue;

                        PPCContext nested = *restore.previous;
                        nested.r3.u64 = id;
                        g_ppcContext = &nested;
                        sub_82216160(nested, g_guestMemory.base);
                        g_ppcContext = restore.previous;

                        if (!tutorialUnlocked(id))
                        {
                            std::fprintf(stderr,
                                         "[debug] Unlock All: tutorial unlock failed for id=%u\n",
                                         id);
                            failed = true;
                            break;
                        }
                        ++tutorialsUnlockedNow;
                    }
                }

                const uint32_t after = countUnlocked();
                const uint32_t tutorialsAfter = countTutorialsUnlocked();
                std::fprintf(stderr,
                             "[debug] Unlock All: moves %u/%u -> %u/%u (+%u), "
                             "tutorials %u/%u -> %u/%u (+%u)\n",
                             before, kMoveCount, after, kMoveCount, movesUnlockedNow,
                             tutorialsBefore, kTutorialCount, tutorialsAfter,
                             kTutorialCount, tutorialsUnlockedNow);
                if (!failed && after == kMoveCount && tutorialsAfter == kTutorialCount)
                    mojorecomp::debug::ReportUnlockAllResult(
                        false, movesUnlockedNow + tutorialsUnlockedNow);
                else
                    mojorecomp::debug::ReportUnlockAllResult(false, 0);
            }
        }
    }

    // F4 Unlock All Episodes uses the title's retail 20-entry level-unlock
    // progression list. The HUB/timeline itself checks this list through
    // sub_82213940(), so no hidden QA GOD_MODE binding or forced game-state
    // transition is needed.
    if (mojorecomp::debug::ConsumeUnlockAllEpisodesRequest())
    {
        constexpr uint32_t kEpisodeUnlockInit = 0x824A80C0u;
        constexpr uint32_t kEpisodeCount = 20u;

        if (!g_ppcContext || !g_guestMemory.base)
        {
            mojorecomp::debug::ReportUnlockAllEpisodesResult(false, 0);
        }
        else
        {
            struct ContextRestore
            {
                PPCContext* previous;
                ~ContextRestore() { g_ppcContext = previous; }
            } restore{g_ppcContext};

            // sub_822137B0 resolves the title's canonical timeline index to the
            // same native level ID stored in the persistent unlock list. It
            // relies on the frontend/game singleton, so fail cleanly if F4 is
            // somehow pressed before that singleton exists.
            PPCContext managerCtx = *restore.previous;
            g_ppcContext = &managerCtx;
            sub_821FB4F0(managerCtx, g_guestMemory.base);
            g_ppcContext = restore.previous;
            if (!managerCtx.r3.u32)
            {
                mojorecomp::debug::ReportUnlockAllEpisodesResult(false, 0);
            }
            else
            {
                if (*reinterpret_cast<const uint8_t*>(
                        g_guestMemory.Translate(kEpisodeUnlockInit)) == 0)
                {
                    PPCContext init = *restore.previous;
                    g_ppcContext = &init;
                    sub_82213C18(init, g_guestMemory.base);
                    g_ppcContext = restore.previous;
                }

                auto episodeId = [&](uint32_t index) -> int32_t {
                    PPCContext nested = *restore.previous;
                    nested.r3.u64 = index;
                    g_ppcContext = &nested;
                    sub_822137B0(nested, g_guestMemory.base);
                    g_ppcContext = restore.previous;
                    return nested.r3.s32;
                };

                auto episodeUnlocked = [&](int32_t id) -> bool {
                    PPCContext nested = *restore.previous;
                    nested.r3.s64 = id;
                    g_ppcContext = &nested;
                    sub_82213940(nested, g_guestMemory.base);
                    g_ppcContext = restore.previous;
                    return nested.r3.u32 != 0;
                };

                uint32_t before = 0;
                std::array<int32_t, kEpisodeCount> ids{};
                bool failed = false;
                for (uint32_t index = 0; index < kEpisodeCount; ++index)
                {
                    ids[index] = episodeId(index);
                    if (ids[index] < 0)
                    {
                        std::fprintf(stderr,
                                     "[debug] Unlock All Episodes: failed to resolve index=%u\n",
                                     index);
                        failed = true;
                        break;
                    }
                    before += episodeUnlocked(ids[index]) ? 1u : 0u;
                }

                if (!failed && before == kEpisodeCount)
                {
                    mojorecomp::debug::ReportUnlockAllEpisodesResult(true, 0);
                }
                else if (!failed)
                {
                    auto* command = static_cast<uint8_t*>(g_guestHeap.Alloc(16));
                    if (!command)
                    {
                        failed = true;
                    }
                    else
                    {
                        std::memset(command, 0, 16);
                        const uint32_t commandGuest = g_guestMemory.MapVirtual(command);
                        uint32_t unlockedNow = 0;

                        for (uint32_t index = 0; index < kEpisodeCount; ++index)
                        {
                            const int32_t id = ids[index];
                            if (episodeUnlocked(id))
                                continue;

                            // Native level-unlock insertion path. The command
                            // reads the canonical level ID from +4, updates the
                            // serialized 20-entry list and refreshes HUB state.
                            *reinterpret_cast<be<uint32_t>*>(command + 4) =
                                static_cast<uint32_t>(id);
                            PPCContext nested = *restore.previous;
                            nested.r3.u64 = commandGuest;
                            g_ppcContext = &nested;
                            sub_82213F98(nested, g_guestMemory.base);
                            g_ppcContext = restore.previous;

                            if (!episodeUnlocked(id))
                            {
                                std::fprintf(stderr,
                                             "[debug] Unlock All Episodes: native unlock failed "
                                             "index=%u id=%d\n",
                                             index, id);
                                failed = true;
                                break;
                            }
                            ++unlockedNow;
                        }

                        g_guestHeap.Free(command);

                        uint32_t after = 0;
                        for (uint32_t index = 0; index < kEpisodeCount; ++index)
                            after += episodeUnlocked(ids[index]) ? 1u : 0u;

                        std::fprintf(stderr,
                                     "[debug] Unlock All Episodes: %u/%u -> %u/%u (+%u)\n",
                                     before, kEpisodeCount, after, kEpisodeCount,
                                     unlockedNow);
                        if (!failed && after == kEpisodeCount)
                            mojorecomp::debug::ReportUnlockAllEpisodesResult(false, unlockedNow);
                        else
                            mojorecomp::debug::ReportUnlockAllEpisodesResult(false, 0);
                    }

                    if (failed)
                        mojorecomp::debug::ReportUnlockAllEpisodesResult(false, 0);
                }
            }
        }
    }

    std::memset(state, 0, sizeof(*state));
    state->packetNumber = packet.fetch_add(1, std::memory_order_relaxed);
    state->gamepad.buttons = host.buttons;
    state->gamepad.leftTrigger = host.leftTrigger;
    state->gamepad.rightTrigger = host.rightTrigger;
    state->gamepad.thumbLX = host.thumbLX;
    state->gamepad.thumbLY = host.thumbLY;
    state->gamepad.thumbRX = host.thumbRX;
    state->gamepad.thumbRY = host.thumbRY;
    return 0;
}

uint32_t XamInputSetState_x(uint32_t userIndex, uint32_t flags, XInputVibration* vibration)
{
    (void)vibration;
    userIndex = NormalizeInputUser(userIndex, flags);
    return userIndex == 0 ? 0u : kErrorDeviceNotConnected;
}

struct XWsaData
{
    be<uint16_t> version;
    be<uint16_t> highVersion;
    char description[257];
    char systemStatus[129];
    be<uint16_t> maxSockets;
    be<uint16_t> maxUdpDatagram;
    uint8_t reserved[6];
    be<uint32_t> vendorInfo;
};
static_assert(sizeof(XWsaData) == 0x194);

uint32_t NetDll_WSAStartup_x(uint32_t caller, uint16_t version, XWsaData* data)
{
    (void)caller;
    if (data)
    {
        // Crash only needs Winsock initialization at this point. Preserve the vendor
        // field because some XDK clients seed and verify it across WSAStartup.
        const uint32_t vendor = data->vendorInfo;
        std::memset(data, 0, sizeof(*data));
        data->version = version;
        data->highVersion = version;
        data->maxSockets = 100;
        data->maxUdpDatagram = 0xFFFF;
        data->vendorInfo = vendor;
    }
    return 0;
}

struct XamOverlapped
{
    be<uint32_t> result;
    be<uint32_t> length;
    be<uint32_t> context;
    be<uint32_t> event;
    be<uint32_t> completionRoutine;
    be<uint32_t> completionContext;
    be<uint32_t> extendedError;
};
static_assert(sizeof(XamOverlapped) == 28);

void SetGuestLastError(uint32_t error)
{
    if (!g_ppcContext || !g_ppcContext->r13.u32)
        return;

    const uint32_t r13 = g_ppcContext->r13.u32;
    const uint32_t overridePtr =
        *reinterpret_cast<const be<uint32_t>*>(g_guestMemory.Translate(r13 + 336));
    if (overridePtr)
        return;

    const uint32_t thread =
        *reinterpret_cast<const be<uint32_t>*>(g_guestMemory.Translate(r13 + 256));
    if (thread)
        *reinterpret_cast<be<uint32_t>*>(g_guestMemory.Translate(thread + 352)) = error;
}

uint32_t CompleteXamIoRequest(uint32_t result, uint32_t overlappedAddress)
{
    if (!overlappedAddress)
    {
        if (result == 0)
            SetGuestLastError(0);
        return result;
    }

    constexpr uint32_t kIoPending = 997u;
    auto* overlapped = reinterpret_cast<XamOverlapped*>(
        g_guestMemory.Translate(overlappedAddress));
    const uint32_t event = overlapped->event;
    const uint32_t completionRoutine = overlapped->completionRoutine;
    const uint32_t completionContext = overlapped->completionContext;
    const uint32_t hostThreadId = GetCurrentThreadId();

    // Retail code may initialize or inspect the XOVERLAPPED immediately after
    // the export returns. Publishing completion before returning can therefore
    // be lost. Keep the request visibly pending, return first, then complete it
    // from a short deferred host task just like the real asynchronous XAM path.
    overlapped->result = kIoPending;
    overlapped->length = 0;
    overlapped->extendedError = kIoPending;

    static std::atomic<uint32_t> reports{0};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 24)
        KLOG("XAM async begin ov=%08X result=%u event=%08X callback=%08X context=%08X thread=%u\n",
             overlappedAddress, result, event, completionRoutine, completionContext,
             hostThreadId);

    std::thread([overlappedAddress, result, event, completionRoutine, hostThreadId] {
        HostSleepPrecise(1);
        auto* completed = reinterpret_cast<XamOverlapped*>(
            g_guestMemory.Translate(overlappedAddress));
        completed->length = 0;
        completed->extendedError = result;
        completed->result = result;
        if (event)
            SignalHandleEvent(event);
        if (completionRoutine)
            QueueXamCompletion(hostThreadId, completionRoutine, result, 0,
                               overlappedAddress);

        static std::atomic<uint32_t> completions{0};
        if (completions.fetch_add(1, std::memory_order_relaxed) < 24)
            KLOG("XAM async complete ov=%08X result=%u event=%08X callback=%08X thread=%u\n",
                 overlappedAddress, result, event, completionRoutine, hostThreadId);
    }).detach();

    SetGuestLastError(0);
    return kIoPending;
}

struct XUserReadProfileSettings
{
    be<uint32_t> settingCount;
    be<uint32_t> settingsPtr;
};
static_assert(sizeof(XUserReadProfileSettings) == 8);

struct XUserProfileSetting
{
    be<uint32_t> from;
    be<uint32_t> unknown04;
    be<uint64_t> userOrXuid;
    be<uint32_t> settingId;
    be<uint32_t> unknown14;
    uint8_t data[16];
};
static_assert(sizeof(XUserProfileSetting) == 40);

constexpr uint32_t kXErrorInvalidParameter = 87;
constexpr uint32_t kXErrorInsufficientBuffer = 122;
constexpr uint32_t kXErrorIoPending = 997;
constexpr uint32_t kXErrorNoSuchUser = 1317;

uint32_t ProfileScalarValue(uint32_t settingId)
{
    switch (settingId)
    {
    case 0x10040002: return 0;          // Y-axis inversion
    case 0x10040003: return 3;          // controller vibration
    case 0x10040004: return 0;          // gamer zone
    case 0x10040005: return 0;          // region
    case 0x10040006: return 0xFA;       // gamer cred
    case 0x1004000C: return 0;          // voice muted
    case 0x1004000D: return 0;          // voice through speakers
    case 0x1004000E: return 0x64;       // voice volume
    case 0x10040012: return 1;          // titles played
    case 0x10040013: return 0;          // achievements earned
    case 0x10040015: return 0;          // gamer difficulty
    case 0x10040018: return 0;          // control sensitivity
    case 0x1004001D: return 0xFFFF0000; // preferred colour 1
    case 0x1004001E: return 0xFF00FF00; // preferred colour 2
    case 0x10040022: return 1;          // auto aim
    case 0x10040023: return 0;          // auto center
    case 0x10040024: return 0;          // movement control
    case 0x10040026: return 0;          // race transmission
    case 0x10040027: return 0;          // race camera
    case 0x10040028: return 0;          // race brake
    case 0x10040029: return 0;          // race accelerator
    case 0x10040038: return 0;          // title cred earned
    case 0x10040039: return 0;          // title achievements earned
    default: return 0;
    }
}

uint32_t CompleteProfileCall(uint32_t status, XamOverlapped* overlapped)
{
    if (!overlapped)
        return status;

    // The title's boot request supplies a zero-initialized OVERLAPPED without an
    // event or completion routine, and later polls Result. Complete it immediately
    // while preserving XAM's asynchronous return contract.
    overlapped->result = status;
    overlapped->length = 0;
    overlapped->extendedError = status;
    return kXErrorIoPending;
}

uint32_t XamUserReadProfileSettings_x(uint32_t titleId, uint32_t userIndex,
                                      uint32_t xuidCount, be<uint64_t>* xuids,
                                      uint32_t settingCount, be<uint32_t>* settingIds,
                                      be<uint32_t>* bufferSizePtr, uint8_t* buffer,
                                      XamOverlapped* overlapped)
{
    (void)titleId;
    if (settingCount == 0 || settingCount > 32 || !settingIds || !bufferSizePtr)
        return CompleteProfileCall(kXErrorInvalidParameter, overlapped);
    if (xuidCount > 1 || (xuidCount != 0 && !xuids))
        return CompleteProfileCall(kXErrorInvalidParameter, overlapped);
    if (xuidCount == 0 && userIndex != 0)
        return CompleteProfileCall(kXErrorNoSuchUser, overlapped);

    uint32_t dataBytes = 0;
    for (uint32_t i = 0; i < settingCount; ++i)
    {
        const uint32_t id = settingIds[i];
        const uint32_t type = id >> 28;
        if (type == 4 || type == 6)
            dataBytes += (id >> 16) & 0xFFFu;
    }
    const uint32_t entries = settingCount * (xuidCount ? xuidCount : 1u);
    const uint32_t headerBytes = sizeof(XUserReadProfileSettings) +
                                 entries * sizeof(XUserProfileSetting);
    const uint32_t needed = headerBytes + dataBytes * (xuidCount ? xuidCount : 1u);
    const uint32_t supplied = *bufferSizePtr;

    if (!buffer || supplied < needed)
    {
        *bufferSizePtr = needed;
        KLOG("XamUserReadProfileSettings size query: user=%u count=%u -> %u bytes%s\n",
             userIndex, settingCount, needed, overlapped ? " (async)" : "");
        return CompleteProfileCall(kXErrorInsufficientBuffer, overlapped);
    }

    std::memset(buffer, 0, needed);
    auto* header = reinterpret_cast<XUserReadProfileSettings*>(buffer);
    auto* out = reinterpret_cast<XUserProfileSetting*>(buffer + sizeof(*header));
    uint8_t* stream = buffer + headerBytes;
    header->settingCount = entries;
    header->settingsPtr = g_guestMemory.MapVirtual(out);

    uint32_t outIndex = 0;
    const uint32_t users = xuidCount ? xuidCount : 1u;
    for (uint32_t u = 0; u < users; ++u)
    {
        for (uint32_t i = 0; i < settingCount; ++i, ++outIndex)
        {
            const uint32_t id = settingIds[i];
            const uint32_t type = id >> 28;
            const uint32_t declaredSize = (id >> 16) & 0xFFFu;
            XUserProfileSetting& s = out[outIndex];
            s.from = 1; // profile/global setting
            if (xuidCount)
                s.userOrXuid = xuids[u];
            else
                s.userOrXuid = uint64_t(userIndex) << 32;
            s.settingId = id;
            s.data[0] = static_cast<uint8_t>(type);

            // The payload union starts at data+8 in the Xbox ABI.
            auto* payload32 = reinterpret_cast<be<uint32_t>*>(s.data + 8);
            if (type == 1) // INT32
            {
                payload32[0] = ProfileScalarValue(id);
            }
            else if (type == 5) // FLOAT
            {
                float value = 0.0f;
                uint32_t bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                payload32[0] = bits;
            }
            else if (type == 4 || type == 6) // WSTRING / BINARY
            {
                payload32[0] = declaredSize;
                payload32[1] = g_guestMemory.MapVirtual(stream);
                std::memset(stream, 0, declaredSize);

                if (type == 4 && id == 0x4064000F && declaredSize >= 2)
                {
                    constexpr char16_t kPictureKey[] = u"gamercard_picture_key";
                    const uint32_t chars = std::min<uint32_t>(
                        static_cast<uint32_t>(std::size(kPictureKey)), declaredSize / 2);
                    auto* dst = reinterpret_cast<be<uint16_t>*>(stream);
                    for (uint32_t c = 0; c < chars; ++c)
                        dst[c] = static_cast<uint16_t>(kPictureKey[c]);
                }
                stream += declaredSize;
            }
        }
    }

    *bufferSizePtr = needed;
    if (settingCount <= 8)
    {
        std::fprintf(stderr, "[kernel] profile settings:");
        for (uint32_t i = 0; i < settingCount; ++i)
            std::fprintf(stderr, " %08X", uint32_t(settingIds[i]));
        std::fprintf(stderr, "%s\n", overlapped ? " (async)" : "");
    }
    return CompleteProfileCall(0, overlapped);
}

void FillVideoMode(XVIDEO_MODE* mode)
{
    if (!mode)
        return;
    std::memset(mode, 0, sizeof(*mode));
    mode->DisplayWidth = 1280;
    mode->DisplayHeight = 720;
    mode->IsWidescreen = 1;
    mode->IsHighDefinition = 1;
    float refresh = 60.0f;
    uint32_t refreshBits = 0;
    std::memcpy(&refreshBits, &refresh, sizeof(refreshBits));
    mode->RefreshRate = refreshBits;
    mode->VideoStandard = 1;
    mode->Unknown4A = 0x4A;
    mode->Unknown01 = 1;
}

void XGetVideoMode_x(XVIDEO_MODE* mode) { FillVideoMode(mode); }
void VdQueryVideoMode_x(XVIDEO_MODE* mode) { FillVideoMode(mode); }
uint32_t VdQueryVideoFlags_x() { return 3; }

uint32_t XMsgInProcessCall_x(uint32_t app, uint32_t message,
                            be<uint32_t>* args, uint32_t arg2)
{
    (void)arg2;

    // Crash of the Titans uses the XMP app (0xFA) for the dashboard media
    // player queries made by its audio/menu code. Mirror the corresponding
    // Xenia/ReXGlue app handlers rather than treating all XMsg calls as a stub.
    if (app == 0xFAu && message == 0x00070009u)
    {
        // XMPGetStatus: { client, state_ptr }. The XMP app starts in Idle (0).
        if (!args || uint32_t(args[0]) != 2u)
            return 0x80070057u; // E_INVALIDARG
        const uint32_t statePtr = args[1];
        if (statePtr)
            *reinterpret_cast<be<uint32_t>*>(g_guestMemory.Translate(statePtr)) = 0u;
        return 0u;
    }

    if (app == 0xFAu && message == 0x0007001Bu)
    {
        // XMPGetPlaybackController: { client, controller_ptr, locked_ptr }.
        if (!args || uint32_t(args[0]) != 2u)
            return 0x80070057u;
        const uint32_t controllerPtr = args[1];
        const uint32_t lockedPtr = args[2];
        if (controllerPtr)
            *reinterpret_cast<be<uint32_t>*>(g_guestMemory.Translate(controllerPtr)) = 0u;
        if (lockedPtr)
            *reinterpret_cast<be<uint32_t>*>(g_guestMemory.Translate(lockedPtr)) = 0u;
        return 0u;
    }

    KLOG("XMsgInProcessCall unsupported app=%08X message=%08X args=%08X arg2=%08X\n",
         app, message, args ? g_guestMemory.MapVirtual(args) : 0u, arg2);
    return 0x80070490u; // HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
}

uint32_t DispatchXMsgIoRequest(uint32_t app, uint32_t message,
                              be<uint32_t>* buffer, uint32_t bufferLength,
                              uint32_t overlapped)
{
    // Dashboard media player app (XMP).
    if (app == 0xFAu && message == 0x0007001Au &&
        buffer && bufferLength == 12u)
    {
        const uint32_t client = buffer[0];
        const uint32_t controller = buffer[1];
        const uint32_t playbackClient = buffer[2];
        const bool valid = (client == 2u && controller == 0u) ||
                           (client == 0u && controller == 1u);
        if (!valid)
            return 0x80070057u;

        static std::atomic<uint32_t> reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
            KLOG("XMPSetPlaybackController client=%u controller=%u playback=%u overlapped=%08X\n",
                 client, controller, playbackClient, overlapped);
        return 0u;
    }

    // Xbox Game Interface app (XGI). These are the two messages used by the
    // title's boot/profile path. Their payload layouts match the retail XAM ABI.
    if (app == 0xFBu && message == 0x000B0006u &&
        buffer && bufferLength == 24u)
    {
        const uint32_t userIndex = buffer[0];
        const uint32_t contextId = buffer[4];
        const uint32_t contextValue = buffer[5];
        static std::atomic<uint32_t> reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 16)
            KLOG("XGIUserSetContextEx user=%u context=%08X value=%08X overlapped=%08X\n",
                 userIndex, contextId, contextValue, overlapped);
        return 0u;
    }

    if (app == 0xFBu && message == 0x000B0008u &&
        buffer && bufferLength == 8u)
    {
        const uint32_t achievementCount = buffer[0];
        const uint32_t achievementsPtr = buffer[1];
        static std::atomic<uint32_t> reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
            KLOG("XGIUserWriteAchievements count=%u entries=%08X overlapped=%08X\n",
                 achievementCount, achievementsPtr, overlapped);
        return 0u;
    }

    KLOG("XMsgStartIORequest unsupported app=%08X message=%08X ov=%08X buffer=%08X len=%u\n",
         app, message, overlapped,
         buffer ? g_guestMemory.MapVirtual(buffer) : 0u, bufferLength);
    return 0x80070490u;
}

uint32_t XMsgStartIORequestEx_x(uint32_t app, uint32_t message,
                               uint32_t overlapped, be<uint32_t>* buffer,
                               uint32_t bufferLength, be<uint32_t>* unknown)
{
    (void)unknown;
    const uint32_t result =
        DispatchXMsgIoRequest(app, message, buffer, bufferLength, overlapped);
    return CompleteXamIoRequest(result, overlapped);
}

uint32_t XMsgStartIORequest_x(uint32_t app, uint32_t message,
                             uint32_t overlapped, be<uint32_t>* buffer,
                             uint32_t bufferLength)
{
    const uint32_t result =
        DispatchXMsgIoRequest(app, message, buffer, bufferLength, overlapped);
    return CompleteXamIoRequest(result, overlapped);
}

uint32_t XamContentGetDeviceData_x(uint32_t deviceId, XDEVICE_DATA* data)
{
    constexpr uint32_t kUserdataDeviceId = 1;
    if (!data || deviceId != kUserdataDeviceId)
        return kXErrorInvalidParameter;

    std::memset(data, 0, sizeof(*data));
    data->DeviceID = kUserdataDeviceId;
    data->DeviceType = XCONTENTDEVICETYPE_HDD;

    uint64_t capacity = 64ull * 1024ull * 1024ull * 1024ull;
    uint64_t available = 32ull * 1024ull * 1024ull * 1024ull;
    const std::string saveRoot = VfsTranslate("save:", true);
    if (!saveRoot.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(saveRoot, ec);
        ec.clear();
        const auto space = std::filesystem::space(saveRoot, ec);
        if (!ec)
        {
            capacity = space.capacity;
            available = space.available;
        }
    }
    data->ulDeviceBytes = capacity;
    data->ulDeviceFreeBytes = available;

    constexpr char16_t kDeviceName[] = u"MojoRecomp Storage";
    constexpr size_t kMaxName = XCONTENTDEVICE_MAX_NAME;
    for (size_t i = 0; i + 1 < kMaxName && kDeviceName[i]; ++i)
        data->wszName[i] = static_cast<uint16_t>(kDeviceName[i]);

    KLOG("XamContentGetDeviceData device=%u capacity=%llu free=%llu\n",
         deviceId, static_cast<unsigned long long>(capacity),
         static_cast<unsigned long long>(available));
    return 0;
}

uint32_t XamContentGetDeviceState_x(uint32_t deviceId, XamOverlapped* overlapped)
{
    constexpr uint32_t kUserdataDeviceId = 1;
    const uint32_t result = deviceId == kUserdataDeviceId ? 0u : kXErrorInvalidParameter;
    if (!overlapped)
    {
        if (result == 0)
            SetGuestLastError(0);
        return result;
    }
    return CompleteXamIoRequest(result, g_guestMemory.MapVirtual(overlapped));
}

std::string ContentFileName(const XCONTENT_DATA* data)
{
    if (!data)
        return {};
    size_t length = 0;
    while (length < XCONTENT_MAX_FILENAME && data->szFileName[length])
        ++length;
    return std::string(data->szFileName, data->szFileName + length);
}

std::string SafeContentComponent(std::string value)
{
    // XCONTENT filenames are flat container identifiers. Keep them host-safe and
    // deterministic rather than allowing separators or traversal components to
    // escape the userdata directory.
    if (value.empty() || value == "." || value == "..")
        return {};
    for (char& c : value)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || uc < 0x20)
            c = '_';
    }
    return value;
}

std::vector<XCONTENT_DATA> EnumerateSavedContent(uint32_t contentType)
{
    std::vector<XCONTENT_DATA> items;
    const std::string saveRoot = VfsTranslate("save:", true);
    if (saveRoot.empty())
        return items;

    std::error_code ec;
    std::filesystem::create_directories(saveRoot, ec);
    ec.clear();
    for (const auto& entry : std::filesystem::directory_iterator(saveRoot, ec))
    {
        if (ec)
            break;
        std::error_code typeEc;
        if (!entry.is_directory(typeEc) || typeEc)
            continue;
        const std::string filename = entry.path().filename().string();
        if (filename.empty() || filename[0] == '.')
            continue;

        XCONTENT_DATA data{};
        data.DeviceID = 1;
        data.dwContentType = contentType;
        const size_t fileChars = std::min(filename.size(), size_t(XCONTENT_MAX_FILENAME - 1));
        std::memcpy(data.szFileName, filename.data(), fileChars);
        const size_t displayChars = std::min(filename.size(), size_t(XCONTENT_MAX_DISPLAYNAME - 1));
        for (size_t i = 0; i < displayChars; ++i)
            data.szDisplayName[i] = static_cast<uint16_t>(static_cast<unsigned char>(filename[i]));
        items.push_back(data);
    }
    return items;
}

uint32_t XamContentCreateEnumerator_x(uint32_t userIndex, uint32_t deviceId,
                                      uint32_t contentType, uint32_t contentFlags,
                                      uint32_t itemsPerEnumerate,
                                      be<uint32_t>* bufferSizeOut,
                                      be<uint32_t>* handleOut)
{
    (void)contentFlags;
    constexpr uint32_t kUserdataDeviceId = 1;
    if (!handleOut || itemsPerEnumerate == 0 ||
        (userIndex != 0 && userIndex != 0xFFu) ||
        (deviceId != 0 && deviceId != kUserdataDeviceId))
    {
        if (bufferSizeOut)
            *bufferSizeOut = 0;
        return kXErrorInvalidParameter;
    }

    if (bufferSizeOut)
        *bufferSizeOut = static_cast<uint32_t>(sizeof(XCONTENT_DATA) * itemsPerEnumerate);

    // Keep the enumerator as a real kernel object even when the user has no
    // saves yet. XamEnumerate then reports ERROR_NO_MORE_FILES, matching the
    // normal first-run path used by the title. Host directories created by
    // XamContentCreateEx are surfaced again on subsequent runs.
    std::vector<XCONTENT_DATA> items = EnumerateSavedContent(contentType);
    const size_t itemCount = items.size();
    const uint32_t handle = AddHandle(std::make_shared<ContentEnumerator>(std::move(items)));
    *handleOut = handle;
    KLOG("XamContentCreateEnumerator user=%u device=%u type=%u flags=%08X max=%u -> %08X items=%zu\n",
         userIndex, deviceId, contentType, contentFlags, itemsPerEnumerate, handle, itemCount);
    return 0;
}

uint32_t XamEnumerate_x(uint32_t handle, uint32_t flags, void* buffer,
                        uint32_t bufferBytes, be<uint32_t>* countOut,
                        XamOverlapped* overlapped)
{
    (void)flags;
    auto enumerator = std::dynamic_pointer_cast<ContentEnumerator>(FindHandle(handle));
    uint32_t count = 0;
    const uint32_t result = enumerator
        ? enumerator->Read(buffer, bufferBytes, &count)
        : kXErrorInvalidParameter;
    if (countOut)
        *countOut = count;

    static std::atomic<uint32_t> reports{0};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 16)
        KLOG("XamEnumerate handle=%08X bytes=%u -> result=%u count=%u ov=%08X\n",
             handle, bufferBytes, result, count,
             overlapped ? g_guestMemory.MapVirtual(overlapped) : 0u);

    if (!overlapped)
        return result;
    return CompleteXamIoRequest(result, g_guestMemory.MapVirtual(overlapped));
}

uint32_t XamContentCreateEx_x(uint32_t userIndex, const char* rootName,
                              XCONTENT_DATA* contentData, uint32_t flags,
                              be<uint32_t>* dispositionOut,
                              be<uint32_t>* licenseMaskOut, uint32_t cacheSize,
                              uint64_t contentSize, XamOverlapped* overlapped)
{
    (void)cacheSize;
    (void)contentSize;
    constexpr uint32_t kUserdataDeviceId = 1;
    constexpr uint32_t kErrorFileNotFound = 2;
    constexpr uint32_t kErrorAlreadyExists = 183;
    constexpr uint32_t kErrorInvalidName = 123;

    auto finish = [&](uint32_t result) {
        if (!overlapped)
        {
            if (result == 0)
                SetGuestLastError(0);
            return result;
        }
        return CompleteXamIoRequest(result, g_guestMemory.MapVirtual(overlapped));
    };

    if (!rootName || !*rootName || !contentData ||
        (userIndex != 0 && userIndex != 0xFEu && userIndex != 0xFFu))
        return finish(kXErrorInvalidParameter);

    const uint32_t deviceId = contentData->DeviceID;
    if (deviceId != kUserdataDeviceId)
        return finish(kXErrorInvalidParameter);

    const std::string filename = SafeContentComponent(ContentFileName(contentData));
    if (filename.empty())
        return finish(kErrorInvalidName);

    std::string mountName(rootName);
    if (!mountName.empty() && mountName.back() == ':')
        mountName.pop_back();
    mountName = SafeContentComponent(mountName);
    if (mountName.empty())
        return finish(kErrorInvalidName);

    const std::string saveRoot = VfsTranslate("save:", true);
    if (saveRoot.empty())
        return finish(kStatusObjectPathNotFound);

    const std::filesystem::path container = std::filesystem::path(saveRoot) / filename;
    std::error_code ec;
    const bool existed = std::filesystem::is_directory(container, ec);
    ec.clear();
    const uint32_t disposition = flags & 0xFu;
    bool shouldCreate = false;
    bool shouldReplace = false;
    uint32_t dispositionResult = XCONTENT_EXISTING;
    switch (disposition)
    {
    case 1: // CREATE_NEW
        if (existed)
            return finish(kErrorAlreadyExists);
        shouldCreate = true;
        dispositionResult = XCONTENT_NEW;
        break;
    case 2: // CREATE_ALWAYS
        // CREATE_ALWAYS is not OPEN_ALWAYS: an existing content container is
        // deleted and recreated. Retail titles use the returned disposition to
        // decide whether initialization/writes for a fresh save are required.
        // Reusing the existing directory here made Crash enter its overwrite
        // path with XCONTENT_EXISTING and leave the Saving UI pending forever.
        shouldReplace = existed;
        shouldCreate = true;
        dispositionResult = XCONTENT_NEW;
        break;
    case 3: // OPEN_EXISTING
        if (!existed)
            return finish(kErrorFileNotFound);
        dispositionResult = XCONTENT_EXISTING;
        break;
    case 4: // OPEN_ALWAYS
        shouldCreate = !existed;
        dispositionResult = existed ? XCONTENT_EXISTING : XCONTENT_NEW;
        break;
    case 5: // TRUNCATE_EXISTING
        if (!existed)
            return finish(kErrorFileNotFound);
        shouldReplace = true;
        shouldCreate = true;
        dispositionResult = XCONTENT_NEW;
        break;
    default:
        return finish(kXErrorInvalidParameter);
    }

    if (shouldReplace)
    {
        // Drop an old mapping before replacing the host directory. This mirrors
        // the content-manager create path rather than leaving the previous
        // container mounted while CREATE_ALWAYS recreates it.
        VfsUnmountDevice(mountName);
        std::filesystem::remove_all(container, ec);
        if (ec)
            return finish(kStatusAccessDenied);
        ec.clear();
    }

    if (shouldCreate)
    {
        std::filesystem::create_directories(container, ec);
        if (ec)
            return finish(kStatusAccessDenied);
    }

    VfsMountDevice(mountName, container.string(), true);
    if (dispositionOut)
        *dispositionOut = dispositionResult;
    if (licenseMaskOut)
        *licenseMaskOut = 0;

    KLOG("XamContentCreateEx user=%u root='%s' file='%s' flags=%08X -> %s (%s, disposition=%u)\n",
         userIndex, mountName.c_str(), filename.c_str(), flags,
         container.string().c_str(), shouldReplace ? "recreated" : (existed ? "existing" : "new"),
         dispositionResult);
    return finish(0);
}

uint32_t XamContentClose_x(const char* rootName, XamOverlapped* overlapped)
{
    uint32_t result = 0;
    if (!rootName || !*rootName)
    {
        result = kXErrorInvalidParameter;
    }
    else
    {
        std::string mountName(rootName);
        if (!mountName.empty() && mountName.back() == ':')
            mountName.pop_back();
        VfsUnmountDevice(mountName);
        KLOG("XamContentClose root='%s'\n", mountName.c_str());
    }
    if (!overlapped)
        return result;
    return CompleteXamIoRequest(result, g_guestMemory.MapVirtual(overlapped));
}

bool SaveDiagnosticsEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_SAVE_DIAGNOSTICS");
        return value && value[0] == '1';
    }();
    return enabled;
}

void ReportCrashSaveManager(const char* stage, uint32_t managerAddress, uint32_t slotIndex)
{
    if (!SaveDiagnosticsEnabled() || !managerAddress)
        return;

    auto load32 = [&](uint32_t offset) -> uint32_t {
        return *reinterpret_cast<const be<uint32_t>*>(
            g_guestMemory.Translate(managerAddress + offset));
    };
    auto load8 = [&](uint32_t offset) -> uint32_t {
        return *reinterpret_cast<const uint8_t*>(
            g_guestMemory.Translate(managerAddress + offset));
    };

    const uint32_t slotBase = 10172u + slotIndex * 16u;
    KLOG("[save probe] %s manager=%08X slot=%u state=%u active=%u flags=%u/%u/%u aux=%u "
         "slot.handle=%08X slot.word=%08X slot.flushFlag=%u\n",
         stage, managerAddress, slotIndex, load32(208), load32(216),
         load8(220), load8(221), load8(222), load32(312),
         load32(slotBase), load32(slotBase + 8u), load8(slotBase + 12u));
}

bool FindCrashSaveManagerFromRoot(const char* rootName, uint32_t& managerAddress,
                                  uint32_t& slotIndex)
{
    managerAddress = 0;
    slotIndex = 0;
    if (!rootName || rootName[0] != 's' || rootName[1] < '0' || rootName[1] > '7' ||
        (rootName[2] != '\0' && rootName[2] != ':'))
        return false;

    slotIndex = static_cast<uint32_t>(rootName[1] - '0');
    const uint32_t rootAddress = g_guestMemory.MapVirtual(rootName);
    const uint32_t rootOffset = 10176u + slotIndex * 16u;
    if (rootAddress < rootOffset)
        return false;
    managerAddress = rootAddress - rootOffset;
    return managerAddress != 0;
}

uint32_t XamContentFlush_x(const char* rootName, XamOverlapped* overlapped)
{
    uint32_t result = (rootName && *rootName) ? 0u : kXErrorInvalidParameter;
    uint32_t flushed = 0;
    if (result == 0)
    {
        std::string prefix(rootName);
        if (prefix.back() != ':')
            prefix.push_back(':');

        std::vector<std::shared_ptr<FileObject>> files;
        {
            std::lock_guard guard(g_handleMutex);
            files.reserve(g_handles.size());
            for (const auto& [handle, object] : g_handles)
            {
                (void)handle;
                if (auto file = std::dynamic_pointer_cast<FileObject>(object))
                    files.push_back(std::move(file));
            }
        }

        for (const auto& file : files)
        {
            if (!file->fp || !file->writable || file->guestPath.rfind(prefix, 0) != 0)
                continue;
            std::lock_guard ioLock(file->io);
            if (std::fflush(file->fp) != 0)
            {
                result = kStatusUnsuccessful;
                break;
            }
#if defined(_WIN32)
            if (_commit(_fileno(file->fp)) != 0)
            {
                result = kStatusUnsuccessful;
                break;
            }
#endif
            ++flushed;
        }
        KLOG("XamContentFlush root='%s' files=%u result=%08X ov=%08X lr=%08X\n",
             prefix.c_str(), flushed, result,
             overlapped ? g_guestMemory.MapVirtual(overlapped) : 0u,
             g_ppcContext ? static_cast<uint32_t>(g_ppcContext->lr) : 0u);

        // Crash keeps eight 16-byte content slots at manager+10172. Each mount
        // name is stored inline at slot+4 ("s0".."s7"), so the guest pointer
        // identifies both the slot and its owning manager without assuming that
        // a particular save operation happens to use slot zero.
        uint32_t managerAddress = 0;
        uint32_t slotIndex = 0;
        if (SaveDiagnosticsEnabled() &&
            FindCrashSaveManagerFromRoot(rootName, managerAddress, slotIndex))
        {
            ReportCrashSaveManager("flush-return", managerAddress, slotIndex);
            std::thread([managerAddress, slotIndex] {
                HostSleepPrecise(2);
                ReportCrashSaveManager("flush+2ms", managerAddress, slotIndex);
                HostSleepPrecise(18);
                ReportCrashSaveManager("flush+20ms", managerAddress, slotIndex);
                HostSleepPrecise(80);
                ReportCrashSaveManager("flush+100ms", managerAddress, slotIndex);
            }).detach();
        }
    }
    if (!overlapped)
        return result;
    return CompleteXamIoRequest(result, g_guestMemory.MapVirtual(overlapped));
}

uint32_t XamContentDelete_x(uint32_t userIndex, XCONTENT_DATA* contentData,
                            XamOverlapped* overlapped)
{
    constexpr uint32_t kErrorFileNotFound = 2;
    uint32_t result = 0;

    if (!contentData ||
        (userIndex != 0 && userIndex != 0xFEu && userIndex != 0xFFu))
    {
        result = kXErrorInvalidParameter;
    }
    else
    {
        const std::string filename = SafeContentComponent(ContentFileName(contentData));
        const std::string saveRoot = VfsTranslate("save:", true);
        if (filename.empty() || saveRoot.empty())
        {
            result = kXErrorInvalidParameter;
        }
        else
        {
            const std::filesystem::path container =
                std::filesystem::path(saveRoot) / filename;
            std::error_code ec;
            const bool exists = std::filesystem::is_directory(container, ec) && !ec;
            ec.clear();
            if (!exists)
            {
                result = kErrorFileNotFound;
            }
            else
            {
                // XamContentDelete removes the whole content container, not only
                // the payload file. This is the path retail titles use before a
                // CREATE_ALWAYS overwrite.
                std::filesystem::remove_all(container, ec);
                if (ec)
                    result = kStatusAccessDenied;
            }

            KLOG("XamContentDelete user=%u file='%s' exists=%u result=%08X ov=%08X\n",
                 userIndex, filename.c_str(), exists ? 1u : 0u, result,
                 overlapped ? g_guestMemory.MapVirtual(overlapped) : 0u);
        }
    }

    if (!overlapped)
    {
        if (result == 0)
            SetGuestLastError(0);
        return result;
    }
    return CompleteXamIoRequest(result, g_guestMemory.MapVirtual(overlapped));
}

uint32_t XamContentGetCreator_x(uint32_t userIndex, XCONTENT_DATA* contentData,
                                be<uint32_t>* isCreatorOut,
                                be<uint64_t>* creatorXuidOut,
                                XamOverlapped* overlapped)
{
    constexpr uint32_t kErrorPathNotFound = 3;
    constexpr uint64_t kLocalXuid = 0xE000000000000001ull;

    uint32_t result = 0;
    if (userIndex != 0 || !contentData || !isCreatorOut)
    {
        result = kXErrorInvalidParameter;
    }
    else
    {
        const std::string filename = SafeContentComponent(ContentFileName(contentData));
        const std::string saveRoot = VfsTranslate("save:", true);
        bool exists = false;
        if (!filename.empty() && !saveRoot.empty())
        {
            std::error_code ec;
            exists = std::filesystem::is_directory(
                std::filesystem::path(saveRoot) / filename, ec) && !ec;
        }

        if (!exists)
        {
            *isCreatorOut = 0;
            if (creatorXuidOut)
                *creatorXuidOut = 0;
            result = kErrorPathNotFound;
        }
        else if (uint32_t(contentData->dwContentType) == XCONTENTTYPE_SAVEDATA)
        {
            *isCreatorOut = 1;
            if (creatorXuidOut)
                *creatorXuidOut = kLocalXuid;
        }
        else
        {
            *isCreatorOut = 0;
            if (creatorXuidOut)
                *creatorXuidOut = 0;
        }

        KLOG("XamContentGetCreator user=%u file='%s' exists=%u creator=%u ov=%08X\n",
             userIndex, filename.c_str(), exists ? 1u : 0u,
             uint32_t(*isCreatorOut),
             overlapped ? g_guestMemory.MapVirtual(overlapped) : 0u);
    }

    if (!overlapped)
    {
        if (result == 0)
            SetGuestLastError(0);
        return result;
    }
    return CompleteXamIoRequest(result, g_guestMemory.MapVirtual(overlapped));
}

uint32_t XamShowDeviceSelectorUI_x(uint32_t userIndex, uint32_t contentType,
                                   uint32_t contentFlags, uint64_t totalRequested,
                                   be<uint32_t>* deviceId, XamOverlapped* overlapped)
{
    constexpr uint32_t kXnSysUi = 0x00000009u;
    constexpr uint32_t kXnSysStorageDevicesChanged = 0x0000000Bu;
    if (!deviceId)
        return kXErrorInvalidParameter;

    // The host-backed userdata volume is exposed to the title as one stable
    // Xbox storage device. Xenia uses the same dummy ID for its virtual content
    // device, and retail callers only need an ID that can be fed into the
    // subsequent XamContent* APIs.
    constexpr uint32_t kUserdataDeviceId = 1;
    static std::atomic<uint32_t> reports{0};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
        KLOG("XamShowDeviceSelectorUI user=%u type=%u flags=%08X requested=%llu "
             "deviceOut=%08X overlapped=%08X\n",
             userIndex, contentType, contentFlags,
             static_cast<unsigned long long>(totalRequested),
             g_guestMemory.MapVirtual(deviceId),
             overlapped ? g_guestMemory.MapVirtual(overlapped) : 0u);

    if (!overlapped)
    {
        BroadcastXamNotification(kXnSysUi, 1);
        *deviceId = kUserdataDeviceId;
        BroadcastXamNotification(kXnSysStorageDevicesChanged, kUserdataDeviceId);
        BroadcastXamNotification(kXnSysUi, 0);
        SetGuestLastError(0);
        return 0;
    }

    // XAM system-UI requests are genuinely asynchronous. In particular, some
    // retail titles reject the selected device if Result/deviceId are completed
    // before control has returned from XamShowDeviceSelectorUI. Publish PENDING
    // first, then complete from a short deferred host task. The guest allocation
    // remains stable while an X_OVERLAPPED request is outstanding.
    *deviceId = 0;
    overlapped->result = kXErrorIoPending;
    overlapped->length = 0;
    overlapped->extendedError = kXErrorIoPending;
    const uint32_t event = overlapped->event;

    // System UI becomes active while the selector owns focus. Titles commonly
    // listen for these XAM notifications instead of only polling OVERLAPPED.
    BroadcastXamNotification(kXnSysUi, 1);

    std::thread([deviceId, overlapped, event] {
        HostSleepPrecise(10);
        *deviceId = kUserdataDeviceId;
        overlapped->length = 0;
        overlapped->extendedError = 0;
        overlapped->result = 0;
        // Choosing the virtual storage device changes the title-visible storage
        // selection, then the system UI closes. The storage manager in retail
        // titles uses the former to refresh XamContent device state.
        BroadcastXamNotification(kXnSysStorageDevicesChanged, kUserdataDeviceId);
        BroadcastXamNotification(kXnSysUi, 0);
        KLOG("XamShowDeviceSelectorUI complete device=%u deviceOut=%08X overlapped=%08X event=%08X\n",
             kUserdataDeviceId, g_guestMemory.MapVirtual(deviceId),
             g_guestMemory.MapVirtual(overlapped), event);
        if (event)
            SignalHandleEvent(event);
    }).detach();

    SetGuestLastError(0);
    return kXErrorIoPending;
}

} // namespace

PPC_FUNC(__imp__RtlRaiseException)
{
    KCALL("RtlRaiseException");
    if (!ctx.r3.u32)
    {
        MojoRecompRaiseUnimplementedImport("RtlRaiseException(null)", ctx.lr);
        return;
    }

    const uint8_t* record = base + ctx.r3.u32;
    const uint32_t code = *reinterpret_cast<const be<uint32_t>*>(record);
    if (code == 0x406D1388u)
    {
        // MSVC's debugger-only SetThreadName exception. It is continuable and has
        // no title-visible effect when no debugger is attached.
        const uint32_t namePtr = *reinterpret_cast<const be<uint32_t>*>(record + 0x18);
        const char* name = namePtr ? reinterpret_cast<const char*>(base + namePtr) : "?";
        KLOG("thread name exception: '%s'\n", name);
        return;
    }

    KLOG("RtlRaiseException code=%08X record=%08X needs SEH support\n", code, ctx.r3.u32);
    MojoRecompRaiseUnimplementedImport("RtlRaiseException(SEH)", ctx.lr);
}

GUEST_FUNCTION_HOOK(__imp__NtAllocateVirtualMemory, NtAllocateVirtualMemory_x)
GUEST_FUNCTION_HOOK(__imp__NtFreeVirtualMemory, NtFreeVirtualMemory_x)
GUEST_FUNCTION_HOOK(__imp__NtQueryVirtualMemory, NtQueryVirtualMemory_x)
GUEST_FUNCTION_HOOK(__imp__MmAllocatePhysicalMemoryEx, MmAllocatePhysicalMemoryEx_x)
GUEST_FUNCTION_HOOK(__imp__MmFreePhysicalMemory, MmFreePhysicalMemory_x)
GUEST_FUNCTION_HOOK(__imp__MmGetPhysicalAddress, MmGetPhysicalAddress_x)
GUEST_FUNCTION_HOOK(__imp__MmQueryAddressProtect, MmQueryAddressProtect_x)
GUEST_FUNCTION_HOOK(__imp__MmQueryStatistics, MmQueryStatistics_x)
GUEST_FUNCTION_HOOK(__imp__MmCreateKernelStack, MmCreateKernelStack_x)
GUEST_FUNCTION_HOOK(__imp__MmDeleteKernelStack, MmDeleteKernelStack_x)

GUEST_FUNCTION_HOOK(__imp__RtlInitializeCriticalSection, RtlInitializeCriticalSection_x)
GUEST_FUNCTION_HOOK(__imp__RtlEnterCriticalSection, RtlEnterCriticalSection_x)
GUEST_FUNCTION_HOOK(__imp__RtlLeaveCriticalSection, RtlLeaveCriticalSection_x)
GUEST_FUNCTION_HOOK(__imp__KfAcquireSpinLock, KfAcquireSpinLock_x)
GUEST_FUNCTION_HOOK(__imp__KfReleaseSpinLock, KfReleaseSpinLock_x)
GUEST_FUNCTION_HOOK(__imp__KeAcquireSpinLockAtRaisedIrql, KeAcquireSpinLockAtRaisedIrql_x)
GUEST_FUNCTION_HOOK(__imp__KeReleaseSpinLockFromRaisedIrql, KeReleaseSpinLockFromRaisedIrql_x)
GUEST_FUNCTION_HOOK(__imp__KeEnterCriticalRegion, KeEnterCriticalRegion_x)
GUEST_FUNCTION_HOOK(__imp__KeLeaveCriticalRegion, KeLeaveCriticalRegion_x)

GUEST_FUNCTION_HOOK(__imp__NtCreateEvent, NtCreateEvent_x)
GUEST_FUNCTION_HOOK(__imp__NtSetEvent, NtSetEvent_x)
GUEST_FUNCTION_HOOK(__imp__NtCreateSemaphore, NtCreateSemaphore_x)
GUEST_FUNCTION_HOOK(__imp__NtReleaseSemaphore, NtReleaseSemaphore_x)
GUEST_FUNCTION_HOOK(__imp__NtCreateMutant, NtCreateMutant_x)
GUEST_FUNCTION_HOOK(__imp__NtReleaseMutant, NtReleaseMutant_x)
GUEST_FUNCTION_HOOK(__imp__NtWaitForSingleObjectEx, NtWaitForSingleObjectEx_x)
GUEST_FUNCTION_HOOK(__imp__NtWaitForMultipleObjectsEx, NtWaitForMultipleObjectsEx_x)
GUEST_FUNCTION_HOOK(__imp__NtClose, NtClose_x)
GUEST_FUNCTION_HOOK(__imp__NtCreateFile, NtCreateFile_x)
GUEST_FUNCTION_HOOK(__imp__NtOpenFile, NtOpenFile_x)
GUEST_FUNCTION_HOOK(__imp__NtReadFile, NtReadFile_x)
GUEST_FUNCTION_HOOK(__imp__NtWriteFile, NtWriteFile_x)
GUEST_FUNCTION_HOOK(__imp__NtQueryInformationFile, NtQueryInformationFile_x)
GUEST_FUNCTION_HOOK(__imp__NtSetInformationFile, NtSetInformationFile_x)
GUEST_FUNCTION_HOOK(__imp__NtFlushBuffersFile, NtFlushBuffersFile_x)
GUEST_FUNCTION_HOOK(__imp__NtQueryVolumeInformationFile, NtQueryVolumeInformationFile_x)
GUEST_FUNCTION_HOOK(__imp__ObReferenceObjectByHandle, ObReferenceObjectByHandle_x)
GUEST_FUNCTION_HOOK(__imp__ObDereferenceObject, ObDereferenceObject_x)
GUEST_FUNCTION_HOOK(__imp__ObCreateSymbolicLink, ObCreateSymbolicLink_x)
GUEST_FUNCTION_HOOK(__imp__ObDeleteSymbolicLink, ObDeleteSymbolicLink_x)
GUEST_FUNCTION_HOOK(__imp__KeSetEvent, KeSetEvent_x)
GUEST_FUNCTION_HOOK(__imp__KeResetEvent, KeResetEvent_x)
GUEST_FUNCTION_HOOK(__imp__KeInitializeSemaphore, KeInitializeSemaphore_x)
GUEST_FUNCTION_HOOK(__imp__KeReleaseSemaphore, KeReleaseSemaphore_x)
GUEST_FUNCTION_HOOK(__imp__KeWaitForSingleObject, KeWaitForSingleObject_x)
GUEST_FUNCTION_HOOK(__imp__KeWaitForMultipleObjects, KeWaitForMultipleObjects_x)
// Kernel APC trampoline used as a callback placeholder. It intentionally performs
// no work when invoked directly.
GUEST_FUNCTION_STUB(__imp__KiApcNormalRoutineNop)

GUEST_FUNCTION_HOOK(__imp__KeTlsAlloc, KeTlsAlloc_x)
GUEST_FUNCTION_HOOK(__imp__KeTlsFree, KeTlsFree_x)
GUEST_FUNCTION_HOOK(__imp__KeTlsGetValue, KeTlsGetValue_x)
GUEST_FUNCTION_HOOK(__imp__KeTlsSetValue, KeTlsSetValue_x)
GUEST_FUNCTION_HOOK(__imp__KeGetCurrentProcessType, KeGetCurrentProcessType_x)
GUEST_FUNCTION_HOOK(__imp__KeSetBasePriorityThread, KeSetBasePriorityThread_x)
GUEST_FUNCTION_HOOK(__imp__KeSetAffinityThread, KeSetAffinityThread_x)
GUEST_FUNCTION_HOOK(__imp__KeQueryPerformanceFrequency, KeQueryPerformanceFrequency_x)
GUEST_FUNCTION_HOOK(__imp__KeQuerySystemTime, KeQuerySystemTime_x)
GUEST_FUNCTION_HOOK(__imp__KeDelayExecutionThread, KeDelayExecutionThread_x)
GUEST_FUNCTION_HOOK(__imp__KeRaiseIrqlToDpcLevel, KeRaiseIrqlToDpcLevel_x)
GUEST_FUNCTION_HOOK(__imp__KfLowerIrql, KfLowerIrql_x)

GUEST_FUNCTION_HOOK(__imp__RtlInitAnsiString, RtlInitAnsiString_x)
GUEST_FUNCTION_HOOK(__imp__RtlCompareMemoryUlong, RtlCompareMemoryUlong_x)
GUEST_FUNCTION_HOOK(__imp__RtlFillMemoryUlong, RtlFillMemoryUlong_x)
GUEST_FUNCTION_HOOK(__imp__RtlTimeToTimeFields, RtlTimeToTimeFields_x)
GUEST_FUNCTION_HOOK(__imp__RtlTimeFieldsToTime, RtlTimeFieldsToTime_x)
GUEST_FUNCTION_HOOK(__imp__RtlNtStatusToDosError, RtlNtStatusToDosError_x)
GUEST_FUNCTION_HOOK(__imp__ExGetXConfigSetting, ExGetXConfigSetting_x)
GUEST_FUNCTION_HOOK(__imp__XexCheckExecutablePrivilege, XexCheckExecutablePrivilege_x)
GUEST_FUNCTION_HOOK(__imp__RtlImageXexHeaderField, RtlImageXexHeaderField_x)

GUEST_FUNCTION_HOOK(__imp__XGetLanguage, XGetLanguage_x)
GUEST_FUNCTION_HOOK(__imp__XGetAVPack, XGetAVPack_x)
GUEST_FUNCTION_HOOK(__imp__XGetGameRegion, XGetGameRegion_x)
GUEST_FUNCTION_HOOK(__imp__XamGetSystemVersion, XamGetSystemVersion_x)
GUEST_FUNCTION_HOOK(__imp__XamUserGetSigninState, XamUserGetSigninState_x)
GUEST_FUNCTION_HOOK(__imp__XamNotifyCreateListener, XamNotifyCreateListener_x)
GUEST_FUNCTION_HOOK(__imp__XNotifyGetNext, XNotifyGetNext_x)
GUEST_FUNCTION_HOOK(__imp__XamInputGetCapabilities, XamInputGetCapabilities_x)
GUEST_FUNCTION_HOOK(__imp__XamInputGetState, XamInputGetState_x)
GUEST_FUNCTION_HOOK(__imp__XamInputSetState, XamInputSetState_x)
GUEST_FUNCTION_HOOK(__imp__XamUserReadProfileSettings, XamUserReadProfileSettings_x)
GUEST_FUNCTION_HOOK(__imp__XMsgInProcessCall, XMsgInProcessCall_x)
GUEST_FUNCTION_HOOK(__imp__XMsgStartIORequestEx, XMsgStartIORequestEx_x)
GUEST_FUNCTION_HOOK(__imp__XMsgStartIORequest, XMsgStartIORequest_x)
GUEST_FUNCTION_HOOK(__imp__XamContentGetDeviceData, XamContentGetDeviceData_x)
GUEST_FUNCTION_HOOK(__imp__XamContentGetDeviceState, XamContentGetDeviceState_x)
GUEST_FUNCTION_HOOK(__imp__XamContentCreateEnumerator, XamContentCreateEnumerator_x)
GUEST_FUNCTION_HOOK(__imp__XamEnumerate, XamEnumerate_x)
GUEST_FUNCTION_HOOK(__imp__XamContentCreateEx, XamContentCreateEx_x)
GUEST_FUNCTION_HOOK(__imp__XamContentClose, XamContentClose_x)
GUEST_FUNCTION_HOOK(__imp__XamContentFlush, XamContentFlush_x)
GUEST_FUNCTION_HOOK(__imp__XamContentDelete, XamContentDelete_x)
GUEST_FUNCTION_HOOK(__imp__XamContentGetCreator, XamContentGetCreator_x)
GUEST_FUNCTION_HOOK(__imp__XamShowDeviceSelectorUI, XamShowDeviceSelectorUI_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_WSAStartup, NetDll_WSAStartup_x)

GUEST_FUNCTION_HOOK(__imp__ExCreateThread, ExCreateThread_x)
GUEST_FUNCTION_HOOK(__imp__ExTerminateThread, ExTerminateThread_x)
GUEST_FUNCTION_HOOK(__imp__ExRegisterTitleTerminateNotification,
                    ExRegisterTitleTerminateNotification_x)
GUEST_FUNCTION_HOOK(__imp__NtResumeThread, NtResumeThread_x)
GUEST_FUNCTION_HOOK(__imp__KeResumeThread, KeResumeThread_x)

GUEST_FUNCTION_HOOK(__imp__XGetVideoMode, XGetVideoMode_x)
GUEST_FUNCTION_HOOK(__imp__VdQueryVideoMode, VdQueryVideoMode_x)
GUEST_FUNCTION_HOOK(__imp__VdQueryVideoFlags, VdQueryVideoFlags_x)

PPC_FUNC(__imp___vsnprintf)
{
    KCALL("_vsnprintf");
    char* destination = ctx.r3.u32 ? reinterpret_cast<char*>(base + ctx.r3.u32) : nullptr;
    const uint32_t count = ctx.r4.u32;
    const char* format = ctx.r5.u32 ? reinterpret_cast<const char*>(base + ctx.r5.u32) : nullptr;
    const uint32_t vaList = ctx.r6.u32;

    if (!format || (!destination && count != 0) || !vaList)
    {
        if (destination && count)
            destination[0] = '\0';
        ctx.r3.s64 = -1;
        return;
    }

    GuestVaListReader args{base, vaList};
    std::string rendered;
    const int result = GuestFormatPrintf(base, format, args, rendered);
    if (result < 0)
    {
        if (destination && count)
            destination[0] = '\0';
        ctx.r3.s64 = -1;
        return;
    }

    if (destination && count)
    {
        const size_t copy = std::min<size_t>(rendered.size(), count - 1u);
        if (copy)
            std::memcpy(destination, rendered.data(), copy);
        destination[copy] = '\0';
    }

    // Xbox 360 titles were built against the pre-VS2015 Microsoft CRT: its
    // _vsnprintf returns a negative value when the output does not fit. The
    // destination is kept NUL terminated here because doing so is safer for the
    // title's immediate DbgPrint/error-report consumers and does not change the
    // successful path.
    ctx.r3.s64 = rendered.size() >= count && count != 0 ? -1 : result;
}

PPC_FUNC(__imp__sprintf)
{
    KCALL("sprintf");
    char* destination = ctx.r3.u32 ? reinterpret_cast<char*>(base + ctx.r3.u32) : nullptr;
    const char* format = ctx.r4.u32 ? reinterpret_cast<const char*>(base + ctx.r4.u32) : nullptr;
    if (!destination || !format)
    {
        ctx.r3.s64 = -1;
        return;
    }

    GuestRegisterVaReader args{ctx, base};
    std::string rendered;
    const int result = GuestFormatPrintf(base, format, args, rendered);
    if (result < 0)
    {
        destination[0] = '\0';
        ctx.r3.s64 = -1;
        return;
    }

    if (!rendered.empty())
        std::memcpy(destination, rendered.data(), rendered.size());
    destination[rendered.size()] = '\0';
    ctx.r3.s64 = result;
}

PPC_FUNC(__imp__DbgPrint)
{
    KCALL("__imp__DbgPrint");
    const char* text = ctx.r3.u32 ? reinterpret_cast<const char*>(base + ctx.r3.u32) : nullptr;
    if (text)
        std::fprintf(stderr, "[guest] %s", text);
    ctx.r3.u64 = 0;
}
