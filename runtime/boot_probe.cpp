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
#include "host/crash_reporter.h"
#include "host/frame_rate_policy.h"
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

#if defined(_WIN32)
int RunCrashDumpSelfTest()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("mojorecomp-crash-dump-test-" + std::to_string(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(root, error);
    if (error)
    {
        std::fprintf(stderr, "FAIL: could not create crash dump self-test directory.\n");
        return 1;
    }

    wchar_t executable[1024]{};
    const DWORD executableLength = GetModuleFileNameW(
        nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!executableLength || executableLength >= std::size(executable))
    {
        std::fprintf(stderr, "FAIL: could not resolve crash dump self-test executable.\n");
        return 2;
    }

    const DWORD previousLength = GetEnvironmentVariableW(L"MOJORECOMP_LOG_ROOT", nullptr, 0);
    std::wstring previous;
    if (previousLength > 0)
    {
        previous.resize(previousLength);
        GetEnvironmentVariableW(L"MOJORECOMP_LOG_ROOT", previous.data(), previousLength);
        if (!previous.empty() && previous.back() == L'\0')
            previous.pop_back();
    }
    SetEnvironmentVariableW(L"MOJORECOMP_LOG_ROOT", root.wstring().c_str());

    std::wstring commandLine = L"\"";
    commandLine += executable;
    commandLine += L"\" --crash-dump-self-test-child";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executable, commandLine.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (previousLength > 0)
        SetEnvironmentVariableW(L"MOJORECOMP_LOG_ROOT", previous.c_str());
    else
        SetEnvironmentVariableW(L"MOJORECOMP_LOG_ROOT", nullptr);
    if (!created)
    {
        std::fprintf(stderr, "FAIL: could not start crash dump self-test child.\n");
        return 3;
    }

    const DWORD wait = WaitForSingleObject(process.hProcess, 15000);
    DWORD exitCode = STILL_ACTIVE;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || exitCode != EXCEPTION_ACCESS_VIOLATION)
    {
        std::fprintf(stderr, "FAIL: crash dump self-test child did not terminate with C0000005.\n");
        return 4;
    }

    std::filesystem::path logPath;
    std::filesystem::path dumpPath;
    std::filesystem::path jsonPath;
    for (const auto& entry : std::filesystem::directory_iterator(root))
    {
        if (!entry.is_regular_file())
            continue;
        const auto extension = entry.path().extension().wstring();
        if (extension == L".log")
            logPath = entry.path();
        else if (extension == L".dmp")
            dumpPath = entry.path();
        else if (extension == L".json")
            jsonPath = entry.path();
    }
    if (logPath.empty() || dumpPath.empty() || jsonPath.empty() ||
        logPath.stem() != dumpPath.stem() || logPath.stem() != jsonPath.stem())
    {
        std::fprintf(stderr, "FAIL: crash dump self-test artifacts were not paired.\n");
        return 5;
    }

    std::ifstream dump(dumpPath, std::ios::binary);
    char signature[4]{};
    dump.read(signature, sizeof(signature));
    if (dump.gcount() != sizeof(signature) || std::memcmp(signature, "MDMP", 4) != 0)
    {
        std::fprintf(stderr, "FAIL: crash dump self-test did not create a valid minidump header.\n");
        return 6;
    }
    std::ifstream log(logPath);
    const std::string logText((std::istreambuf_iterator<char>(log)),
                              std::istreambuf_iterator<char>());
    if (logText.find("code=C0000005") == std::string::npos ||
        logText.find("[minidump] status=created") == std::string::npos)
    {
        std::fprintf(stderr, "FAIL: crash report is missing exception or minidump status.\n");
        return 7;
    }

    std::filesystem::remove_all(root, error);
    const auto failureRoot = root / "forced-dump-failure";
    std::filesystem::create_directories(failureRoot, error);
    if (error)
    {
        std::fprintf(stderr, "FAIL: could not create forced dump failure directory.\n");
        return 8;
    }

    const DWORD previousFailureLength =
        GetEnvironmentVariableW(L"MOJORECOMP_CRASH_DUMP_FAIL_TEST", nullptr, 0);
    std::wstring previousFailure;
    if (previousFailureLength > 0)
    {
        previousFailure.resize(previousFailureLength);
        GetEnvironmentVariableW(L"MOJORECOMP_CRASH_DUMP_FAIL_TEST",
                                previousFailure.data(), previousFailureLength);
        if (!previousFailure.empty() && previousFailure.back() == L'\0')
            previousFailure.pop_back();
    }
    SetEnvironmentVariableW(L"MOJORECOMP_LOG_ROOT", failureRoot.wstring().c_str());
    SetEnvironmentVariableW(L"MOJORECOMP_CRASH_DUMP_FAIL_TEST", L"1");

    std::wstring failureCommandLine = L"\"";
    failureCommandLine += executable;
    failureCommandLine += L"\" --crash-dump-self-test-child";
    STARTUPINFOW failureStartup{};
    failureStartup.cb = sizeof(failureStartup);
    PROCESS_INFORMATION failureProcess{};
    const BOOL failureCreated = CreateProcessW(
        executable, failureCommandLine.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &failureStartup, &failureProcess);

    if (previousLength > 0)
        SetEnvironmentVariableW(L"MOJORECOMP_LOG_ROOT", previous.c_str());
    else
        SetEnvironmentVariableW(L"MOJORECOMP_LOG_ROOT", nullptr);
    if (previousFailureLength > 0)
        SetEnvironmentVariableW(L"MOJORECOMP_CRASH_DUMP_FAIL_TEST", previousFailure.c_str());
    else
        SetEnvironmentVariableW(L"MOJORECOMP_CRASH_DUMP_FAIL_TEST", nullptr);

    if (!failureCreated)
    {
        std::fprintf(stderr, "FAIL: could not start forced dump failure child.\n");
        return 9;
    }
    const DWORD failureWait = WaitForSingleObject(failureProcess.hProcess, 15000);
    DWORD failureExitCode = STILL_ACTIVE;
    GetExitCodeProcess(failureProcess.hProcess, &failureExitCode);
    CloseHandle(failureProcess.hThread);
    CloseHandle(failureProcess.hProcess);
    if (failureWait != WAIT_OBJECT_0 || failureExitCode != EXCEPTION_ACCESS_VIOLATION)
    {
        std::fprintf(stderr, "FAIL: forced dump failure child did not terminate with C0000005.\n");
        return 10;
    }

    std::filesystem::path failureLog;
    std::filesystem::path failureJson;
    bool unexpectedDump = false;
    for (const auto& entry : std::filesystem::directory_iterator(failureRoot))
    {
        if (!entry.is_regular_file())
            continue;
        const auto extension = entry.path().extension().wstring();
        if (extension == L".log")
            failureLog = entry.path();
        else if (extension == L".json")
            failureJson = entry.path();
        else if (extension == L".dmp")
            unexpectedDump = true;
    }
    if (failureLog.empty() || failureJson.empty() || unexpectedDump ||
        failureLog.stem() != failureJson.stem())
    {
        std::fprintf(stderr, "FAIL: dump failure did not preserve paired log and JSON only.\n");
        return 11;
    }
    std::ifstream failureLogFile(failureLog);
    const std::string failureLogText((std::istreambuf_iterator<char>(failureLogFile)),
                                     std::istreambuf_iterator<char>());
    if (failureLogText.find("code=C0000005") == std::string::npos ||
        failureLogText.find("[minidump] status=failed") == std::string::npos)
    {
        std::fprintf(stderr, "FAIL: dump failure suppressed or damaged the textual crash report.\n");
        return 12;
    }

    std::filesystem::remove_all(root, error);
    std::puts("OK: crash reporter created paired artifacts and preserved logs when minidump creation failed.");
    return 0;
}
#endif

} // namespace

int RunFiberProbe();
int RunEventProbe();

int main(int argc, char** argv) {
#if defined(_WIN32)
    mojorecomp::host::CrashReporterConfig crashReporterConfig{};
    crashReporterConfig.runtimeVersion = mojorecomp::version::kCotRuntime;
    crashReporterConfig.buildId = mojorecomp::version::kBuildId;
    if (const char* runtimeSha256 = std::getenv("MOJORECOMP_RUNTIME_SHA256");
        runtimeSha256 && *runtimeSha256)
        crashReporterConfig.runtimeSha256 = runtimeSha256;
    if (const char* logRoot = std::getenv("MOJORECOMP_LOG_ROOT"); logRoot && *logRoot)
        crashReporterConfig.outputDirectory = std::filesystem::path(logRoot);
    mojorecomp::host::InitializeCrashReporter(crashReporterConfig);
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

        const auto framePolicy = mojorecomp::host::ActiveFrameRatePolicy();
        mojorecomp::timebase::AdvanceDebugFrame(framePolicy.simulationHz);
        const uint64_t step = mojorecomp::timebase::GuestTicks() - pause1;
        const uint64_t expectedStep = MOJORECOMP_TIMEBASE_HZ / framePolicy.simulationHz;
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

        std::printf("OK: debug clock pause, 1/%u frame-step and 4x fast-forward passed.\n",
                    framePolicy.simulationHz);
        return 0;
    }
    if (argc == 2 &&
        (std::strcmp(argv[1], "--crash-log-self-test") == 0 ||
         std::strcmp(argv[1], "--crash-dump-self-test") == 0)) {
#if defined(_WIN32)
        return RunCrashDumpSelfTest();
#else
        std::puts("SKIP: crash logger self-test is Windows-only.");
        return 0;
#endif
    }
    if (argc == 2 && std::strcmp(argv[1], "--crash-dump-self-test-child") == 0) {
#if defined(_WIN32)
        volatile int* invalid = reinterpret_cast<volatile int*>(uintptr_t{0x10});
        return *invalid;
#else
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
        const std::filesystem::path actual =
            mojorecomp::host::CrashReportDirectoryForConfig(config);
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
    const auto frameRatePolicy = mojorecomp::host::ActiveFrameRatePolicy();
    std::fprintf(stderr,
                 "cot-runtime: frame policy simulation=%uHz presentation=%uHz display=%uHz vblank=%uHz title_patches=%u\n",
                 frameRatePolicy.simulationHz, frameRatePolicy.presentationHz,
                 frameRatePolicy.displayHz, frameRatePolicy.guestVblankHz,
                 frameRatePolicy.requiresTitlePatches ? 1u : 0u);
    mojorecomp::debug::SetEnabled(false);
    if (options.configPath)
    {
#if defined(_WIN32)
        mojorecomp::host::CrashReporterConfig configuredCrashReporter{};
        configuredCrashReporter.runtimeVersion = mojorecomp::version::kCotRuntime;
        configuredCrashReporter.buildId = mojorecomp::version::kBuildId;
        if (const char* runtimeSha256 = std::getenv("MOJORECOMP_RUNTIME_SHA256");
            runtimeSha256 && *runtimeSha256)
            configuredCrashReporter.runtimeSha256 = runtimeSha256;
        if (const char* logRoot = std::getenv("MOJORECOMP_LOG_ROOT"); logRoot && *logRoot)
            configuredCrashReporter.outputDirectory = std::filesystem::path(logRoot);
        else
            configuredCrashReporter.outputDirectory =
                mojorecomp::host::CrashReportDirectoryForConfig(*options.configPath);
        mojorecomp::host::InitializeCrashReporter(configuredCrashReporter);
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
