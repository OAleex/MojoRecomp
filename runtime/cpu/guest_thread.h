#pragma once

#include <cstdint>

#include "ppc_recomp_shared.h"

struct GuestThreadContext
{
    PPCContext ppc{};
    uint8_t* block = nullptr;
    uint32_t pcr = 0;
    uint32_t teb = 0;
    uint32_t threadId = 0;
    uint32_t stackBase = 0;
    uint32_t stackLimit = 0;

    GuestThreadContext(uint32_t cpuNumber, uint32_t stackSize, uint32_t tlsSlots,
                       uint32_t threadId = 0xF00u);
    ~GuestThreadContext();

    GuestThreadContext(const GuestThreadContext&) = delete;
    GuestThreadContext& operator=(const GuestThreadContext&) = delete;
};

struct GuestThreadExit
{
    uint32_t code;
};

// Xbox 360 software threads carry a logical processor identity in the PCR.
// Titles may read PCR+0x10C directly (Crash of the Titans does this for
// per-CPU job queues), so changing affinity must update the guest-visible CPU
// even when host CPU pinning is intentionally disabled.
bool SetGuestThreadLogicalCpu(uint32_t pcr, uint32_t cpuNumber,
                              uint32_t* previousCpu = nullptr);
