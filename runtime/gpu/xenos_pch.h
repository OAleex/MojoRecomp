// Compatibility PCH for compiling XenosRecomp's shader translator in-process.
// The upstream PCH also pulls in cache/compression and DXC dependencies that this
// translation unit does not use, so this file intentionally exposes only the
// required endian helpers and standard headers. Keep it synchronized with the
// symbols used by the pinned XenosRecomp source.
#pragma once

#include <bit>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <fmt/core.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

template<typename T>
static T byteSwap(T value)
{
    if constexpr (sizeof(T) == 1)
        return value;
    else if constexpr (sizeof(T) == 2)
        return static_cast<T>(__builtin_bswap16(static_cast<uint16_t>(value)));
    else if constexpr (sizeof(T) == 4)
        return static_cast<T>(__builtin_bswap32(static_cast<uint32_t>(value)));
    else if constexpr (sizeof(T) == 8)
        return static_cast<T>(__builtin_bswap64(static_cast<uint64_t>(value)));

    assert(false && "Unexpected byte size.");
    return value;
}

template<typename T>
struct be
{
    T value;

    T get() const
    {
        if constexpr (std::is_enum_v<T>)
            return T(byteSwap(std::underlying_type_t<T>(value)));
        else
            return byteSwap(value);
    }

    operator T() const
    {
        return get();
    }
};
