#include "guest_fiber.h"
#include "../kernel/memory.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unordered_map>

namespace mojorecomp::fiber {
namespace {
struct Fiber {
    void* host = nullptr;
    uint32_t guest = 0;
    uint32_t stackLimit = 0;
    uint32_t pendingTarget = 0;
};
struct ThreadFibers {
    std::unordered_map<uint32_t, std::unique_ptr<Fiber>> fibers;
    Fiber* root = nullptr;
    Fiber* current = nullptr;
    PPCContext* context = nullptr;
    bool convertedThread = false;
};
thread_local ThreadFibers state;

[[noreturn]] void Fail(const char* message) {
    std::fprintf(stderr, "[cpu fiber] %s (host error %lu)\n", message, GetLastError());
    std::abort();
}

void WINAPI Enter(void*) {
    auto& ctx = *state.context;
    // SwitchToFiber restores host FP control too. The generated switch has
    // already loaded the target FPSCR; synchronize it after the host switch.
    ctx.fpscr.setcsr(ctx.fpscr.csr);
    auto* entry = g_guestMemory.FindFunction(static_cast<uint32_t>(ctx.lr));
    if (!entry)
        Fail("new fiber entry is not registered");
    std::fprintf(stderr, "[cpu fiber] enter context=%08X pc=%08X sp=%08X\n",
                 state.current->guest, uint32_t(ctx.lr), ctx.r1.u32);
    entry(ctx, g_guestMemory.base);
    Fail("guest fiber entry returned without terminating or yielding");
}
} // namespace

void Begin(PPCContext& ctx, uint32_t target, uint32_t pcr) {
    uint8_t* base = g_guestMemory.base;
    const uint32_t thread = PPC_LOAD_U32(pcr + 0x100);
    const uint32_t source = PPC_LOAD_U32(thread + 0x164);
    if (!source || !target)
        Fail("switch requires initialized source and target contexts");
    if (!state.root) {
        auto root = std::make_unique<Fiber>();
        root->guest = source;
        state.convertedThread = !IsThreadAFiber();
        root->host = state.convertedThread
            ? ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH) : GetCurrentFiber();
        if (!root->host)
            Fail("ConvertThreadToFiberEx failed");
        state.root = state.current = root.get();
        state.context = &ctx;
        state.fibers.emplace(source, std::move(root));
    }
    if (state.context != &ctx || state.current->host != GetCurrentFiber())
        Fail("context or native thread ownership changed during a switch");
    if (state.current->guest != source) {
        // A guest may convert its original fiber back to a thread and later
        // create a fresh guest descriptor, while retaining this native stack.
        if (state.current != state.root || state.fibers.contains(source))
            Fail("guest and native fiber identities disagree");
        auto node = state.fibers.extract(state.root->guest);
        node.key() = source;
        state.root->guest = source;
        state.fibers.insert(std::move(node));
    }
    state.current->pendingTarget = target;
}

void Commit(PPCContext& ctx, uint32_t sp, uint32_t thread,
            uint32_t allocationBase, uint32_t stackBase, uint32_t stackLimit) {
    uint8_t* base = g_guestMemory.base;
    Fiber* source = state.current;
    if (!source || !source->pendingTarget || state.context != &ctx)
        Fail("commit without matching begin");
    const uint32_t targetAddress = source->pendingTarget;
    source->pendingTarget = 0;
    if (PPC_LOAD_U32(thread + 0x164) != targetAddress)
        Fail("guest switch did not publish the target context");

    // KeSetCurrentStackPointers tail semantics. Register/CR/FPR/VMX/LR restore
    // is left entirely to the real translated guest routine.
    PPC_STORE_U32(thread + 0xD0, allocationBase);
    PPC_STORE_U32(thread + 0x5C, stackBase);
    PPC_STORE_U32(thread + 0x60, stackLimit);
    ctx.r1.u64 = sp;
    if (source->guest == targetAddress)
        return; // Windows must not SwitchToFiber(GetCurrentFiber()).

    auto found = state.fibers.find(targetAddress);
    if (found == state.fibers.end()) {
        if (!g_guestMemory.FindFunction(static_cast<uint32_t>(ctx.lr)))
            Fail("new fiber resume PC has no native entry");
        auto target = std::make_unique<Fiber>();
        target->guest = targetAddress;
        target->stackLimit = stackLimit;
        // Host call frames are larger than PPC frames. Never size the native
        // stack from the guest's small stack allocation.
        target->host = CreateFiberEx(0x40000, 0x400000, FIBER_FLAG_FLOAT_SWITCH,
                                     Enter, nullptr);
        if (!target->host)
            Fail("CreateFiberEx failed");
        found = state.fibers.emplace(targetAddress, std::move(target)).first;
    }
    state.current = found->second.get();
    SwitchToFiber(state.current->host);
    if (state.current != source)
        Fail("resumed the wrong native fiber");
    ctx.fpscr.setcsr(ctx.fpscr.csr);
}

void ReleaseStack(uint32_t stackLimit) {
    for (auto it = state.fibers.begin(); it != state.fibers.end(); ++it) {
        auto* fiber = it->second.get();
        if (fiber == state.root || fiber->stackLimit != stackLimit)
            continue;
        if (fiber == state.current)
            Fail("attempt to delete the active fiber stack");
        DeleteFiber(fiber->host);
        state.fibers.erase(it);
        return;
    }
}

void Shutdown() {
    if (!state.root)
        return;
    if (state.current != state.root)
        Fail("thread teardown must occur on its original native stack");
    for (const auto& [address, fiber] : state.fibers)
        if (fiber.get() != state.root)
            DeleteFiber(fiber->host);
    if (state.convertedThread && !ConvertFiberToThread())
        Fail("ConvertFiberToThread failed");
    state.fibers.clear();
    state.root = state.current = nullptr;
    state.context = nullptr;
    state.convertedThread = false;
}
} // namespace mojorecomp::fiber
