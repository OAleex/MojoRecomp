#include "host_paths.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <array>

namespace HostPaths {

std::filesystem::path ExeDir()
{
#ifdef _WIN32
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length && length < buffer.size())
        return std::filesystem::path(buffer.data(), buffer.data() + length).parent_path();
#endif
    return std::filesystem::current_path();
}

} // namespace HostPaths

