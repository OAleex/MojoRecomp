#pragma once

#include <cstdint>

struct MojoRecompUnimplementedImport
{
    const char* name;
    uint32_t lr;
};

[[noreturn]] void MojoRecompRaiseUnimplementedImport(const char* name, uint32_t lr);
