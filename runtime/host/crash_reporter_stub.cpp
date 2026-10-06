#include "crash_reporter.h"

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
