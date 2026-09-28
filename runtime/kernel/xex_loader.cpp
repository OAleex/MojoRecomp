#include "xex_loader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <xbox.h>
#include <xex.h>

#include "heap.h"
#include "klog.h"
#include "memory.h"

std::atomic<uint32_t> g_xexHeaderBase{0};
std::atomic<uint32_t> g_keTimeStampBundle{0};

namespace {

struct KernelVariable
{
    const char* name;
    uint32_t size;
};

const KernelVariable* LookupKernelVariable(uint32_t ordinal)
{
    // These are XDK kernel data exports, not title-specific addresses. The same
    // ordinal set is used by the static XDK libraries across many retail titles.
    static const struct Entry { uint32_t ordinal; KernelVariable var; } vars[] = {
        {0x00E, {"ExEventObjectType",         0x20}},
        {0x01B, {"ExThreadObjectType",        0x20}},
        {0x059, {"KeDebugMonitorData",        4}},
        {0x0AD, {"KeTimeStampBundle",         0x18}},
        {0x156, {"XboxHardwareInfo",          0x10}},
        {0x158, {"XboxKrnlVersion",           8}},
        {0x193, {"XexExecutableModuleHandle", 4}},
        {0x1AE, {"ExLoadedCommandLine",       1024}},
        {0x1BE, {"VdGlobalDevice",            4}},
        {0x1BF, {"VdGlobalXamDevice",         4}},
        {0x1C0, {"VdGpuClockInMHz",           4}},
        {0x1C1, {"VdHSIOCalibrationLock",     28}},
        {0x266, {"KeCertMonitorData",         4}},
    };
    for (const auto& entry : vars)
        if (entry.ordinal == ordinal)
            return &entry.var;
    return nullptr;
}

uint32_t AllocateVariable(uint32_t ordinal, const char* library)
{
    const KernelVariable* known = LookupKernelVariable(ordinal);
    const uint32_t size = known ? known->size : 16;
    auto* host = static_cast<uint8_t*>(g_guestHeap.Alloc(size));
    if (!host)
        return 0;

    std::memset(host, 0, size);
    const uint32_t guest = g_guestMemory.MapVirtual(host);

    switch (ordinal)
    {
    case 0x0AD:
        g_keTimeStampBundle = guest;
        break;
    case 0x156:
        // XboxHardwareInfo: six logical hardware threads (3 cores x 2 SMT).
        host[4] = 6;
        break;
    case 0x158:
        // Conservative 2.0 kernel version. Build 1888 matches the older XDK path
        // and avoids advertising exports the minimal runtime does not implement yet.
        *reinterpret_cast<be<uint16_t>*>(host + 0) = 2;
        *reinterpret_cast<be<uint16_t>*>(host + 2) = 0;
        *reinterpret_cast<be<uint16_t>*>(host + 4) = 1888;
        break;
    case 0x193:
        *reinterpret_cast<be<uint32_t>*>(host) = g_xexHeaderBase.load();
        break;
    case 0x1AE:
        std::strcpy(reinterpret_cast<char*>(host), "default.xex");
        break;
    case 0x1C0:
        *reinterpret_cast<be<uint32_t>*>(host) = 500;
        break;
    default:
        break;
    }

    std::fprintf(stderr, "[loader] data import %s!%s ord=%03X -> %08X (%u bytes)\n",
                 library ? library : "?", known ? known->name : "<unknown>", ordinal,
                 guest, size);
    return guest;
}

uint32_t GuestU32(uint32_t address)
{
    return *reinterpret_cast<const be<uint32_t>*>(g_guestMemory.Translate(address));
}

} // namespace

uint32_t PublishXexHeaders(const uint8_t* xexFile, size_t xexFileSize)
{
    if (!xexFile || xexFileSize < sizeof(Xex2Header))
        return 0;

    const auto* header = reinterpret_cast<const Xex2Header*>(xexFile);
    const uint32_t headerSize = header->headerSize;
    const uint32_t copy = std::min<uint32_t>(headerSize, static_cast<uint32_t>(xexFileSize));
    if (copy < sizeof(Xex2Header))
        return 0;

    auto* host = static_cast<uint8_t*>(g_guestHeap.Alloc(copy));
    if (!host)
        return 0;
    std::memcpy(host, xexFile, copy);

    const uint32_t guest = g_guestMemory.MapVirtual(host);
    g_xexHeaderBase = guest;
    std::fprintf(stderr, "[loader] XEX headers published at %08X (%u bytes)\n", guest, copy);
    return guest;
}

void ResolveXexDataImports(const uint8_t* xexFile)
{
    const auto* importHeader = reinterpret_cast<const Xex2ImportHeader*>(
        getOptHeaderPtr(xexFile, XEX_HEADER_IMPORT_LIBRARIES));
    if (!importHeader)
    {
        KLOG("XEX has no import-library optional header\n");
        return;
    }

    const uint32_t libraryCount = importHeader->numImports;
    const char* stringTable = reinterpret_cast<const char*>(importHeader + 1);
    const uint32_t stringBytes = importHeader->sizeOfStringTable;

    const char* names[64] = {};
    size_t stringOffset = 0;
    for (uint32_t i = 0; i < libraryCount && i < 64 && stringOffset < stringBytes; ++i)
    {
        names[i] = stringTable + stringOffset;
        const size_t remaining = stringBytes - stringOffset;
        const size_t len = strnlen(names[i], remaining);
        if (len == remaining)
            break;
        stringOffset += (len + 1 + 3) & ~size_t(3);
    }

    const auto* library = reinterpret_cast<const Xex2ImportLibrary*>(
        reinterpret_cast<const uint8_t*>(importHeader + 1) + stringBytes);

    uint32_t functionSlots = 0;
    uint32_t variables = 0;
    for (uint32_t lib = 0; lib < libraryCount; ++lib)
    {
        const uint32_t nameIndex = library->name;
        const char* libraryName = nameIndex < 64 && names[nameIndex] ? names[nameIndex] : "?";
        const uint16_t count = library->numberOfImports;
        const auto* descriptors = reinterpret_cast<const Xex2ImportDescriptor*>(library + 1);

        for (uint16_t i = 0; i < count; ++i)
        {
            const uint32_t slotVA = descriptors[i].firstThunk;
            if (slotVA >= PPC_CODE_BASE)
                continue;
            if (slotVA >= PPC_MEMORY_SIZE)
                continue;

            const bool pairedWithThunk =
                i + 1 < count && uint32_t(descriptors[i + 1].firstThunk) >= PPC_CODE_BASE;
            if (pairedWithThunk)
            {
                *reinterpret_cast<be<uint32_t>*>(g_guestMemory.Translate(slotVA)) =
                    uint32_t(descriptors[i + 1].firstThunk);
                ++functionSlots;
                continue;
            }

            // Image::ParseImage has already run XenonUtils' XEX loader over the
            // image. Type-0 import entries therefore contain the ordinal word in
            // host byte order at this point, matching XenonUtils' thunk rewrite.
            const uint32_t encoded =
                *reinterpret_cast<const uint32_t*>(g_guestMemory.Translate(slotVA));
            const uint32_t ordinal = encoded & 0xFFFFu;
            *reinterpret_cast<be<uint32_t>*>(g_guestMemory.Translate(slotVA)) =
                AllocateVariable(ordinal, libraryName);
            ++variables;
        }

        library = reinterpret_cast<const Xex2ImportLibrary*>(
            reinterpret_cast<const uint8_t*>(library + 1) +
            size_t(count) * sizeof(Xex2ImportDescriptor));
    }

    std::fprintf(stderr, "[loader] resolved %u function IAT slots + %u data imports\n",
                 functionSlots, variables);
}

uint32_t XexHeaderField(uint32_t headerBase, uint32_t key)
{
    if (!headerBase)
        headerBase = g_xexHeaderBase.load();
    if (!headerBase)
        return 0;

    const uint32_t count = GuestU32(headerBase + 0x14);
    if (count > 256)
    {
        KLOG("XexHeaderField: bad header count %u at %08X\n", count, headerBase);
        return 0;
    }

    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t entry = headerBase + 0x18 + i * 8;
        if (GuestU32(entry) != key)
            continue;

        const uint32_t low = key & 0xFFu;
        if (low == 0 || low == 1)
            return entry + 4;
        return headerBase + GuestU32(entry + 4);
    }
    return 0;
}

uint32_t XexTitleId()
{
    const uint32_t info = XexHeaderField(0, XEX_HEADER_EXECUTION_INFO);
    return info ? GuestU32(info + 12) : 0;
}
