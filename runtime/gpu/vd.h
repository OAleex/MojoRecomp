#pragma once

#include <cstdint>

struct MojoRecompVdState
{
    uint32_t interruptCallback;
    uint32_t interruptUserData;
    uint32_t ringBase;
    uint32_t ringSize;
    uint32_t readPointerWriteback;
    uint32_t gpuIdentifierAddress;
};

MojoRecompVdState MojoRecompVdGetState();
