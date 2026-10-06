#include "crash_reporter.h"

#if defined(_WIN32)

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../debug_mode.h"
#include "../gpu/pm4.h"
#include "../kernel/guestcall.h"
#include "../kernel/memory.h"

extern "C" void MojoRecompGetLastIndirectCall(uint32_t* target, uint32_t* lr,
                                              uint32_t* object, uint64_t* sequence);

namespace mojorecomp::host {
namespace {

constexpr size_t kCrashPathCapacity = 1400;
constexpr size_t kTextFieldCapacity = 96;

struct VulkanFailureRecord
{
    std::array<char, kTextFieldCapacity> call{};
    std::array<char, kTextFieldCapacity> resultName{};
    std::array<char, kTextFieldCapacity> stage{};
    int32_t result = 0;
    uint64_t frame = 0;
    uint64_t sequence = 0;
    uint32_t threadId = 0;
};

volatile LONG g_crashReportInProgress = 0;
std::array<wchar_t, kCrashPathCapacity> g_outputDirectory{};
std::array<char, kTextFieldCapacity> g_runtimeVersion{};
std::array<char, kTextFieldCapacity> g_buildId{};
std::array<char, kTextFieldCapacity> g_runtimeSha256{};
std::array<VulkanFailureRecord, 2> g_vulkanFailures{};
std::atomic<uint64_t> g_vulkanFailureGeneration{0};

void CopyText(std::array<char, kTextFieldCapacity>& destination, const char* source) noexcept
{
    if (!source)
    {
        destination[0] = '\0';
        return;
    }
    std::snprintf(destination.data(), destination.size(), "%s", source);
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

bool BuildCrashBasePath(wchar_t* path, size_t pathCount, SYSTEMTIME& now)
{
    if (!path || pathCount == 0)
        return false;

    std::filesystem::path crashDirectory;
    if (g_outputDirectory[0])
    {
        crashDirectory = g_outputDirectory.data();
    }
    else
    {
        wchar_t exePath[1024]{};
        const DWORD length = GetModuleFileNameW(
            nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
        if (!length || length >= std::size(exePath))
            return false;
        std::filesystem::path executable(exePath);
        crashDirectory = executable.parent_path() / L"crashlogs";
    }

    std::error_code directoryError;
    std::filesystem::create_directories(crashDirectory, directoryError);
    if (directoryError)
        return false;

    GetLocalTime(&now);
    return swprintf_s(
               path, pathCount,
               L"%ls\\crash-%04u-%02u-%02u-%02u%02u%02u-pid%lu",
               crashDirectory.wstring().c_str(),
               static_cast<unsigned>(now.wYear),
               static_cast<unsigned>(now.wMonth),
               static_cast<unsigned>(now.wDay),
               static_cast<unsigned>(now.wHour),
               static_cast<unsigned>(now.wMinute),
               static_cast<unsigned>(now.wSecond),
               static_cast<unsigned long>(GetCurrentProcessId())) >= 0;
}

bool AppendExtension(const wchar_t* base, const wchar_t* extension,
                     wchar_t* destination, size_t destinationCount)
{
    if (!base || !extension || !destination || destinationCount == 0)
        return false;
    return swprintf_s(destination, destinationCount, L"%ls%ls", base, extension) >= 0;
}

VulkanFailureRecord LastVulkanFailure() noexcept
{
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const uint64_t generation = g_vulkanFailureGeneration.load(std::memory_order_acquire);
        if (!generation)
            return {};
        const VulkanFailureRecord record = g_vulkanFailures[generation & 1u];
        if (generation == g_vulkanFailureGeneration.load(std::memory_order_acquire))
            return record;
    }
    return {};
}

std::array<wchar_t, MAX_PATH> FaultingModuleName(const void* address) noexcept
{
    std::array<wchar_t, MAX_PATH> result{};
    HMODULE module = nullptr;
    if (!address ||
        !GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module) ||
        !module)
    {
        return result;
    }
    const DWORD length = GetModuleFileNameW(module, result.data(),
                                            static_cast<DWORD>(result.size()));
    if (!length || length >= result.size())
    {
        result[0] = L'\0';
        return result;
    }
    const wchar_t* slash = std::wcsrchr(result.data(), L'\\');
    if (slash && slash[1])
        std::memmove(result.data(), slash + 1, (std::wcslen(slash + 1) + 1) * sizeof(wchar_t));
    return result;
}

bool WriteMiniDump(const wchar_t* dumpPath, EXCEPTION_POINTERS* info, DWORD& errorCode)
{
    errorCode = ERROR_SUCCESS;
    char forcedFailure[2]{};
    if (GetEnvironmentVariableA(
            "MOJORECOMP_CRASH_DUMP_FAIL_TEST",
            forcedFailure,
            static_cast<DWORD>(std::size(forcedFailure))) > 0)
    {
        errorCode = ERROR_ACCESS_DENIED;
        return false;
    }
    HANDLE file = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        errorCode = GetLastError();
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
    exceptionInfo.ThreadId = GetCurrentThreadId();
    exceptionInfo.ExceptionPointers = info;
    exceptionInfo.ClientPointers = FALSE;
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    const BOOL written = MiniDumpWriteDump(
        GetCurrentProcess(), GetCurrentProcessId(), file, type,
        &exceptionInfo, nullptr, nullptr);
    if (!written)
        errorCode = GetLastError();
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!written)
        DeleteFileW(dumpPath);
    return written != FALSE;
}

void WriteCrashSidecar(const wchar_t* jsonPath, const SYSTEMTIME& crashTime,
                       const EXCEPTION_RECORD& record, uint64_t frame,
                       const VulkanFailureRecord& vulkanFailure,
                       const wchar_t* faultingModule, bool dumpCreated,
                       DWORD dumpError)
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, jsonPath, L"wb") != 0 || !file)
        return;

    const char* access = nullptr;
    uint64_t invalidAddress = 0;
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
    {
        access = record.ExceptionInformation[0] == 0 ? "read" :
                 record.ExceptionInformation[0] == 1 ? "write" : "execute";
        invalidAddress = static_cast<uint64_t>(record.ExceptionInformation[1]);
    }

    std::fprintf(file, "{\n");
    std::fprintf(file, "  \"schema_version\": 1,\n");
    std::fprintf(file, "  \"timestamp_local\": \"%04u-%02u-%02uT%02u:%02u:%02u\",\n",
                 crashTime.wYear, crashTime.wMonth, crashTime.wDay,
                 crashTime.wHour, crashTime.wMinute, crashTime.wSecond);
    std::fprintf(file, "  \"runtime_version\": \"%s\",\n", g_runtimeVersion.data());
    std::fprintf(file, "  \"build_id\": \"%s\",\n", g_buildId.data());
    if (g_runtimeSha256[0])
        std::fprintf(file, "  \"runtime_sha256\": \"%s\",\n", g_runtimeSha256.data());
    else
        std::fprintf(file, "  \"runtime_sha256\": null,\n");
    std::fprintf(file, "  \"architecture\": \"x86_64\",\n");
    std::fprintf(file, "  \"executable\": \"cot-runtime.exe\",\n");
    std::fprintf(file, "  \"exception_code\": \"0x%08lX\",\n",
                 record.ExceptionCode);
    if (access)
    {
        std::fprintf(file, "  \"access\": \"%s\",\n", access);
        std::fprintf(file, "  \"invalid_address\": \"0x%llX\",\n",
                     static_cast<unsigned long long>(invalidAddress));
    }
    else
    {
        std::fprintf(file, "  \"access\": null,\n");
        std::fprintf(file, "  \"invalid_address\": null,\n");
    }
    std::fprintf(file, "  \"frame\": %llu,\n",
                 static_cast<unsigned long long>(frame));
    std::fprintf(file, "  \"thread_id\": %lu,\n",
                 static_cast<unsigned long>(GetCurrentThreadId()));
    if (vulkanFailure.call[0])
    {
        std::fprintf(file, "  \"last_vulkan_call\": \"%s\",\n", vulkanFailure.call.data());
        std::fprintf(file, "  \"vulkan_result\": %d,\n", vulkanFailure.result);
        std::fprintf(file, "  \"vulkan_result_name\": \"%s\",\n",
                     vulkanFailure.resultName.data());
        std::fprintf(file, "  \"vulkan_stage\": \"%s\",\n",
                     vulkanFailure.stage.data());
        std::fprintf(file, "  \"gpu_sequence\": %llu,\n",
                     static_cast<unsigned long long>(vulkanFailure.sequence));
    }
    else
    {
        std::fprintf(file, "  \"last_vulkan_call\": null,\n");
        std::fprintf(file, "  \"vulkan_result\": null,\n");
        std::fprintf(file, "  \"vulkan_result_name\": null,\n");
        std::fprintf(file, "  \"vulkan_stage\": null,\n");
        std::fprintf(file, "  \"gpu_sequence\": null,\n");
    }
    char faultingModuleUtf8[MAX_PATH * 3]{};
    if (faultingModule && *faultingModule)
        WideCharToMultiByte(CP_UTF8, 0, faultingModule, -1,
                            faultingModuleUtf8, static_cast<int>(std::size(faultingModuleUtf8)),
                            nullptr, nullptr);
    if (faultingModuleUtf8[0])
        std::fprintf(file, "  \"faulting_module\": \"%s\",\n", faultingModuleUtf8);
    else
        std::fprintf(file, "  \"faulting_module\": null,\n");
    std::fprintf(file, "  \"dump_created\": %s,\n", dumpCreated ? "true" : "false");
    if (dumpCreated)
        std::fprintf(file, "  \"dump_error\": null\n");
    else
        std::fprintf(file, "  \"dump_error\": %lu\n", static_cast<unsigned long>(dumpError));
    std::fprintf(file, "}\n");
    std::fclose(file);
}

LONG WINAPI CrashExceptionFilter(EXCEPTION_POINTERS* info)
{
    if (!info || !info->ExceptionRecord || !info->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedCompareExchange(&g_crashReportInProgress, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    const auto* record = info->ExceptionRecord;
    const auto* context = info->ContextRecord;
    const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto rip = static_cast<uintptr_t>(context->Rip);
    const auto rsp = static_cast<uintptr_t>(context->Rsp);
    const uint64_t frame = Pm4_FrameCount();
    const bool debugMode = mojorecomp::debug::ExtendedCrashInfoEnabled();
    const VulkanFailureRecord vulkanFailure = LastVulkanFailure();
    const auto faultingModule = FaultingModuleName(record->ExceptionAddress);

    SYSTEMTIME crashTime{};
    wchar_t basePath[kCrashPathCapacity]{};
    wchar_t logPath[kCrashPathCapacity]{};
    wchar_t dumpPath[kCrashPathCapacity]{};
    wchar_t jsonPath[kCrashPathCapacity]{};
    const bool havePaths = BuildCrashBasePath(basePath, std::size(basePath), crashTime) &&
                           AppendExtension(basePath, L".log", logPath, std::size(logPath)) &&
                           AppendExtension(basePath, L".dmp", dumpPath, std::size(dumpPath)) &&
                           AppendExtension(basePath, L".json", jsonPath, std::size(jsonPath));

    FILE* crashFile = nullptr;
    if (havePaths)
        _wfopen_s(&crashFile, logPath, L"wb");

    uint32_t lastIndirectTarget = 0;
    uint32_t lastIndirectLr = 0;
    uint32_t lastIndirectObject = 0;
    uint64_t lastIndirectSequence = 0;
    MojoRecompGetLastIndirectCall(&lastIndirectTarget, &lastIndirectLr,
                                  &lastIndirectObject, &lastIndirectSequence);
    const char* lastIndirectMapped = "unknown";
    __try
    {
        lastIndirectMapped = lastIndirectTarget && g_guestMemory.FindFunction(lastIndirectTarget)
            ? "yes" : "no";
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        lastIndirectMapped = "unavailable";
    }

    CrashWrite(crashFile, "MojoRecomp Crash Report\n");
    CrashWrite(crashFile, "timestamp_local=%04u-%02u-%02u %02u:%02u:%02u\n",
               crashTime.wYear, crashTime.wMonth, crashTime.wDay,
               crashTime.wHour, crashTime.wMinute, crashTime.wSecond);
    CrashWrite(crashFile, "runtime_version=%s build_id=%s runtime_sha256=%s architecture=x86_64 executable=cot-runtime.exe\n",
               g_runtimeVersion.data(), g_buildId.data(),
               g_runtimeSha256[0] ? g_runtimeSha256.data() : "unknown");
    CrashWrite(crashFile, "build=%s %s\n", __DATE__, __TIME__);
    CrashWrite(crashFile, "pid=%lu tid=%lu debug_mode=%u frame=%llu\n",
               static_cast<unsigned long>(GetCurrentProcessId()),
               static_cast<unsigned long>(GetCurrentThreadId()),
               debugMode ? 1u : 0u,
               static_cast<unsigned long long>(frame));
    CrashWrite(crashFile,
               "[host-crash] code=%08lX address=%p module=%p rva=%llX rip=%llX rsp=%llX rbp=%llX faulting_module=%ls\n",
               record->ExceptionCode, record->ExceptionAddress,
               reinterpret_cast<void*>(module),
               static_cast<unsigned long long>(rip >= module ? rip - module : 0),
               static_cast<unsigned long long>(rip),
               static_cast<unsigned long long>(rsp),
               static_cast<unsigned long long>(context->Rbp),
               faultingModule[0] ? faultingModule.data() : L"unknown");

    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
        const char* kind = record->ExceptionInformation[0] == 0 ? "read" :
                           record->ExceptionInformation[0] == 1 ? "write" : "execute";
        CrashWrite(crashFile, "[host-crash] access=%s target=%llX\n", kind,
                   static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }
    CrashWrite(crashFile,
               "[last-indirect] seq=%llu target=%08X lr=%08X object=%08X mapped=%s\n",
               static_cast<unsigned long long>(lastIndirectSequence),
               lastIndirectTarget, lastIndirectLr, lastIndirectObject, lastIndirectMapped);
    if (vulkanFailure.call[0])
    {
        CrashWrite(crashFile,
                   "[last-vulkan-failure] call=%s result=%d name=%s frame=%llu sequence=%llu thread=%u stage=%s\n",
                   vulkanFailure.call.data(), vulkanFailure.result,
                   vulkanFailure.resultName.data(),
                   static_cast<unsigned long long>(vulkanFailure.frame),
                   static_cast<unsigned long long>(vulkanFailure.sequence),
                   vulkanFailure.threadId, vulkanFailure.stage.data());
    }

    __try
    {
        if (g_ppcContext)
        {
            CrashWrite(crashFile,
                       "[guest-crash] lr=%08X ctr=%08X ctr_mapped=%s r1=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X r11=%08X r12=%08X r13=%08X\n",
                       static_cast<uint32_t>(g_ppcContext->lr), g_ppcContext->ctr.u32,
                       g_guestMemory.FindFunction(g_ppcContext->ctr.u32) ? "yes" : "no",
                       g_ppcContext->r1.u32, g_ppcContext->r3.u32, g_ppcContext->r4.u32,
                       g_ppcContext->r5.u32, g_ppcContext->r6.u32, g_ppcContext->r7.u32,
                       g_ppcContext->r8.u32, g_ppcContext->r9.u32, g_ppcContext->r10.u32,
                       g_ppcContext->r11.u32, g_ppcContext->r12.u32, g_ppcContext->r13.u32);
            if (debugMode)
            {
                CrashWrite(crashFile,
                           "[guest-debug] r14=%08X r15=%08X r16=%08X r17=%08X r18=%08X r19=%08X r20=%08X r21=%08X r22=%08X r23=%08X r24=%08X r25=%08X r26=%08X r27=%08X r28=%08X r29=%08X r30=%08X r31=%08X\n",
                           g_ppcContext->r14.u32, g_ppcContext->r15.u32, g_ppcContext->r16.u32,
                           g_ppcContext->r17.u32, g_ppcContext->r18.u32, g_ppcContext->r19.u32,
                           g_ppcContext->r20.u32, g_ppcContext->r21.u32, g_ppcContext->r22.u32,
                           g_ppcContext->r23.u32, g_ppcContext->r24.u32, g_ppcContext->r25.u32,
                           g_ppcContext->r26.u32, g_ppcContext->r27.u32, g_ppcContext->r28.u32,
                           g_ppcContext->r29.u32, g_ppcContext->r30.u32, g_ppcContext->r31.u32);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        CrashWrite(crashFile, "[guest-crash] context unavailable\n");
    }

    __try
    {
        const auto* stack = reinterpret_cast<const uintptr_t*>(rsp);
        for (unsigned i = 0; i < 32; ++i)
        {
            const uintptr_t value = stack[i];
            if (value >= module && value < module + 0x40000000ull)
            {
                CrashWrite(crashFile, "[host-crash] stack[%02u]=%llX rva=%llX\n", i,
                           static_cast<unsigned long long>(value),
                           static_cast<unsigned long long>(value - module));
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        CrashWrite(crashFile, "[host-crash] stack unreadable\n");
    }

    DWORD dumpError = ERROR_PATH_NOT_FOUND;
    const bool dumpCreated = havePaths && WriteMiniDump(dumpPath, info, dumpError);
    CrashWrite(crashFile, "[minidump] status=%s error=%lu\n",
               dumpCreated ? "created" : "failed",
               static_cast<unsigned long>(dumpCreated ? ERROR_SUCCESS : dumpError));
    if (havePaths)
        WriteCrashSidecar(jsonPath, crashTime, *record, frame, vulkanFailure,
                          faultingModule.data(), dumpCreated, dumpError);

    if (crashFile)
    {
        std::fclose(crashFile);
        std::fwprintf(stderr, L"[crash-log] report=%ls\n", logPath);
        if (dumpCreated)
            std::fwprintf(stderr, L"[crash-dump] report=%ls\n", dumpPath);
    }
    else
    {
        std::fprintf(stderr, "[crash-log] failed to create report file\n");
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

std::filesystem::path CrashReportDirectoryForConfig(
    const std::filesystem::path& configPath)
{
    return configPath.parent_path() / "logs";
}

bool InitializeCrashReporter(const CrashReporterConfig& config)
{
    InterlockedExchange(&g_crashReportInProgress, 0);
    g_outputDirectory.fill(L'\0');
    if (!config.outputDirectory.empty())
    {
        const std::wstring output = config.outputDirectory.wstring();
        if (output.size() >= g_outputDirectory.size())
            return false;
        std::wmemcpy(g_outputDirectory.data(), output.c_str(), output.size() + 1);
        std::error_code error;
        std::filesystem::create_directories(config.outputDirectory, error);
        if (error)
            return false;
    }
    CopyText(g_runtimeVersion, config.runtimeVersion.c_str());
    CopyText(g_buildId, config.buildId.c_str());
    CopyText(g_runtimeSha256, config.runtimeSha256.c_str());
    SetUnhandledExceptionFilter(CrashExceptionFilter);
    return true;
}

void ShutdownCrashReporter()
{
    SetUnhandledExceptionFilter(nullptr);
}

void RecordVulkanFailure(const char* call, int32_t result,
                         const char* resultName, uint64_t frame,
                         uint64_t sequence, const char* stage) noexcept
{
    const uint64_t next = g_vulkanFailureGeneration.load(std::memory_order_relaxed) + 1;
    VulkanFailureRecord& record = g_vulkanFailures[next & 1u];
    CopyText(record.call, call);
    CopyText(record.resultName, resultName);
    CopyText(record.stage, stage);
    record.result = result;
    record.frame = frame;
    record.sequence = sequence;
    record.threadId = GetCurrentThreadId();
    g_vulkanFailureGeneration.store(next, std::memory_order_release);
}

} // namespace mojorecomp::host

#else

namespace mojorecomp::host {

bool InitializeCrashReporter(const CrashReporterConfig&) { return false; }
void ShutdownCrashReporter() {}
std::filesystem::path CrashReportDirectoryForConfig(const std::filesystem::path& configPath)
{
    return configPath.parent_path() / "logs";
}
void RecordVulkanFailure(const char*, int32_t, const char*, uint64_t, uint64_t,
                         const char*) noexcept {}

} // namespace mojorecomp::host

#endif
