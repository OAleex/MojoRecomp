#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace mojorecomp::host {

struct CrashReporterConfig
{
    std::filesystem::path outputDirectory;
    std::string runtimeVersion;
    std::string buildId;
    std::string runtimeSha256;
};

bool InitializeCrashReporter(const CrashReporterConfig& config);
void ShutdownCrashReporter();

std::filesystem::path CrashReportDirectoryForConfig(
    const std::filesystem::path& configPath);

void RecordVulkanFailure(const char* call, int32_t result,
                         const char* resultName, uint64_t frame,
                         uint64_t sequence, const char* stage) noexcept;

} // namespace mojorecomp::host
