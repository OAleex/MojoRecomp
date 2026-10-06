#include <cstdint>
#include <cstdio>
#include <vector>

#include "../gpu/guest_memory_snapshot.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

int Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using namespace mojorecomp::gpu;

    std::vector<uint8_t> guest(0x4000, 0);
    constexpr uint32_t address = 0x1000;
    for (uint32_t i = 0; i < 32; ++i)
        guest[address + i] = static_cast<uint8_t>(0x40u + i);

    GuestMemorySnapshot snapshot;
    if (!snapshot.Capture(guest.data(), address, 32))
        return Fail("capture failed");
    if (snapshot.ByteSize() != 32)
        return Fail("captured byte count is wrong");
    if (!snapshot.Capture(guest.data(), address + 4, 8) || snapshot.ByteSize() != 32)
        return Fail("contained capture was not deduplicated");

    GuestMemoryIdentity fullIdentity{};
    GuestMemoryIdentity subIdentity{};
    if (!snapshot.ResolveIdentity(address, 32, fullIdentity) || !fullIdentity)
        return Fail("captured range has no immutable identity");
    if (!snapshot.ResolveIdentity(address + 4, 8, subIdentity) ||
        subIdentity.token != fullIdentity.token || subIdentity.offset != 4)
        return Fail("contained range did not preserve identity and offset");

    guest[address + 7] = 0xEE;
    {
        ScopedGuestMemorySnapshot active(&snapshot);
        const uint8_t* frozen = GuestReadPtr(guest.data(), address, 32);
        if (!frozen || frozen[7] != 0x47)
            return Fail("active snapshot did not preserve submission-time bytes");
        GuestMemoryIdentity activeIdentity{};
        if (!GuestReadIdentity(address + 4, 8, activeIdentity) ||
            activeIdentity.token != fullIdentity.token || activeIdentity.offset != 4)
            return Fail("active snapshot identity did not match captured range");

        const uint8_t* missing = GuestReadPtr(guest.data(), address + 0x100, 4);
        if (!missing || missing[0] != 0 || !GuestSnapshotReadFailed())
            return Fail("missing active-snapshot read did not fail with immutable zero data");
    }

    GuestMemoryIdentity inactiveIdentity{};
    if (GuestReadIdentity(address, 32, inactiveIdentity))
        return Fail("inactive snapshot unexpectedly exposed an identity");

    const uint8_t* live = GuestReadPtr(guest.data(), address, 32);
    if (!live || live[7] != 0xEE)
        return Fail("live reads were not restored after snapshot scope");

#if defined(_WIN32)
    constexpr size_t physicalBytes = 0x10000;
    constexpr uintptr_t physicalViews[] = {
        0xA0000000ull, 0xC0000000ull, 0xE0000000ull};
    HANDLE process = GetCurrentProcess();
    auto* aliasBase = static_cast<uint8_t*>(VirtualAlloc2(
        process, nullptr, 1ull << 32,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
    if (!aliasBase)
        return Fail("physical-alias test could not reserve guest address space");

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                        PAGE_READWRITE | SEC_COMMIT,
                                        0, physicalBytes, nullptr);
    if (!mapping)
        return Fail("physical-alias test could not create shared backing");
    for (uintptr_t view : physicalViews)
    {
        if (!VirtualFree(aliasBase + view, physicalBytes,
                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) ||
            !MapViewOfFile3(mapping, process, aliasBase + view, 0, physicalBytes,
                            MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
        {
            CloseHandle(mapping);
            return Fail("physical-alias test could not map all guest views");
        }
    }
    CloseHandle(mapping);

    constexpr uint32_t trackedAddress = 0xA0001000u;
    aliasBase[trackedAddress] = 0x31;
    GuestMemorySnapshot beforeAliasWrite;
    if (!beforeAliasWrite.Capture(aliasBase, trackedAddress, 1))
        return Fail("tracked alias snapshot capture failed");
    aliasBase[0xC0001000u] = 0x72;

    GuestMemorySnapshot afterAliasWrite;
    if (!afterAliasWrite.Capture(aliasBase, trackedAddress, 1))
        return Fail("tracked alias recapture failed");
    const uint8_t* refreshed = afterAliasWrite.Resolve(trackedAddress, 1);
    if (!refreshed || refreshed[0] != 0x72)
        return Fail("write through a physical alias reused stale snapshot bytes");
    GuestSnapshotCacheShutdown();
#endif

    std::puts("PASS: guest memory snapshot freezes draw-visible bytes and restores live reads.");
    return 0;
}
