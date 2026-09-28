#pragma once
#include <cstdint>
#include "ppc_recomp_shared.h"

namespace mojorecomp::fiber {
// Paired around the guest's own register save/restore code, not a replacement
// for it. Native translated call stacks must switch along with guest stacks.
void Begin(PPCContext& ctx, uint32_t target, uint32_t pcr);
void Commit(PPCContext& ctx, uint32_t stackPointer, uint32_t thread,
            uint32_t stackAllocationBase, uint32_t stackBase, uint32_t stackLimit);
void ReleaseStack(uint32_t stackLimit);
void Shutdown();
}
