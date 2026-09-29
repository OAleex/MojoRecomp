#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cwchar>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <image.h>
#include <xex.h>

#include "cpu/timebase.h"
#include "cpu/guest_thread.h"
#include "config/runtime_config.h"
#include "debug_mode.h"
#include "gpu/hardware_probe.h"
#include "gpu/pm4.h"
#include "gpu/vk_presenter.h"
#include "host/window.h"
#include "kernel/heap.h"
#include "kernel/guestcall.h"
#include "kernel/memory.h"
#include "kernel/unimplemented.h"

extern "C" void MojoRecompEnsureXmaDevice();
extern "C" void MojoRecompTraceIndirectCall(uint32_t target, uint32_t lr, uint32_t object);
extern "C" void MojoRecompGetLastIndirectCall(uint32_t* target, uint32_t* lr,
                                              uint32_t* object, uint64_t* sequence);
#include "kernel/vfs.h"
#include "kernel/xex_loader.h"
#include "mojorecomp_version.h"
#include "subtitles/subtitle_runtime.h"

namespace {

#if defined(_WIN32)
uint64_t FileTime100ns(const FILETIME& value)
{
    ULARGE_INTEGER ticks{};
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    return ticks.QuadPart;
}

bool GuestThreadTimeProfileEnabled()
{
    const char* value = std::getenv("MOJORECOMP_GUEST_THREAD_TIME_PROFILE");
    return value && *value && std::strtoul(value, nullptr, 0) != 0;
}

void StartGuestThreadTimeProfile(DWORD threadId)
{
    if (!GuestThreadTimeProfileEnabled())
        return;

    std::thread([threadId] {
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, threadId);
        if (!thread)
        {
            std::fprintf(stderr, "[guest-cpu] failed to open host tid=%lu err=%lu\n",
                         static_cast<unsigned long>(threadId),
                         static_cast<unsigned long>(GetLastError()));
            return;
        }

        FILETIME create{}, exit{}, kernel{}, user{};
        if (!GetThreadTimes(thread, &create, &exit, &kernel, &user))
        {
            CloseHandle(thread);
            return;
        }
        uint64_t previousCpu100ns = FileTime100ns(kernel) + FileTime100ns(user);
        auto previousWall = std::chrono::steady_clock::now();
        uint64_t previousFrame = Pm4_FrameCount();

        for (;;)
        {
            Sleep(1000);
            if (!GetThreadTimes(thread, &create, &exit, &kernel, &user))
                break;

            const auto now = std::chrono::steady_clock::now();
            const uint64_t currentCpu100ns = FileTime100ns(kernel) + FileTime100ns(user);
            const uint64_t currentFrame = Pm4_FrameCount();
            const double wallMs = std::chrono::duration<double, std::milli>(now - previousWall).count();
            const double cpuMs = double(currentCpu100ns - previousCpu100ns) / 10000.0;
            const uint64_t frames = currentFrame - previousFrame;
            const double cpuPerFrame = frames ? cpuMs / double(frames) : 0.0;
            const double corePercent = wallMs > 0.0 ? cpuMs * 100.0 / wallMs : 0.0;
            std::fprintf(stderr,
                         "[guest-cpu] frame=%llu frames=%llu wall=%.1f ms cpu=%.2f ms cpu/frame=%.3f ms core=%.1f%%\n",
                         static_cast<unsigned long long>(currentFrame),
                         static_cast<unsigned long long>(frames), wallMs, cpuMs,
                         cpuPerFrame, corePercent);

            previousCpu100ns = currentCpu100ns;
            previousWall = now;
            previousFrame = currentFrame;
        }
        CloseHandle(thread);
    }).detach();
}
#endif

#if defined(_WIN32)
volatile LONG g_crashReportInProgress = 0;
std::filesystem::path g_crashLogRoot;
constexpr wchar_t kCotRuntimeMutexName[] = L"Local\\MojoRecomp.COT.Runtime";

HANDLE AcquireNamedRuntimeMutex(const wchar_t* name, bool& alreadyRunning)
{
    alreadyRunning = false;
    SetLastError(ERROR_SUCCESS);
    HANDLE handle = CreateMutexW(nullptr, TRUE, name);
    if (!handle)
        return nullptr;
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        alreadyRunning = true;
        CloseHandle(handle);
        return nullptr;
    }
    return handle;
}

std::filesystem::path CrashLogRootForConfig(const std::filesystem::path& configPath)
{
    return configPath.parent_path() / "logs";
}

bool CrashDebugModeEnabled()
{
    return mojorecomp::debug::ExtendedCrashInfoEnabled();
}

void CrashWrite(FILE* crashFile, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    std::vfprintf(stderr, format, args);
    va_end(args);
    if (crashFile)
    {
        std::vfprintf(crashFile, format, copy);
        std::fflush(crashFile);
    }
    va_end(copy);
}

FILE* OpenCrashReport(wchar_t* reportPath, size_t reportPathCount)
{
    if (!reportPath || reportPathCount == 0)
        return nullptr;

    wchar_t exePath[1024]{};
    const DWORD length = GetModuleFileNameW(nullptr, exePath,
                                            static_cast<DWORD>(std::size(exePath)));
    if (!length || length >= std::size(exePath))
        return nullptr;

    wchar_t* slash = std::wcsrchr(exePath, L'\\');
    if (!slash)
        slash = std::wcsrchr(exePath, L'/');
    if (slash)
        *slash = L'\0';

    const std::filesystem::path crashDirPath = !g_crashLogRoot.empty()
        ? g_crashLogRoot
        : std::filesystem::path(exePath) / L"crashlogs";
    std::error_code crashDirError;
    std::filesystem::create_directories(crashDirPath, crashDirError);
    if (crashDirError)
        return nullptr;
    const std::wstring crashDir = crashDirPath.wstring();

    SYSTEMTIME now{};
    GetLocalTime(&now);
    if (swprintf_s(reportPath, reportPathCount,
                   L"%ls\\crash-%04u-%02u-%02u-%02u%02u%02u-pid%lu.log",
                   crashDir.c_str(),
                   static_cast<unsigned>(now.wYear),
                   static_cast<unsigned>(now.wMonth),
                   static_cast<unsigned>(now.wDay),
                   static_cast<unsigned>(now.wHour),
                   static_cast<unsigned>(now.wMinute),
                   static_cast<unsigned>(now.wSecond),
                   static_cast<unsigned long>(GetCurrentProcessId())) < 0)
        return nullptr;

    FILE* file = nullptr;
    return _wfopen_s(&file, reportPath, L"wb") == 0 ? file : nullptr;
}

LONG WINAPI BootProbeExceptionFilter(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    if (InterlockedCompareExchange(&g_crashReportInProgress, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    const auto* record = info->ExceptionRecord;
    const auto* context = info->ContextRecord;
    const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto rip = static_cast<uintptr_t>(context->Rip);
    const auto rsp = static_cast<uintptr_t>(context->Rsp);

    wchar_t reportPath[1400]{};
    FILE* crashFile = OpenCrashReport(reportPath, std::size(reportPath));
    const bool debugMode = CrashDebugModeEnabled();
    uint32_t lastIndirectTarget = 0;
    uint32_t lastIndirectLr = 0;
    uint32_t lastIndirectObject = 0;
    uint64_t lastIndirectSequence = 0;
    MojoRecompGetLastIndirectCall(&lastIndirectTarget, &lastIndirectLr,
                                &lastIndirectObject, &lastIndirectSequence);
    const char* lastIndirectMapped = "unknown";
    __try {
        lastIndirectMapped = lastIndirectTarget && g_guestMemory.FindFunction(lastIndirectTarget)
            ? "yes" : "no";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lastIndirectMapped = "unavailable";
    }

    SYSTEMTIME crashTime{};
    GetLocalTime(&crashTime);

    CrashWrite(crashFile, "MojoRecomp Crash Report\n");
    CrashWrite(crashFile, "timestamp_local=%04u-%02u-%02u %02u:%02u:%02u\n",
               static_cast<unsigned>(crashTime.wYear),
               static_cast<unsigned>(crashTime.wMonth),
               static_cast<unsigned>(crashTime.wDay),
               static_cast<unsigned>(crashTime.wHour),
               static_cast<unsigned>(crashTime.wMinute),
               static_cast<unsigned>(crashTime.wSecond));
    CrashWrite(crashFile, "version=%s build=%s %s\n",
               mojorecomp::version::kCotRuntime, __DATE__, __TIME__);
    CrashWrite(crashFile, "pid=%lu tid=%lu debug_mode=%u frame=%llu\n",
               static_cast<unsigned long>(GetCurrentProcessId()),
               static_cast<unsigned long>(GetCurrentThreadId()),
               debugMode ? 1u : 0u,
               static_cast<unsigned long long>(Pm4_FrameCount()));

    CrashWrite(crashFile,
               "[host-crash] code=%08lX address=%p module=%p rva=%llX "
               "rip=%llX rsp=%llX rbp=%llX\n",
               record->ExceptionCode, record->ExceptionAddress,
               reinterpret_cast<void*>(module),
               static_cast<unsigned long long>(rip >= module ? rip - module : 0),
               static_cast<unsigned long long>(rip),
               static_cast<unsigned long long>(rsp),
               static_cast<unsigned long long>(context->Rbp));

    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        record->NumberParameters >= 2) {
        const char* kind = record->ExceptionInformation[0] == 0 ? "read" :
                           record->ExceptionInformation[0] == 1 ? "write" : "execute";
        CrashWrite(crashFile, "[host-crash] access=%s target=%llX\n", kind,
                   static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }

    CrashWrite(crashFile,
               "[last-indirect] seq=%llu target=%08X lr=%08X object=%08X mapped=%s\n",
               static_cast<unsigned long long>(lastIndirectSequence),
               lastIndirectTarget, lastIndirectLr, lastIndirectObject,
               lastIndirectMapped);

    __try {
        if (g_ppcContext) {
            CrashWrite(crashFile,
                       "[guest-crash] lr=%08X ctr=%08X ctr_mapped=%s r1=%08X r3=%08X r4=%08X "
                       "r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X r11=%08X r12=%08X r13=%08X\n",
                       static_cast<uint32_t>(g_ppcContext->lr),
                       g_ppcContext->ctr.u32,
                       g_guestMemory.FindFunction(g_ppcContext->ctr.u32) ? "yes" : "no",
                       g_ppcContext->r1.u32,
                       g_ppcContext->r3.u32,
                       g_ppcContext->r4.u32,
                       g_ppcContext->r5.u32,
                       g_ppcContext->r6.u32,
                       g_ppcContext->r7.u32,
                       g_ppcContext->r8.u32,
                       g_ppcContext->r9.u32,
                       g_ppcContext->r10.u32,
                       g_ppcContext->r11.u32,
                       g_ppcContext->r12.u32,
                       g_ppcContext->r13.u32);

            if (debugMode) {
                CrashWrite(crashFile,
                           "[guest-debug] r14=%08X r15=%08X r16=%08X r17=%08X r18=%08X r19=%08X "
                           "r20=%08X r21=%08X r22=%08X r23=%08X r24=%08X r25=%08X r26=%08X "
                           "r27=%08X r28=%08X r29=%08X r30=%08X r31=%08X\n",
                           g_ppcContext->r14.u32, g_ppcContext->r15.u32,
                           g_ppcContext->r16.u32, g_ppcContext->r17.u32,
                           g_ppcContext->r18.u32, g_ppcContext->r19.u32,
                           g_ppcContext->r20.u32, g_ppcContext->r21.u32,
                           g_ppcContext->r22.u32, g_ppcContext->r23.u32,
                           g_ppcContext->r24.u32, g_ppcContext->r25.u32,
                           g_ppcContext->r26.u32, g_ppcContext->r27.u32,
                           g_ppcContext->r28.u32, g_ppcContext->r29.u32,
                           g_ppcContext->r30.u32, g_ppcContext->r31.u32);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        CrashWrite(crashFile, "[guest-crash] context unavailable\n");
    }

    __try {
        const auto* stack = reinterpret_cast<const uintptr_t*>(rsp);
        for (unsigned i = 0; i < 32; ++i) {
            const uintptr_t value = stack[i];
            if (value >= module && value < module + 0x40000000ull) {
                CrashWrite(crashFile, "[host-crash] stack[%02u]=%llX rva=%llX\n", i,
                           static_cast<unsigned long long>(value),
                           static_cast<unsigned long long>(value - module));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        CrashWrite(crashFile, "[host-crash] stack unreadable\n");
    }
    if (crashFile) {
        std::fclose(crashFile);
        std::fwprintf(stderr, L"[crash-log] report=%ls\n", reportPath);
    } else {
        std::fprintf(stderr, "[crash-log] failed to create report file\n");
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return {};
    const auto size = file.tellg();
    if (size <= 0)
        return {};
    std::vector<uint8_t> data(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

struct StartupOptions {
    bool runCpu = false;
    bool showVersion = false;
    bool probeHardware = false;
    bool probeRenderer = false;
    bool jsonOutput = false;
    std::optional<std::filesystem::path> configPath;
    std::optional<std::filesystem::path> gameRoot;
    std::optional<std::filesystem::path> gameOverlay;
    std::optional<std::filesystem::path> xexPath;
    mojorecomp::config::RuntimeConfigOverrides overrides;
};

bool ParseStartupOptions(int argc, char** argv, StartupOptions& options,
                         std::string& error)
{
    auto requireValue = [&](int& index, const char* option) -> const char* {
        if (index + 1 >= argc)
        {
            error = std::string("Missing value for ") + option;
            return nullptr;
        }
        return argv[++index];
    };

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--cpu")
            options.runCpu = true;
        else if (arg == "--version")
            options.showVersion = true;
        else if (arg == "--probe-hardware")
            options.probeHardware = true;
        else if (arg == "--probe-renderer")
            options.probeRenderer = true;
        else if (arg == "--json")
            options.jsonOutput = true;
        else if (arg == "--config")
        {
            const char* value = requireValue(i, "--config");
            if (!value) return false;
            options.configPath = value;
        }
        else if (arg == "--game-root")
        {
            const char* value = requireValue(i, "--game-root");
            if (!value) return false;
            options.gameRoot = value;
        }
        else if (arg == "--game-overlay")
        {
            const char* value = requireValue(i, "--game-overlay");
            if (!value) return false;
            options.gameOverlay = value;
        }
        else if (arg == "--display-mode")
        {
            const char* value = requireValue(i, "--display-mode");
            if (!value) return false;
            mojorecomp::config::DisplayMode parsed{};
            if (!mojorecomp::config::ParseDisplayMode(value, parsed))
            {
                error = std::string("Invalid --display-mode value: ") + value;
                return false;
            }
            options.overrides.displayMode = parsed;
        }
        else if (arg == "--resolution-scale")
        {
            const char* value = requireValue(i, "--resolution-scale");
            if (!value) return false;
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(value, &end, 10);
            if (end == value || *end != '\0' || parsed < 1 || parsed > 3)
            {
                error = std::string("Invalid --resolution-scale value: ") + value;
                return false;
            }
            options.overrides.resolutionScale = static_cast<uint32_t>(parsed);
        }
        else if (arg == "--aspect-ratio")
        {
            const char* value = requireValue(i, "--aspect-ratio");
            if (!value) return false;
            mojorecomp::config::AspectRatio parsed{};
            if (!mojorecomp::config::ParseAspectRatio(value, parsed))
            {
                error = std::string("Invalid --aspect-ratio value: ") + value;
                return false;
            }
            options.overrides.aspectRatio = parsed;
        }
        else if (arg == "--vsync")
        {
            const char* value = requireValue(i, "--vsync");
            if (!value) return false;
            bool parsed = false;
            if (!mojorecomp::config::ParseBool(value, parsed))
            {
                error = std::string("Invalid --vsync value: ") + value;
                return false;
            }
            options.overrides.vsync = parsed;
        }
        else if (arg == "--anti-aliasing")
        {
            const char* value = requireValue(i, "--anti-aliasing");
            if (!value) return false;
            mojorecomp::config::AntiAliasing parsed{};
            if (!mojorecomp::config::ParseAntiAliasing(value, parsed))
            {
                error = std::string("Invalid --anti-aliasing value: ") + value;
                return false;
            }
            options.overrides.antiAliasing = parsed;
        }
        else if (arg == "--texture-filtering")
        {
            const char* value = requireValue(i, "--texture-filtering");
            if (!value) return false;
            uint32_t parsed = 0;
            if (!mojorecomp::config::ParseTextureFiltering(value, parsed))
            {
                error = std::string("Invalid --texture-filtering value: ") + value;
                return false;
            }
            options.overrides.textureFiltering = parsed;
        }
        else if (!arg.empty() && arg[0] == '-')
        {
            error = "Unknown option: " + arg;
            return false;
        }
        else if (!options.xexPath)
            options.xexPath = arg;
        else
        {
            error = "Only one XEX path may be supplied";
            return false;
        }
    }
    return true;
}

} // namespace

int RunFiberProbe();
int RunEventProbe();

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetUnhandledExceptionFilter(BootProbeExceptionFilter);
#endif
    if (argc == 2 && std::strcmp(argv[1], "--debug-clock-self-test") == 0) {
        if (!mojorecomp::timebase::Init()) {
            std::fprintf(stderr, "FAIL: debug clock could not initialize host counter.\n");
            return 1;
        }

        const auto rawGuestTicks = []() -> uint64_t {
            return uint64_t((__uint128_t(mojorecomp::timebase::host_ticks()) *
                             MOJORECOMP_TIMEBASE_HZ) /
                            mojorecomp::timebase::host_hz);
        };
        const auto ratioOk = [](uint64_t scaled, uint64_t raw,
                                double expected, double tolerance) {
            if (!raw)
                return false;
            const double ratio = double(scaled) / double(raw);
            return ratio >= expected - tolerance && ratio <= expected + tolerance;
        };

        mojorecomp::timebase::SetDebugClock(1, 1, true);
        const uint64_t pause0 = mojorecomp::timebase::GuestTicks();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const uint64_t pause1 = mojorecomp::timebase::GuestTicks();
        if (pause1 != pause0) {
            std::fprintf(stderr, "FAIL: paused debug clock advanced.\n");
            return 2;
        }

        mojorecomp::timebase::AdvanceDebugFrame(30);
        const uint64_t step = mojorecomp::timebase::GuestTicks() - pause1;
        const uint64_t expectedStep = MOJORECOMP_TIMEBASE_HZ / 30;
        if (step != expectedStep) {
            std::fprintf(stderr,
                         "FAIL: frame step=%llu expected=%llu.\n",
                         static_cast<unsigned long long>(step),
                         static_cast<unsigned long long>(expectedStep));
            return 3;
        }

        mojorecomp::timebase::SetDebugClock(4, 1, false);
        const uint64_t turbo0 = mojorecomp::timebase::GuestTicks();
        const uint64_t turboRaw0 = rawGuestTicks();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        const uint64_t turbo1 = mojorecomp::timebase::GuestTicks();
        const uint64_t turboRaw1 = rawGuestTicks();
        if (!ratioOk(turbo1 - turbo0, turboRaw1 - turboRaw0, 4.0, 0.55)) {
            std::fprintf(stderr, "FAIL: debug clock 4x fast-forward scaling is outside tolerance.\n");
            return 4;
        }

        std::puts("OK: debug clock pause, 1/30 frame-step and 4x fast-forward passed.");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--crash-log-self-test") == 0) {
#if defined(_WIN32)
        uintptr_t fakeStack[32]{};
        PPCContext fakeGuest{};
        fakeGuest.lr = 0x821DAED8u;
        fakeGuest.ctr.u32 = 0x821D7BE0u;
        fakeGuest.r1.u32 = 0x88000000u;
        fakeGuest.r3.u32 = 0xA1001000u;
        fakeGuest.r4.u32 = 1u;
        fakeGuest.r13.u32 = 0x88001000u;
        g_ppcContext = &fakeGuest;
        MojoRecompTraceIndirectCall(0x821D7BE0u, 0x821DAED8u, 0xA1001000u);
        EXCEPTION_RECORD record{};
        record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        record.ExceptionAddress = reinterpret_cast<void*>(uintptr_t{0x1234});
        record.NumberParameters = 2;
        record.ExceptionInformation[0] = 8;
        record.ExceptionInformation[1] = 0;
        CONTEXT context{};
        context.Rip = 0x1234;
        context.Rsp = reinterpret_cast<DWORD64>(fakeStack);
        EXCEPTION_POINTERS pointers{&record, &context};
        BootProbeExceptionFilter(&pointers);
        g_ppcContext = nullptr;
        std::puts("OK: crash logger self-test report emitted.");
        return 0;
#else
        std::puts("SKIP: crash logger self-test is Windows-only.");
        return 0;
#endif
    }
    if (argc == 2 && std::strcmp(argv[1], "--single-instance-self-test") == 0) {
#if defined(_WIN32)
        constexpr wchar_t kSelfTestName[] = L"Local\\MojoRecomp.COT.Runtime.SelfTest";
        bool firstAlreadyRunning = false;
        HANDLE first = AcquireNamedRuntimeMutex(kSelfTestName, firstAlreadyRunning);
        if (!first || firstAlreadyRunning) {
            std::fprintf(stderr, "FAIL: first runtime mutex acquisition failed.\n");
            return 1;
        }

        bool secondAlreadyRunning = false;
        HANDLE second = AcquireNamedRuntimeMutex(kSelfTestName, secondAlreadyRunning);
        if (second || !secondAlreadyRunning) {
            if (second)
                CloseHandle(second);
            ReleaseMutex(first);
            CloseHandle(first);
            std::fprintf(stderr, "FAIL: duplicate runtime mutex was not rejected.\n");
            return 2;
        }
        ReleaseMutex(first);
        CloseHandle(first);
        std::puts("OK: COT runtime single-instance mutex rejects a duplicate process.");
        return 0;
#else
        std::puts("SKIP: single-instance self-test is Windows-only.");
        return 0;
#endif
    }
    if (argc == 2 && std::strcmp(argv[1], "--crash-log-path-self-test") == 0) {
#if defined(_WIN32)
        const std::filesystem::path titleRoot =
            std::filesystem::temp_directory_path() / "MojoRecomp" / "cot";
        const std::filesystem::path config = titleRoot / "settings.toml";
        const std::filesystem::path expected = titleRoot / "logs";
        const std::filesystem::path actual = CrashLogRootForConfig(config);
        if (actual.lexically_normal() != expected.lexically_normal()) {
            std::fprintf(stderr, "FAIL: crash log root did not resolve beside settings.toml.\n");
            return 1;
        }
        std::puts("OK: COT crash log root resolves to the per-title logs directory.");
        return 0;
#else
        std::puts("SKIP: crash log path self-test is Windows-only.");
        return 0;
#endif
    }
    if (argc == 2 && std::strcmp(argv[1], "--window-resize-self-test") == 0) {
#if defined(_WIN32)
        mojorecomp::config::RuntimeConfig config{};
        mojorecomp::config::Set(config);
        if (!HostWindow_Init(640, 360, true, false)) {
            std::fprintf(stderr, "FAIL: hidden window creation failed.\n");
            return 1;
        }
        if (!VkPresenter_Init(HostWindow_NativeHandle(), 1280, 720)) {
            HostWindow_Shutdown();
            std::fprintf(stderr, "FAIL: Vulkan presenter initialization failed.\n");
            return 2;
        }

        auto waitForResize = [](uint64_t& generation, uint32_t expectedWidth,
                                uint32_t expectedHeight, bool expectedMinimized) {
            for (int i = 0; i < 100; ++i) {
                uint32_t width = 0;
                uint32_t height = 0;
                bool minimized = false;
                if (HostWindow_ConsumeResize(generation, width, height, minimized) &&
                    width == expectedWidth && height == expectedHeight &&
                    minimized == expectedMinimized)
                    return true;
                Sleep(10);
            }
            return false;
        };

        uint64_t generation = 0;
        if (!waitForResize(generation, 640, 360, false)) {
            VkPresenter_Shutdown();
            HostWindow_Shutdown();
            std::fprintf(stderr, "FAIL: initial output extent was not published.\n");
            return 3;
        }

        HWND hwnd = static_cast<HWND>(HostWindow_NativeHandle());
        PostMessageW(hwnd, WM_SIZE, SIZE_MINIMIZED, 0);
        if (!waitForResize(generation, 0, 0, true) ||
            !VkPresenter_SetOutputExtent(0, 0) ||
            !VkPresenter_Present(0, 1280, 720)) {
            VkPresenter_Shutdown();
            HostWindow_Shutdown();
            std::fprintf(stderr, "FAIL: minimized presentation did not suspend safely.\n");
            return 4;
        }

        RECT outer{0, 0, 800, 450};
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        if (!AdjustWindowRectEx(&outer, style, FALSE, 0) ||
            !SetWindowPos(hwnd, nullptr, 0, 0,
                          outer.right - outer.left, outer.bottom - outer.top,
                          SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
            VkPresenter_Shutdown();
            HostWindow_Shutdown();
            std::fprintf(stderr, "FAIL: restore resize failed.\n");
            return 5;
        }
        if (!waitForResize(generation, 800, 450, false) ||
            !VkPresenter_SetOutputExtent(800, 450)) {
            VkPresenter_Shutdown();
            HostWindow_Shutdown();
            std::fprintf(stderr, "FAIL: restored output extent was not recreated.\n");
            return 6;
        }

        VkPresenterExtentInfo extents{};
        if (!VkPresenter_GetExtentInfo(extents) ||
            extents.outputWidth != 800 || extents.outputHeight != 450) {
            VkPresenter_Shutdown();
            HostWindow_Shutdown();
            std::fprintf(stderr, "FAIL: restored swapchain extent is incorrect.\n");
            return 7;
        }

        VkPresenter_Shutdown();
        HostWindow_Shutdown();
        std::puts("OK: minimize suspends presentation and restore recreates the Vulkan output safely.");
        return 0;
#else
        std::puts("SKIP: window resize self-test is Windows-only.");
        return 0;
#endif
    }
    if (argc == 2 && std::strcmp(argv[1], "--window-close-self-test") == 0) {
#if defined(_WIN32)
        if (!HostWindow_Init(320, 180, true, false)) {
            std::fprintf(stderr, "FAIL: hidden window creation failed.\n");
            return 1;
        }
        HWND hwnd = static_cast<HWND>(HostWindow_NativeHandle());
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        Sleep(2000);
        std::fprintf(stderr, "FAIL: runtime survived a user window close.\n");
        return 2;
#else
        std::puts("SKIP: window close self-test is Windows-only.");
        return 0;
#endif
    }
    if (argc == 2 && std::strcmp(argv[1], "--fiber-test") == 0)
        return RunFiberProbe();
    if (argc == 2 && std::strcmp(argv[1], "--event-test") == 0)
        return RunEventProbe();
    StartupOptions options{};
    std::string startupError;
    if (!ParseStartupOptions(argc, argv, options, startupError)) {
        std::fprintf(stderr, "cot-runtime: %s\n", startupError.c_str());
        return 2;
    }
    if (options.showVersion) {
        std::printf("MojoRecomp %s\nCOT runtime %s\n",
                    mojorecomp::version::kSuite, mojorecomp::version::kCotRuntime);
        return 0;
    }
    if (options.probeHardware) {
        if (!options.jsonOutput) {
            std::fprintf(stderr,
                         "cot-runtime: --probe-hardware currently requires --json\n");
            return 2;
        }
        return mojorecomp::gpu::RunHardwareProbeJson();
    }
    if (options.jsonOutput && !options.probeRenderer) {
        std::fprintf(stderr,
                     "cot-runtime: --json is only valid with --probe-hardware or --probe-renderer\n");
        return 2;
    }

    mojorecomp::config::RuntimeConfig runtimeConfig{};
    if (options.configPath &&
        !mojorecomp::config::LoadFile(*options.configPath, runtimeConfig, startupError)) {
        std::fprintf(stderr, "cot-runtime: %s\n", startupError.c_str());
        return 2;
    }
    if (!mojorecomp::config::ApplyEnvironmentOverrides(runtimeConfig, startupError)) {
        std::fprintf(stderr, "cot-runtime: %s\n", startupError.c_str());
        return 2;
    }
    mojorecomp::config::ApplyOverrides(runtimeConfig, options.overrides);
    mojorecomp::config::Set(runtimeConfig);
    std::fprintf(stderr, "cot-runtime: localization profile=%s xbox_language=%u\n",
                 runtimeConfig.localizationProfile.c_str(), runtimeConfig.xboxLanguage);
    mojorecomp::debug::SetEnabled(false);
    if (options.configPath)
    {
#if defined(_WIN32)
        if (const char* logRoot = std::getenv("MOJORECOMP_LOG_ROOT"); logRoot && *logRoot)
            g_crashLogRoot = std::filesystem::path(logRoot);
        else
            g_crashLogRoot = CrashLogRootForConfig(*options.configPath);
#endif
    }

    if (options.probeRenderer) {
        if (!options.jsonOutput) {
            std::fprintf(stderr,
                         "cot-runtime: --probe-renderer currently requires --json\n");
            return 2;
        }
        float outputAspect = 16.0f / 9.0f;
        switch (runtimeConfig.aspectRatio)
        {
            case mojorecomp::config::AspectRatio::Ultrawide21x9:
                outputAspect = 21.0f / 9.0f;
                break;
            case mojorecomp::config::AspectRatio::SuperUltrawide32x9:
                outputAspect = 32.0f / 9.0f;
                break;
            case mojorecomp::config::AspectRatio::Ratio16x10:
                outputAspect = 16.0f / 10.0f;
                break;
            case mojorecomp::config::AspectRatio::Ratio4x3:
                outputAspect = 4.0f / 3.0f;
                break;
            default:
                break;
        }
        constexpr uint32_t probeOutputHeight = 720;
        const uint32_t probeOutputWidth = std::max(
            1u,
            static_cast<uint32_t>(std::lround(
                static_cast<double>(probeOutputHeight) * outputAspect)));
        if (!HostWindow_Init(probeOutputWidth, probeOutputHeight, true, false)) {
            std::fprintf(stderr, "renderer probe: hidden host window creation failed\n");
            return 1;
        }
        const bool initialized =
            VkPresenter_Init(HostWindow_NativeHandle(), 1280, 720);
        VkPresenterExtentInfo extents{};
        const bool resized = initialized &&
                             VkPresenter_SetOutputExtent(probeOutputWidth,
                                                         probeOutputHeight);
        const bool queried = resized && VkPresenter_GetExtentInfo(extents) &&
                             VkPresenter_ValidateConfiguredPostProcess();
        if (initialized)
            VkPresenter_Shutdown();
        HostWindow_Shutdown();
        if (!queried) {
            std::fprintf(stderr, "renderer probe: Vulkan presenter initialization failed\n");
            return 1;
        }
        std::printf(
            "{\n"
            "  \"logical_extent\": [%u, %u],\n"
            "  \"internal_extent\": [%u, %u],\n"
            "  \"output_extent\": [%u, %u],\n"
            "  \"content_rect\": [%d, %d, %u, %u],\n"
            "  \"scene_scale\": [%.6f, %.6f],\n"
            "  \"ui_scale\": [%.6f, %.6f],\n"
            "  \"resolution_scale\": %u\n"
            "}\n",
            extents.logicalWidth, extents.logicalHeight,
            extents.internalWidth, extents.internalHeight,
            extents.outputWidth, extents.outputHeight,
            extents.contentX, extents.contentY,
            extents.contentWidth, extents.contentHeight,
            extents.sceneScaleX, extents.sceneScaleY,
            extents.uiScaleX, extents.uiScaleY,
            extents.resolutionScale);
        return 0;
    }

#if defined(_WIN32)
    HANDLE runtimeInstanceMutex = nullptr;
    if (options.runCpu)
    {
        bool alreadyRunning = false;
        runtimeInstanceMutex = AcquireNamedRuntimeMutex(kCotRuntimeMutexName, alreadyRunning);
        if (!runtimeInstanceMutex)
        {
            if (alreadyRunning)
            {
                std::fprintf(stderr,
                             "cot-runtime: another Crash of the Titans runtime is already running.\n");
                return 3;
            }
            std::fprintf(stderr, "cot-runtime: could not create single-instance guard (Win32 error %lu).\n",
                         static_cast<unsigned long>(GetLastError()));
            return 1;
        }
    }
#endif

    const std::filesystem::path defaultXex = "../../game/default.xex";
    const std::filesystem::path xex = options.xexPath
        ? *options.xexPath
        : options.gameRoot ? (*options.gameRoot / "default.xex") : defaultXex;
    auto file = ReadFile(xex);
    if (file.empty()) {
        std::fprintf(stderr, "boot-probe: cannot read %s\n", xex.string().c_str());
        return 1;
    }

    Image image = Image::ParseImage(file.data(), file.size());
    if (!image.size || image.sections.empty()) {
        std::fprintf(stderr, "boot-probe: XEX parsed to an empty image\n");
        return 1;
    }

    if (image.base != PPC_IMAGE_BASE || image.size != PPC_IMAGE_SIZE) {
        std::fprintf(stderr,
                     "boot-probe: XEX/recompiler identity mismatch: XEX=%08zX+%X, "
                     "PPC=%08llX+%llX\n",
                     image.base, image.size,
                     static_cast<unsigned long long>(PPC_IMAGE_BASE),
                     static_cast<unsigned long long>(PPC_IMAGE_SIZE));
        return 1;
    }

    if (!mojorecomp::timebase::Init()) {
        std::fprintf(stderr, "boot-probe: host timebase calibration failed\n");
        return 1;
    }

    uint32_t stackSize = 0;
    uint32_t tlsSlots = 0;
    if (const auto* stack = static_cast<const be<uint32_t>*>(
            getOptHeaderPtr(file.data(), XEX_HEADER_DEFAULT_STACK_SIZE)))
        stackSize = *stack;
    if (const auto* tls = static_cast<const be<uint32_t>*>(
            getOptHeaderPtr(file.data(), XEX_HEADER_TLS_INFO)))
        tlsSlots = tls[0];

    g_guestMemory.Init();
    g_guestHeap.Init();
    // XMA owns a hardware-visible physical context array. Reserve and publish it
    // before the translated title reads the XMA context-array register and before
    // unrelated physical allocations can move the array away from its boot-time base.
    MojoRecompEnsureXmaDevice();
    VfsSetGameRoot(options.gameRoot ? options.gameRoot->string() : xex.parent_path().string());
    if (options.gameOverlay)
        VfsSetGameOverlay(options.gameOverlay->string());
    else
        VfsSetGameOverlay({});
    const char* saveRoot = std::getenv("MOJORECOMP_SAVE_ROOT");
    const char* contentRoot = std::getenv("MOJORECOMP_CONTENT_ROOT");
    const char* cacheRoot = std::getenv("MOJORECOMP_CACHE_ROOT");
    const char* utilityRoot = std::getenv("MOJORECOMP_UTILITY_ROOT");
    if (saveRoot && *saveRoot && contentRoot && *contentRoot &&
        cacheRoot && *cacheRoot && utilityRoot && *utilityRoot)
    {
        VfsSetUserRoots(saveRoot, contentRoot, cacheRoot, utilityRoot);
    }
    else if (const char* userRoot = std::getenv("MOJORECOMP_USERDATA_ROOT"); userRoot && *userRoot)
        VfsSetUserRoot(userRoot);
    else if (options.configPath)
        VfsSetUserRoot((options.configPath->parent_path() / "userdata").string());
    else
        VfsSetUserRoot((xex.parent_path().parent_path() / "userdata").string());
    mojorecomp::subtitles::Initialize();
    const uint64_t imageEnd = uint64_t(image.base) + image.size;
    std::size_t copied = 0;
    for (const auto& section : image.sections) {
        if (!section.data)
            continue;
        if (uint64_t(section.base) + section.size > imageEnd)
            continue;
        std::memcpy(g_guestMemory.base + section.base, section.data, section.size);
        ++copied;
    }

    const uint32_t headerBase = PublishXexHeaders(file.data(), file.size());
    if (!headerBase) {
        std::fprintf(stderr, "boot-probe: could not publish XEX headers\n");
        return 1;
    }
    ResolveXexDataImports(file.data());

    auto* entry = g_guestMemory.FindFunction(static_cast<uint32_t>(image.entry_point));
    std::printf("MojoRecomp XEX boot probe\n");
    std::printf("  image       0x%08zX + 0x%X\n", image.base, image.size);
    std::printf("  entry point 0x%08zX\n", image.entry_point);
    std::printf("  stack       0x%X bytes\n", stackSize);
    std::printf("  TLS slots   %u\n", tlsSlots);
    std::printf("  timebase    %.6f MHz guest / %.3f GHz host\n",
                double(MOJORECOMP_TIMEBASE_HZ) / 1e6,
                double(mojorecomp::timebase::host_hz) / 1e9);
    std::printf("  sections    %zu parsed / %zu copied\n", image.sections.size(), copied);
    std::printf("  XEX headers 0x%08X\n", headerBase);
    std::printf("  title ID    0x%08X\n", XexTitleId());
    std::printf("  entry map   %s\n", entry ? "present" : "MISSING");

    if (!entry)
        return 1;

    std::puts("OK: XEX identity, guest memory, headers/imports and translated entry mapping agree.");

    if (options.runCpu) {
        std::puts("CPU gate: entering _xstart; the first missing HLE import will stop the probe.");
        GuestThreadContext thread(0, stackSize, tlsSlots);
#if defined(_WIN32)
        const DWORD mainGuestTid = GetCurrentThreadId();
        std::fprintf(stderr, "[cpu] main guest host tid=%lu\n",
                     static_cast<unsigned long>(mainGuestTid));
        StartGuestThreadTimeProfile(mainGuestTid);
#endif
        try {
            entry(thread.ppc, g_guestMemory.base);
            std::printf("CPU gate: entry returned unexpectedly with r3=0x%08X\n", thread.ppc.r3.u32);
        } catch (const MojoRecompUnimplementedImport& missing) {
            std::printf("CPU gate reached unimplemented import: %s (guest lr=0x%08X)\n",
                        missing.name, missing.lr);
        }
    }
#if defined(_WIN32)
    if (runtimeInstanceMutex)
    {
        ReleaseMutex(runtimeInstanceMutex);
        CloseHandle(runtimeInstanceMutex);
    }
#endif
    return 0;
}
