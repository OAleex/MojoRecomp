#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

inline bool CotTitleResources_IsBinkShaderPath(std::string_view guestPath)
{
    constexpr std::string_view suffix = "\\shaders\\binkdecompress.out";
    if (guestPath.size() < suffix.size())
        return false;
    guestPath.remove_prefix(guestPath.size() - suffix.size());
    for (std::size_t i = 0; i < suffix.size(); ++i)
    {
        char actual = guestPath[i];
        if (actual == '/')
            actual = '\\';
        if (actual >= 'A' && actual <= 'Z')
            actual = static_cast<char>(actual - 'A' + 'a');
        if (actual != suffix[i])
            return false;
    }
    return true;
}

void CotTitleResources_OnFileOpened(std::string_view guestPath, FILE* file,
                                    uint64_t size, bool truncated);
