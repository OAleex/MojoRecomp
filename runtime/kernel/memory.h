#pragma once

#include <cstddef>
#include <cstdint>

#include "ppc_recomp_shared.h"

class GuestMemory {
public:
    uint8_t* base = nullptr;

    void Init();

    void* Translate(uint32_t address) const noexcept {
        return base ? base + address : nullptr;
    }

    uint32_t MapVirtual(const void* pointer) const noexcept {
        if (!base || !pointer)
            return 0;
        const auto host = reinterpret_cast<uintptr_t>(pointer);
        const auto guestBase = reinterpret_cast<uintptr_t>(base);
        if (host < guestBase || host - guestBase >= PPC_MEMORY_SIZE)
            return 0;
        return static_cast<uint32_t>(host - guestBase);
    }

    PPCFunc* FindFunction(uint32_t address) const noexcept {
        if (!base || address < PPC_CODE_BASE || address >= PPC_CODE_BASE + PPC_CODE_SIZE)
            return nullptr;
        return PPC_LOOKUP_FUNC(base, address);
    }

private:
    bool InsertFunction(uint32_t address, PPCFunc* host) noexcept;
};

extern GuestMemory g_guestMemory;
