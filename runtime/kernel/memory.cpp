#include "memory.h"

#include <cstdio>
#include <cstdlib>

#include "../runtime_state.h"

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

GuestMemory g_guestMemory;

namespace {

constexpr std::size_t kPhysicalSize = 0x20000000ull;
constexpr std::size_t kPhysicalViews[] = {
    0xA0000000ull,
    0xC0000000ull,
    0xE0000000ull,
};

static_assert(kPhysicalViews[2] + kPhysicalSize == PPC_MEMORY_SIZE);

bool CheckPhysicalViews(uint8_t* base) {
    constexpr std::size_t probe = 0x4000;
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t value = 0x4D4F4A00u | i;
        *reinterpret_cast<volatile uint32_t*>(base + kPhysicalViews[i] + probe) = value;
        for (std::size_t view : kPhysicalViews) {
            if (*reinterpret_cast<volatile uint32_t*>(base + view + probe) != value)
                return false;
        }
    }
    for (std::size_t view : kPhysicalViews)
        *reinterpret_cast<volatile uint32_t*>(base + view + probe) = 0;
    return true;
}

} // namespace

bool GuestMemory::InsertFunction(uint32_t address, PPCFunc* host) noexcept {
    if (!base || address < PPC_CODE_BASE || address >= PPC_CODE_BASE + PPC_CODE_SIZE)
        return false;
    PPC_LOOKUP_FUNC(base, address) = host;
    return true;
}

void GuestMemory::Init() {
    if (base)
        return;

#if defined(_WIN32)
    HANDLE process = GetCurrentProcess();
    base = static_cast<uint8_t*>(VirtualAlloc2(
        process, nullptr, PPC_MEMORY_SIZE,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
    if (!base) {
        std::fprintf(stderr, "memory: 4 GB placeholder reservation failed (%lu)\n",
                     GetLastError());
        std::abort();
    }

    // Split the low 2.5 GB from the placeholder and replace it with normal committed
    // virtual memory. Pages consume physical backing only when touched.
    if (!VirtualFree(base, kPhysicalViews[0],
                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) ||
        !VirtualAlloc2(process, base, kPhysicalViews[0],
                       MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                       PAGE_READWRITE, nullptr, 0)) {
        std::fprintf(stderr, "memory: low guest mapping failed (%lu)\n", GetLastError());
        std::abort();
    }

    HANDLE physical = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                         PAGE_READWRITE | SEC_COMMIT,
                                         DWORD(uint64_t(kPhysicalSize) >> 32),
                                         DWORD(kPhysicalSize), nullptr);
    if (!physical) {
        std::fprintf(stderr, "memory: physical backing section failed (%lu)\n",
                     GetLastError());
        std::abort();
    }

    for (std::size_t i = 0; i < 3; ++i) {
        if (i != 2 &&
            !VirtualFree(base + kPhysicalViews[i], kPhysicalSize,
                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            std::fprintf(stderr, "memory: physical placeholder split failed (%lu)\n",
                         GetLastError());
            std::abort();
        }
        if (!MapViewOfFile3(physical, process, base + kPhysicalViews[i], 0,
                            kPhysicalSize, MEM_REPLACE_PLACEHOLDER,
                            PAGE_READWRITE, nullptr, 0)) {
            std::fprintf(stderr, "memory: physical alias map failed at 0x%08zX (%lu)\n",
                         kPhysicalViews[i], GetLastError());
            std::abort();
        }
    }
    CloseHandle(physical);
#else
    base = static_cast<uint8_t*>(mmap(nullptr, PPC_MEMORY_SIZE,
                                     PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                                     -1, 0));
    if (base == MAP_FAILED) {
        std::perror("memory: mmap guest space");
        std::abort();
    }

    const int fd = memfd_create("mojorecomp_physical", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, kPhysicalSize) != 0) {
        std::perror("memory: physical backing");
        std::abort();
    }
    for (std::size_t view : kPhysicalViews) {
        if (mmap(base + view, kPhysicalSize, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_FIXED | MAP_NORESERVE, fd, 0) == MAP_FAILED) {
            std::perror("memory: physical alias map");
            std::abort();
        }
    }
    close(fd);
#endif

    g_mojoGuestBase = base;
    if (!CheckPhysicalViews(base)) {
        std::fputs("memory: physical alias self-check failed\n", stderr);
        std::abort();
    }

    std::size_t installed = 0;
    std::size_t refused = 0;
    for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m) {
        if (InsertFunction(static_cast<uint32_t>(m->guest), m->host))
            ++installed;
        else
            ++refused;
    }

    std::fprintf(stderr,
                 "memory: guest base=%p, physical aliases OK, functions=%zu, refused=%zu\n",
                 static_cast<void*>(base), installed, refused);
}

extern "C" void* MmGetHostAddress(uint32_t address) {
    return g_guestMemory.Translate(address);
}
