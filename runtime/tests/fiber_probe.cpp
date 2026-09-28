// Synthetic contexts through the actual recompiled switch routine. No XEX/assets.
#include "../kernel/guestcall.h"
#include "../cpu/guest_fiber.h"
#include <cstdio>
#include <cstdlib>

namespace {
constexpr uint32_t kMain = 0x10000, kWorker = 0x11000;
constexpr uint32_t kPcr = 0x20000, kThread = 0x21000;
constexpr uint32_t kMainSp = 0x3FF00, kWorkerSp = 0x4FF00;
uint32_t progress;

void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: fiber %s\n", message);
        std::exit(1);
    }
}

void Switch(PPCContext& ctx, uint8_t* base, uint32_t target) {
    ctx.r3.u64 = target;
    sub_823E9C10(ctx, base);
}

void Worker(PPCContext& ctx, uint8_t* base) {
    Check(ctx.r1.u32 == kWorkerSp, "initial guest stack");
    Check(ctx.r31.u64 == 0x123456789ABCDEF0ull, "initial nonvolatile register");
    volatile uint64_t nativeLocal = 0xFA123456ABCDEF00ull;
    ctx.r31.u64 = 0x7766554433221100ull;
    ctx.f14.f64 = 42.5;
    ctx.v127.u64[0] = 0xAABBCCDDEEFF0011ull;
    ctx.fpscr.storeFromGuest(PPC_ROUND_UP);
    for (uint32_t i = 1; ; ++i) {
        progress = i;
        ctx.lr = 0x12340000;
        Switch(ctx, base, kMain);
        Check(nativeLocal == 0xFA123456ABCDEF00ull, "native stack survived suspension");
        Check(ctx.r1.u32 == kWorkerSp && ctx.lr == 0x12340000, "worker SP/LR restore");
        Check(ctx.r31.u64 == 0x7766554433221100ull, "worker r31 restore");
        Check(ctx.f14.f64 == 42.5, "worker FPR restore");
        Check(ctx.v127.u64[0] == 0xAABBCCDDEEFF0011ull, "worker VMX restore");
        Check(ctx.fpscr.loadFromHost() == PPC_ROUND_UP, "worker rounding mode");
    }
}
} // namespace

int RunFiberProbe() {
    g_guestMemory.Init();
    uint8_t* base = g_guestMemory.base;
    PPCContext ctx{};
    g_ppcContext = &ctx;
    PPC_STORE_U32(kPcr + 0x100, kThread);
    PPC_STORE_U32(kThread + 0x164, kMain);
    PPC_STORE_U32(kMain + 4, 0x40000);
    PPC_STORE_U32(kMain + 8, 0x40000);
    PPC_STORE_U32(kMain + 12, 0x30000);
    PPC_STORE_U32(kWorker + 4, 0x50000);
    PPC_STORE_U32(kWorker + 8, 0x50000);
    PPC_STORE_U32(kWorker + 12, 0x40000);
    PPC_STORE_U64(kWorker + 48, kWorkerSp);
    PPC_STORE_U64(kWorker + 288, 0x123456789ABCDEF0ull);
    PPC_STORE_U32(kWorker + 28, PPC_CODE_BASE);
    // The synthetic callback replaces a mapping only in this test process.
    PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = Worker;
    ctx.r13.u64 = kPcr;
    ctx.r1.u64 = kMainSp;
    ctx.r31.u64 = 0xFFEEDDCCBBAA9988ull;
    ctx.f14.f64 = -19.75;
    ctx.v127.u64[0] = 0x0123456789ABCDEFull;
    ctx.fpscr.loadFromHost();
    ctx.fpscr.storeFromGuest(PPC_ROUND_DOWN);
    for (uint32_t i = 1; i <= 100; ++i) {
        ctx.lr = 0x56780000;
        Switch(ctx, base, kWorker);
        Check(progress == i, "worker did not execute before caller resumed");
        Check(ctx.r1.u32 == kMainSp && ctx.lr == 0x56780000, "main SP/LR restore");
        Check(ctx.r31.u64 == 0xFFEEDDCCBBAA9988ull, "main r31 restore");
        Check(ctx.f14.f64 == -19.75, "main FPR restore");
        Check(ctx.v127.u64[0] == 0x0123456789ABCDEFull, "main VMX restore");
        Check(ctx.fpscr.loadFromHost() == PPC_ROUND_DOWN, "main rounding mode");
        Check(PPC_LOAD_U32(kThread + 0x164) == kMain, "current guest fiber identity");
    }
    mojorecomp::fiber::Shutdown();
    std::puts("PASS: fiber execution, 100 round trips, native/PPC stack and GPR/FPR/VMX preservation");
    return 0;
}
