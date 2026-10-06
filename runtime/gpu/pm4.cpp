#include "pm4.h"

#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "../kernel/klog.h"
#include "xenos.h"

namespace {

constexpr uint32_t kPhysicalBase = 0xA0000000u;
constexpr uint32_t kPhysicalEnd = 0xBFFF0000u;
constexpr uint32_t kRegisterCount = 0x8000u;
constexpr uint32_t kScratchUmsk = 0x01DCu;
constexpr uint32_t kScratchAddr = 0x01DDu;
constexpr uint32_t kScratch0 = 0x0578u;
constexpr uint32_t kScratch7 = 0x057Fu;
constexpr uint32_t kCoherStatusHost = 0x0A31u;

std::atomic<uint32_t> g_ringBase{0};
std::atomic<uint32_t> g_ringDwords{0};
std::atomic<uint32_t> g_cursor{0};
std::atomic<uint32_t> g_rptrSlot{0};
std::atomic<uint32_t> g_rptrUpdateFreq{1};
std::atomic<uint32_t> g_counter{0};

std::array<uint32_t, kRegisterCount> g_registers{};
uint64_t g_binMask = ~0ull;
uint64_t g_binSelect = ~0ull;
uint64_t g_frames = 0;

bool IbProfileEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_IB_PROFILE");
        return value && value[0] != '0';
    }();
    return enabled;
}

bool Pm4PacketProfileEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_PM4_PROFILE");
        return value && value[0] != '0';
    }();
    return enabled;
}

bool Pm4TimingProfileEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_PM4_TIME_PROFILE");
        return value && value[0] != '0';
    }();
    return enabled;
}

bool Pm4RuntimeTimingEnabled()
{
    static const bool enabled = [] {
        if (const char* overrideValue = std::getenv("MOJORECOMP_PM4_RUNTIME_TIMING");
            overrideValue && *overrideValue)
        {
            return overrideValue[0] != '0';
        }
        const char* logRoot = std::getenv("MOJORECOMP_LOG_ROOT");
        return logRoot && *logRoot;
    }();
    return enabled;
}

bool Pm4DrawSinkTimingEnabled()
{
    return Pm4RuntimeTimingEnabled() || IbProfileEnabled();
}

uint64_t g_packets = 0;
uint64_t g_indirectBuffers = 0;
uint64_t g_interrupts = 0;
uint64_t g_gpuStores = 0;
uint64_t g_waits = 0;
uint64_t g_waitStalls = 0;
uint64_t g_waitForIdle = 0;
uint64_t g_draws = 0;
uint64_t g_shaderBinds = 0;
uint64_t g_shaderCacheHits = 0;
std::array<uint64_t, 128> g_profileOpcodePackets{};
std::array<uint64_t, 128> g_profileOpcodeDwords{};
std::array<uint64_t, 3> g_profileTypePackets{};
std::array<uint64_t, 3> g_profileTypeDwords{};

// Sparse CPU timing profiler. Type-3 packets use buckets 0..127 (their opcode),
// while Type-0/1/2 packets use 128..130. Sampling one packet in 256 keeps the
// diagnostic cheap enough to run through a real cutscene.
constexpr uint32_t kTimingBucketCount = 131;
constexpr uint32_t kTimingType0Bucket = 128;
constexpr uint32_t kTimingType1Bucket = 129;
constexpr uint32_t kTimingType2Bucket = 130;
std::array<uint64_t, kTimingBucketCount> g_timingSamples{};
std::array<uint64_t, kTimingBucketCount> g_timingNs{};
uint64_t g_timingSequence = 0;
uint64_t g_drawSinkNs = 0;
std::atomic<uint64_t> g_executeNs{0};
std::atomic<uint64_t> g_executeDrawSinkNs{0};
std::atomic<uint64_t> g_executeCalls{0};

uint32_t TimingBucketForHeader(uint32_t header)
{
    const uint32_t type = header >> 30;
    if (type == 0) return kTimingType0Bucket;
    if (type == 1) return kTimingType1Bucket;
    if (type == 2) return kTimingType2Bucket;
    return (header >> 8) & 0x7Fu;
}

const char* TimingBucketName(uint32_t bucket)
{
    if (bucket == kTimingType0Bucket) return "TYPE0";
    if (bucket == kTimingType1Bucket) return "TYPE1";
    if (bucket == kTimingType2Bucket) return "TYPE2";
    switch (bucket)
    {
        case 0x22: return "DRAW_INDX";
        case 0x23: return "VIZ_QUERY";
        case 0x26: return "WAIT_FOR_IDLE";
        case 0x32: return "INDIRECT_BUFFER";
        case 0x33: return "INDIRECT_BUFFER_PFD";
        case 0x3F: return "INDIRECT_BUFFER";
        case 0x64: return "XE_SWAP";
        case 0x34: return "DRAW_INDX_2";
        case 0x35: return "DRAW_INDX_BIN";
        case 0x36: return "DRAW_INDX_2_BIN";
        case 0x3C: return "WAIT_REG_MEM";
        case 0x46: return "EVENT_WRITE";
        case 0x47: return "EVENT_WRITE_SHD";
        case 0x48: return "EVENT_WRITE_CFL";
        case 0x4A: return "EVENT_WRITE_ZPD";
        case 0x50: return "SET_BIN_MASK_LO";
        case 0x51: return "SET_BIN_MASK_HI";
        case 0x52: return "WAIT_REG_EQ";
        case 0x53: return "WAIT_REG_GTE";
        case 0x5A: return "EVENT_WRITE_EXT";
        case 0x5C: return "WAIT_UNTIL_READ";
        case 0x5D: return "WAIT_IB_PFD_COMPLETE";
        default: return "TYPE3";
    }
}

struct ScopedPacketTiming
{
    uint32_t bucket = 0;
    bool sampled = false;
    std::chrono::steady_clock::time_point start{};

    explicit ScopedPacketTiming(uint32_t header)
    {
        if (!Pm4TimingProfileEnabled())
            return;
        const uint64_t sequence = ++g_timingSequence;
        if ((sequence & 0xFFu) != 0)
            return;
        bucket = TimingBucketForHeader(header);
        sampled = true;
        start = std::chrono::steady_clock::now();
    }

    ~ScopedPacketTiming()
    {
        if (!sampled)
            return;
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
        ++g_timingSamples[bucket];
        g_timingNs[bucket] += ns;
    }
};

void ProfilePacketType(uint32_t type, uint32_t dwords)
{
    if (!Pm4PacketProfileEnabled() || type > 2)
        return;
    ++g_profileTypePackets[type];
    g_profileTypeDwords[type] += dwords;
}

void ProfilePacketOpcode(uint32_t opcode, uint32_t dwords)
{
    if (!Pm4PacketProfileEnabled() || opcode >= g_profileOpcodePackets.size())
        return;
    ++g_profileOpcodePackets[opcode];
    g_profileOpcodeDwords[opcode] += dwords;
}

void (*g_interruptSink)() = nullptr;
void (*g_interruptCommandSink)() = nullptr;
void (*g_interruptWakeSink)() = nullptr;
void (*g_shaderSink)(uint32_t, uint64_t, const uint8_t*, uint32_t) = nullptr;
void (*g_registerSink)(uint32_t, uint32_t) = nullptr;
void (*g_registerBatchSink)(const uint32_t*, const uint32_t*, std::size_t) = nullptr;
void (*g_storeSink)(uint8_t*, uint32_t, uint32_t) = nullptr;
void (*g_drawSink)(uint8_t*, const Pm4Draw&) = nullptr;
void (*g_swapSink)(uint8_t*, uint32_t, uint32_t, uint32_t) = nullptr;

Pm4ShaderBinding g_boundShaders[2]{};
std::vector<uint8_t> g_shaderStaging;
std::vector<uint64_t> g_announcedShaders[2];
std::atomic<uint64_t> g_rendererInterruptRequested{0};
std::atomic<uint64_t> g_rendererInterruptDelivered{0};
std::atomic<bool> g_rendererInterruptPending{false};
std::atomic<uint64_t> g_binkPixelShaderHash{0};
std::atomic<bool> g_binkVideoCadenceActive{false};
bool g_binkDrawSeenThisFrame = false;

void ServiceRendererInterruptRequests()
{
    if (!g_rendererInterruptPending.load(std::memory_order_acquire))
        return;
    if (!g_rendererInterruptPending.exchange(false, std::memory_order_acq_rel))
        return;

    while (g_rendererInterruptDelivered.load(std::memory_order_relaxed) <
           g_rendererInterruptRequested.load(std::memory_order_acquire))
    {
        if (g_interruptSink)
            g_interruptSink();
        g_rendererInterruptDelivered.fetch_add(1, std::memory_order_release);
    }
}

struct ShaderBindCacheEntry
{
    uint32_t va = 0;
    uint32_t sizeDwords = 0;
    uint64_t fingerprint = 0;
    uint64_t hash = 0;
};

std::vector<ShaderBindCacheEntry> g_shaderBindCache[2];

struct StallPlan {
    std::array<uint32_t, 9> va{};
    std::array<uint32_t, 9> pos{};
    bool pending = false;
};

StallPlan g_resumePlan{};
StallPlan g_nextPlan{};
bool g_stallHit = false;

inline uint32_t GuestLoad32(const uint8_t* base, uint32_t address)
{
    const auto* p = reinterpret_cast<const volatile uint32_t*>(base + address);
    return __builtin_bswap32(*p);
}

inline void GuestStore32(uint8_t* base, uint32_t address, uint32_t value)
{
    auto* p = reinterpret_cast<volatile uint32_t*>(base + address);
    *p = __builtin_bswap32(value);
}

inline uint32_t PhysicalToCached(uint32_t address)
{
    return kPhysicalBase | (address & 0x1FFFFFFFu);
}

inline uint32_t GpuSwap(uint32_t value, uint32_t endian)
{
    switch (endian & 3u)
    {
        default:
        case 0: return value;
        case 1: return ((value & 0x00FF00FFu) << 8) | ((value & 0xFF00FF00u) >> 8);
        case 2: return __builtin_bswap32(value);
        case 3: return (value >> 16) | (value << 16);
    }
}

inline uint32_t GpuSwapResidual(uint32_t value, uint32_t endian)
{
    return GpuSwap(value, (endian & 3u) ^ 2u);
}

bool StoreGpuRaw(uint8_t* base, uint32_t physicalAddress, uint32_t value)
{
    const uint32_t va = PhysicalToCached(physicalAddress & ~3u);
    if (va < kPhysicalBase || uint64_t(va) + 4 > kPhysicalEnd)
        return false;
    if (g_storeSink)
        g_storeSink(base, va, value);
    else
        GuestStore32(base, va, value);
    ++g_gpuStores;
    return true;
}

void StoreGpu(uint8_t* base, uint32_t addressDword, uint32_t value,
              uint32_t indexDwords = 0)
{
    const uint32_t address = (addressDword & ~3u) + indexDwords * 4u;
    StoreGpuRaw(base, address, GpuSwapResidual(value, addressDword));
}

bool EvalWaitCondition(uint32_t info, uint32_t value, uint32_t mask, uint32_t ref);

uint32_t LoadGpu(const uint8_t* base, uint32_t addressDword)
{
    const uint32_t va = PhysicalToCached(addressDword & ~3u);
    if (va < kPhysicalBase || uint64_t(va) + 4 > kPhysicalEnd)
        return 0;
    return GpuSwapResidual(GuestLoad32(base, va), addressDword);
}

uint64_t Fnv1a(const uint8_t* bytes, size_t size)
{
    uint64_t hash = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

uint32_t ReadBe32(const uint8_t* bytes)
{
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

uint64_t ShaderFingerprint(const uint8_t* code, uint32_t sizeDwords)
{
    if (!code || !sizeDwords)
        return 0;
    // Shader identity is correctness-sensitive: Bink cadence and translation
    // caches consume it. Hash the complete payload so an in-place rewrite can
    // never be mistaken for the previous shader because unsampled words changed.
    return Fnv1a(code, size_t(sizeDwords) * sizeof(uint32_t));
}

bool LooksLikeUcode(const uint8_t* code, uint32_t sizeDwords)
{
    if (!code || sizeDwords < 3 || (sizeDwords % 3) != 0)
        return false;

    auto dword = [&](uint32_t i) {
        const uint8_t* p = code + size_t(i) * 4;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    };

    for (uint32_t i = 0; i + 2 < sizeDwords; i += 3)
    {
        const uint32_t a = dword(i);
        const uint32_t b = dword(i + 1);
        const uint32_t c = dword(i + 2);
        const uint32_t cf[2][2] = {
            {a, b & 0xFFFFu},
            {(b >> 16) | (c << 16), c >> 16},
        };

        for (const auto& insn : cf)
        {
            const uint32_t opcode = (insn[1] >> 12) & 0xFu;
            const uint32_t address = insn[0] & 0xFFFu;
            const uint32_t count = (insn[0] >> 12) & 7u;
            const bool exec = opcode == 1 || opcode == 2 || opcode == 3 ||
                              opcode == 4 || opcode == 5 || opcode == 6 ||
                              opcode == 13 || opcode == 14;
            if (exec && count)
                return (address + count) * 3u <= sizeDwords;
        }
    }
    return false;
}

void BindShader(uint32_t type, uint32_t va, const uint8_t* code, uint32_t sizeDwords)
{
    if (type > 1 || !code || !sizeDwords || sizeDwords > 0x10000u)
        return;

    const uint64_t fingerprint = ShaderFingerprint(code, sizeDwords);
    auto& cache = g_shaderBindCache[type];
    for (const auto& entry : cache)
    {
        if (entry.va != va || entry.sizeDwords != sizeDwords ||
            entry.fingerprint != fingerprint)
            continue;

        g_boundShaders[type] = {va, sizeDwords, entry.hash};
        ++g_shaderBinds;
        ++g_shaderCacheHits;

        // Normally the first full bind already announced this hash. Keep the
        // existing contract correct if sinks are installed after cache warmup.
        auto& announced = g_announcedShaders[type];
        if (std::find(announced.begin(), announced.end(), entry.hash) == announced.end())
        {
            announced.push_back(entry.hash);
            if (g_shaderSink)
                g_shaderSink(type, entry.hash, code, sizeDwords);
        }
        return;
    }

    if (!LooksLikeUcode(code, sizeDwords))
    {
        static std::atomic<uint64_t> rejected{0};
        const uint64_t n = rejected.fetch_add(1, std::memory_order_relaxed);
        if (n < 8)
            KLOG("PM4 IM_LOAD rejected non-microcode %s va=%08X size=%u\n",
                 type == 0 ? "VS" : "PS", va, sizeDwords);
        return;
    }

    const uint64_t hash = Fnv1a(code, size_t(sizeDwords) * 4u);
    // Guest-address loads normally keep one immutable shader at a VA. Replace
    // a stale same-VA entry when the content stamp changed; immediate shaders
    // use VA zero and may legitimately have many distinct payloads of one size.
    if (va != 0)
    {
        auto it = std::find_if(cache.begin(), cache.end(), [&](const ShaderBindCacheEntry& entry) {
            return entry.va == va && entry.sizeDwords == sizeDwords;
        });
        if (it != cache.end())
            *it = {va, sizeDwords, fingerprint, hash};
        else
            cache.push_back({va, sizeDwords, fingerprint, hash});
    }
    else
    {
        cache.push_back({va, sizeDwords, fingerprint, hash});
    }
    g_boundShaders[type] = {va, sizeDwords, hash};
    ++g_shaderBinds;

    auto& announced = g_announcedShaders[type];
    if (std::find(announced.begin(), announced.end(), hash) != announced.end())
        return;
    announced.push_back(hash);

    if (g_shaderSink)
        g_shaderSink(type, hash, code, sizeDwords);
}

uint32_t ConstantRegisterBase(uint32_t descriptor)
{
    uint32_t index = descriptor & 0x7FFu;
    switch ((descriptor >> 16) & 0xFFu)
    {
        case 0: return index + 0x4000u;
        case 1: return index + 0x4800u;
        case 2: return index + 0x4900u;
        case 3: return index + 0x4908u;
        case 4: return index + 0x2000u;
        default: return kRegisterCount;
    }
}

void WriteRegister(uint8_t* base, uint32_t index, uint32_t value)
{
    if (index >= kRegisterCount)
        return;
    const bool changed = g_registers[index] != value;
    g_registers[index] = value;
    // The renderer sink mirrors Xenos state; it doesn't observe register-write
    // events themselves. Avoid forwarding an identical value into the async
    // executor while keeping the PM4 write and all local side effects below.
    if (changed && g_registerSink)
        g_registerSink(index, value);

    if (index >= kScratch0 && index <= kScratch7)
    {
        const uint32_t scratch = index - kScratch0;
        if (g_registers[kScratchUmsk] & (1u << scratch))
            StoreGpuRaw(base, g_registers[kScratchAddr] + scratch * 4u, value);
    }
}

template <typename Loader>
void WriteRegisterRange(uint8_t* base, uint32_t start, uint32_t count, Loader&& load)
{
    if (!count)
        return;

    const uint64_t end = uint64_t(start) + count;
    if (start <= kScratch7 && end > kScratch0)
    {
        for (uint32_t i = 0; i < count; ++i)
            WriteRegister(base, start + i, load(i));
        return;
    }

    constexpr std::size_t kBatch = 64;
    std::array<uint32_t, kBatch> indices;
    std::array<uint32_t, kBatch> values;
    std::size_t pending = 0;

    auto flush = [&] {
        if (!pending)
            return;
        if (g_registerBatchSink)
            g_registerBatchSink(indices.data(), values.data(), pending);
        else if (g_registerSink)
            for (std::size_t i = 0; i < pending; ++i)
                g_registerSink(indices[i], values[i]);
        pending = 0;
    };

    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t index = start + i;
        const uint32_t value = load(i);
        if (index >= kRegisterCount || g_registers[index] == value)
            continue;
        g_registers[index] = value;
        indices[pending] = index;
        values[pending] = value;
        if (++pending == kBatch)
            flush();
    }
    flush();
}

bool EvalWaitCondition(uint32_t info, uint32_t value, uint32_t mask, uint32_t ref)
{
    const uint32_t v = value & mask;
    switch (info & 7u)
    {
        case 0: return false;
        case 1: return v < ref;
        case 2: return v <= ref;
        case 3: return v == ref;
        case 4: return v != ref;
        case 5: return v >= ref;
        case 6: return v > ref;
        default: return true;
    }
}

struct Source {
    const uint8_t* base = nullptr;
    uint32_t va = 0;
    uint32_t wrapDwords = 0;

    uint32_t operator()(uint32_t position) const
    {
        if (wrapDwords && position >= wrapDwords)
            position -= wrapDwords;
        return GuestLoad32(base, va + position * 4u);
    }
};

uint32_t ExecuteLinear(uint8_t* base, uint32_t va, uint32_t sizeDwords, int depth);

void NoteStall(const Source& source, uint32_t position, int depth)
{
    g_stallHit = true;
    g_nextPlan.pending = true;
    if (depth > 0 && depth < static_cast<int>(g_nextPlan.va.size()))
    {
        g_nextPlan.va[depth] = source.va;
        g_nextPlan.pos[depth] = position;
    }
}

const char* OpcodeName(uint32_t opcode)
{
    switch (opcode)
    {
        case 0x10: return "NOP";
        case 0x21: return "REG_RMW";
        case 0x22: return "DRAW_INDX";
        case 0x23: return "VIZ_QUERY";
        case 0x25: return "SET_STATE";
        case 0x26: return "WAIT_FOR_IDLE";
        case 0x27: return "IM_LOAD";
        case 0x2B: return "IM_LOAD_IMMEDIATE";
        case 0x2C: return "IM_STORE";
        case 0x2D: return "SET_CONSTANT";
        case 0x2E: return "LOAD_CONSTANT_CONTEXT";
        case 0x2F: return "LOAD_ALU_CONSTANT";
        case 0x34: return "DRAW_INDX_BIN";
        case 0x35: return "DRAW_INDX_2_BIN";
        case 0x36: return "DRAW_INDX_2";
        case 0x37: return "INDIRECT_BUFFER_PFD";
        case 0x3B: return "INVALIDATE_STATE";
        case 0x3C: return "WAIT_REG_MEM";
        case 0x3D: return "MEM_WRITE";
        case 0x3E: return "REG_TO_MEM";
        case 0x3F: return "INDIRECT_BUFFER";
        case 0x44: return "COND_EXEC";
        case 0x45: return "COND_WRITE";
        case 0x46: return "EVENT_WRITE";
        case 0x48: return "ME_INIT";
        case 0x4A: return "SET_SHADER_BASES";
        case 0x4B: return "SET_BIN_BASE_OFFSET";
        case 0x4F: return "MEM_WRITE_CNTR";
        case 0x50: return "SET_BIN_MASK";
        case 0x51: return "SET_BIN_SELECT";
        case 0x52: return "WAIT_REG_EQ";
        case 0x53: return "WAIT_REG_GTE";
        case 0x54: return "INTERRUPT";
        case 0x55: return "SET_CONSTANT2";
        case 0x56: return "SET_SHADER_CONSTANTS";
        case 0x58: return "EVENT_WRITE_SHD";
        case 0x59: return "EVENT_WRITE_CFL";
        case 0x5A: return "EVENT_WRITE_EXT";
        case 0x5B: return "EVENT_WRITE_ZPD";
        case 0x5C: return "WAIT_UNTIL_READ";
        case 0x5D: return "WAIT_IB_PFD_COMPLETE";
        case 0x5E: return "CONTEXT_UPDATE";
        case 0x60: return "SET_BIN_MASK_LO";
        case 0x61: return "SET_BIN_MASK_HI";
        case 0x62: return "SET_BIN_SELECT_LO";
        case 0x63: return "SET_BIN_SELECT_HI";
        case 0x64: return "XE_SWAP";
        default: return nullptr;
    }
}

uint32_t ExecutePacket(uint8_t* base, const Source& fetch, uint32_t position,
                       uint32_t available, int depth)
{
    // The async renderer may reach an INTERRUPT while the walk is already
    // parsing later packets. Deliver the ISR only on this PM4 thread, but at
    // the renderer's exact stream position; post-interrupt work remains parked
    // behind the worker handshake until this request is acknowledged.
    ServiceRendererInterruptRequests();
    const uint32_t header = fetch(position);
    ScopedPacketTiming timing(header);
    const uint32_t type = header >> 30;

    if (header == 0 && depth == 0)
        return 0;

    if (type == 2)
    {
        ProfilePacketType(2, 1);
        ++g_packets;
        return 1;
    }

    if (type == 1)
    {
        if (available < 3)
            return 0;
        ProfilePacketType(1, 3);
        WriteRegister(base, header & 0x7FFu, fetch(position + 1));
        WriteRegister(base, (header >> 11) & 0x7FFu, fetch(position + 2));
        ++g_packets;
        return 3;
    }

    const uint32_t bodyCount = ((header >> 16) & 0x3FFFu) + 1u;
    if (available < bodyCount + 1u)
        return 0;

    if (type == 0)
    {
        ProfilePacketType(0, bodyCount + 1u);
        const uint32_t reg = header & 0x7FFFu;
        const bool oneRegister = ((header >> 15) & 1u) != 0;
        if (oneRegister)
        {
            for (uint32_t i = 0; i < bodyCount; ++i)
                WriteRegister(base, reg, fetch(position + 1u + i));
        }
        else
        {
            WriteRegisterRange(base, reg, bodyCount, [&](uint32_t i) {
                return fetch(position + 1u + i);
            });
        }
        ++g_packets;
        return bodyCount + 1u;
    }

    const uint32_t opcode = (header >> 8) & 0x7Fu;
    ProfilePacketOpcode(opcode, bodyCount + 1u);
    // ReXGlue / Xenia Type-3 semantics: when the packet predicate bit is set,
    // execute the packet iff at least one selected bin is enabled by the mask.
    // Do not promote a draw from another EDRAM tile into the current replay.
    const bool predicated = (header & 1u) && ((g_binMask & g_binSelect) == 0);
    if (predicated)
    {
        ++g_packets;
        return bodyCount + 1u;
    }

    auto body = [&](uint32_t index) { return fetch(position + 1u + index); };

    switch (opcode)
    {
        case 0x22: // DRAW_INDX
        case 0x36: // DRAW_INDX_2
        {
            ++g_draws;
            if (!g_drawSink)
                break;

            const uint32_t initiatorAt = opcode == 0x22 ? 1u : 0u;
            if (bodyCount <= initiatorAt)
                break;

            const uint32_t initiator = body(initiatorAt);
            Pm4Draw draw{};
            draw.primType = initiator & 0x3Fu;
            const uint32_t sourceSelect = (initiator >> 6) & 3u;
            draw.index32 = ((initiator >> 11) & 1u) != 0;
            draw.indexCount = initiator >> 16;
            const uint32_t packetPosition = position;
            draw.packetVa = fetch.va + packetPosition * 4u;
            draw.packetSourceVa = fetch.va;
            draw.packetPosition = packetPosition;
            draw.packetDepth = static_cast<uint32_t>(std::max(depth, 0));
            draw.packetHeader = header;
            draw.predicateForced = false;
            draw.predicated = (header & 1u) != 0;
            draw.binMask = g_binMask;
            draw.binSelect = g_binSelect;
            draw.indexed = sourceSelect == 0;

            if (draw.indexed && bodyCount > initiatorAt + 2u)
            {
                const uint32_t addressDword = body(initiatorAt + 1u);
                const uint32_t sizeDword = body(initiatorAt + 2u);
                draw.indexVa = PhysicalToCached(addressDword);
                draw.indexEndian = sizeDword >> 30;
                draw.indexEndianTop = sizeDword >> 30;
                draw.indexSizeDword = sizeDword;

                if ((sizeDword & 0xFFFFFFu) != draw.indexCount)
                {
                    static std::atomic<uint64_t> mismatch{0};
                    const uint64_t n = mismatch.fetch_add(1, std::memory_order_relaxed);
                    if (n < 8)
                        KLOG("PM4 DRAW_INDX count mismatch init=%u size=%u raw=%08X\n",
                             draw.indexCount, sizeDword & 0xFFFFFFu, sizeDword);
                }
            }
            else if (draw.indexed)
            {
                draw.indexed = false;
            }

            const uint64_t binkPixelShader =
                g_binkPixelShaderHash.load(std::memory_order_acquire);
            const bool binkDraw =
                binkPixelShader && g_boundShaders[1].hash == binkPixelShader;
            if (binkDraw)
            {
                g_binkDrawSeenThisFrame = true;
                g_binkVideoCadenceActive.store(true, std::memory_order_release);
                static std::atomic<uint32_t> binkDrawReports{0};
                const uint32_t n = binkDrawReports.fetch_add(1, std::memory_order_relaxed);
                if (n < 48)
                    KLOG("PM4 Bink draw n=%u frame=%llu mask=%016llX select=%016llX "
                         "winTL=%08X winBR=%08X winOff=%08X header=%08X\n",
                         n + 1,
                         static_cast<unsigned long long>(g_frames + 1),
                         static_cast<unsigned long long>(g_binMask),
                         static_cast<unsigned long long>(g_binSelect),
                         g_registers[0x2081], g_registers[0x2082], g_registers[0x2080], header);
            }

            if (Pm4DrawSinkTimingEnabled())
            {
                const auto drawSinkStart = std::chrono::steady_clock::now();
                g_drawSink(base, draw);
                g_drawSinkNs += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - drawSinkStart).count());
            }
            else
            {
                g_drawSink(base, draw);
            }
            break;
        }

        case 0x21:
            if (bodyCount >= 3)
            {
                const uint32_t info = body(0);
                const uint32_t andMask = body(1);
                const uint32_t orMask = body(2);
                const uint32_t dst = info & 0x1FFFu;
                uint32_t value = dst < kRegisterCount ? g_registers[dst] : 0;
                value &= (info & 0x80000000u)
                             ? g_registers[andMask & (kRegisterCount - 1u)]
                             : andMask;
                value |= (info & 0x40000000u)
                             ? g_registers[orMask & (kRegisterCount - 1u)]
                             : orMask;
                WriteRegister(base, dst, value);
            }
            break;

        case 0x2D: // SET_CONSTANT
            if (bodyCount >= 2)
            {
                const uint32_t start = ConstantRegisterBase(body(0));
                if (start < kRegisterCount)
                {
                    const uint32_t count = std::min(bodyCount - 1u, kRegisterCount - start);
                    WriteRegisterRange(base, start, count, [&](uint32_t i) {
                        return body(i + 1u);
                    });
                }
            }
            break;

        case 0x55: // SET_CONSTANT2
        case 0x56: // SET_SHADER_CONSTANTS
            if (bodyCount >= 2)
            {
                const uint32_t start = body(0) & 0xFFFFu;
                if (start < kRegisterCount)
                {
                    const uint32_t count = std::min(bodyCount - 1u, kRegisterCount - start);
                    WriteRegisterRange(base, start, count, [&](uint32_t i) {
                        return body(i + 1u);
                    });
                }
            }
            break;

        case 0x2F: // LOAD_ALU_CONSTANT
            if (bodyCount >= 3)
            {
                const uint32_t start = ConstantRegisterBase(body(1));
                const uint32_t source = PhysicalToCached(body(0) & 0x3FFFFFFCu);
                const uint32_t count = body(2) & 0xFFFu;
                if (start < kRegisterCount &&
                    uint64_t(source & 0x1FFFFFFFu) + uint64_t(count) * 4u <= 0x20000000ull)
                {
                    const uint32_t clipped = std::min(count, kRegisterCount - start);
                    WriteRegisterRange(base, start, clipped, [&](uint32_t i) {
                        return GuestLoad32(base, source + i * 4u);
                    });
                }
            }
            break;

        case 0x27: // IM_LOAD: address|stage, start/size
            if (bodyCount >= 2 && (body(0) & 3u) < 2u)
            {
                const uint32_t type = body(0) & 3u;
                const uint32_t va = PhysicalToCached(body(0) & ~3u);
                const uint32_t size = body(1) & 0xFFFFu;
                if (size && size <= 0x10000u &&
                    uint64_t(va & 0x1FFFFFFFu) + uint64_t(size) * 4u <= 0x20000000ull)
                {
                    // Guest shader memory is already contiguous in the mapped
                    // address space. Bind directly so cache hits do not pay a
                    // full microcode memcpy before they can be recognized.
                    BindShader(type, va, base + va, size);
                }
            }
            break;

        case 0x2B: // IM_LOAD_IMMEDIATE
            if (bodyCount >= 2 && (body(0) & 3u) < 2u)
            {
                const uint32_t type = body(0) & 3u;
                const uint32_t size = body(1) & 0xFFFFu;
                if (size && size <= 0x10000u && size + 2u <= bodyCount)
                {
                    g_shaderStaging.resize(size_t(size) * 4u);
                    for (uint32_t i = 0; i < size; ++i)
                    {
                        const uint32_t word = body(2u + i);
                        uint8_t* dst = g_shaderStaging.data() + size_t(i) * 4u;
                        dst[0] = uint8_t(word >> 24);
                        dst[1] = uint8_t(word >> 16);
                        dst[2] = uint8_t(word >> 8);
                        dst[3] = uint8_t(word);
                    }
                    BindShader(type, 0, g_shaderStaging.data(), size);
                }
            }
            break;

        case 0x3D:
            if (bodyCount >= 2)
                for (uint32_t i = 1; i < bodyCount; ++i)
                    StoreGpu(base, body(0), body(i), i - 1u);
            break;

        case 0x3E:
            if (bodyCount >= 2)
            {
                const uint32_t reg = body(0) & 0x7FFFu;
                StoreGpu(base, body(1), reg < kRegisterCount ? g_registers[reg] : 0);
            }
            break;

        case 0x3C:
            if (bodyCount >= 4)
            {
                ++g_waits;
                const uint32_t info = body(0);
                const uint32_t poll = body(1);
                const uint32_t ref = body(2);
                const uint32_t mask = body(3);
                uint32_t value = 0;
                if (info & 0x10u)
                    value = LoadGpu(base, poll);
                else
                {
                    const uint32_t reg = poll & 0x7FFFu;
                    if (reg == kCoherStatusHost && reg < kRegisterCount)
                        WriteRegister(base, reg, g_registers[reg] & ~0x80000000u);
                    value = reg < kRegisterCount ? g_registers[reg] : 0;
                }

                if (!EvalWaitCondition(info, value, mask, ref))
                {
                    const uint64_t n = ++g_waitStalls;
                    if (n <= 8 && MojoRecompVerboseDiagnosticsEnabled())
                    {
                        KLOG_DIAG("PM4 WAIT_REG_MEM stall #%llu depth=%d %s=%08X "
                             "value=%08X mask=%08X ref=%08X func=%u irq=%llu/%llu\n",
                             static_cast<unsigned long long>(n), depth,
                             (info & 0x10u) ? "mem" : "reg", poll, value, mask, ref,
                             info & 7u,
                             static_cast<unsigned long long>(
                                 g_rendererInterruptDelivered.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(
                                 g_rendererInterruptRequested.load(std::memory_order_relaxed)));
                    }
                    NoteStall(fetch, position, depth);
                    return 0;
                }
            }
            break;

        case 0x26: // WAIT_FOR_IDLE
        {
            const uint64_t n = ++g_waitForIdle;
            if (n <= 32)
                KLOG_DIAG("PM4 WAIT_FOR_IDLE #%llu frame=%llu draw=%llu depth=%d body=%u\n",
                     static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(g_frames),
                     static_cast<unsigned long long>(g_draws),
                     depth, bodyCount);
            break;
        }

        case 0x45:
            if (bodyCount >= 6)
            {
                const uint32_t info = body(0);
                const uint32_t poll = body(1);
                const uint32_t ref = body(2);
                const uint32_t mask = body(3);
                const uint32_t value = (info & 0x10u)
                                           ? LoadGpu(base, poll)
                                           : ((poll & 0x7FFFu) < kRegisterCount
                                                  ? g_registers[poll & 0x7FFFu]
                                                  : 0);
                if (EvalWaitCondition(info, value, mask, ref))
                {
                    if (info & 0x100u)
                        StoreGpu(base, body(4), body(5));
                    else
                        WriteRegister(base, body(4) & 0x7FFFu, body(5));
                }
            }
            break;

        case 0x46:
        case 0x59:
        case 0x5B:
            if (bodyCount >= 3)
                StoreGpu(base, body(1), body(2));
            break;

        case 0x58: // EVENT_WRITE_SHD
            if (bodyCount >= 3)
            {
                const uint32_t initiator = body(0);
                WriteRegister(base, xenos::kVgtEventInitiator, initiator & 0x3Fu);
                const uint32_t value = (initiator & 0x80000000u)
                                           ? g_counter.load(std::memory_order_relaxed)
                                           : body(2);
                StoreGpu(base, body(1), value);
            }
            break;

        case 0x5A: // EVENT_WRITE_EXT: six uint16 screen extents, no data payload.
            if (bodyCount >= 2)
            {
                WriteRegister(base, xenos::kVgtEventInitiator, body(0) & 0x3Fu);
                // Until post-VS extent collection is implemented, conservatively
                // cover the Xenos coordinate range (XY in eight-pixel units).
                // The CPU uses this writeback to build next-frame tile masks.
                // Leaving stale/empty bounds drops valid draws on later tiles.
                // This matches Xenia's conservative EVENT_WRITE_EXT policy; it
                // does not override draw predication, scissor, or window offset.
                constexpr uint32_t xyExtent = (8192u / 8u) << 16;
                StoreGpu(base, body(1), xyExtent, 0);
                StoreGpu(base, body(1), xyExtent, 1);
                StoreGpu(base, body(1), 1u << 16, 2);
            }
            break;

        case 0x54:
        {
            const uint64_t n = ++g_interrupts;
            if (n <= 64)
                KLOG_DIAG("PM4 INTERRUPT #%llu frame=%llu draw=%llu depth=%d\n",
                     static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(g_frames),
                     static_cast<unsigned long long>(g_draws),
                     depth);
            if (g_interruptCommandSink)
                g_interruptCommandSink();
            else if (g_interruptSink)
                g_interruptSink();
            break;
        }

        case 0x64: // XE_SWAP
        {
            ++g_frames;
            const bool binkFrame = g_binkDrawSeenThisFrame;
            g_binkDrawSeenThisFrame = false;
            const bool previousBinkFrame =
                g_binkVideoCadenceActive.exchange(binkFrame, std::memory_order_acq_rel);
            if (previousBinkFrame != binkFrame)
                KLOG("PM4 Bink native cadence %s at frame=%llu\n",
                     binkFrame ? "active" : "released",
                     static_cast<unsigned long long>(g_frames));
            if (bodyCount >= 4 && body(0) == 0x53574150u && g_swapSink)
                g_swapSink(base, body(1), body(2), body(3));
            g_counter.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        case 0x37:
        case 0x3F:
            if (bodyCount >= 2 && depth < 8)
            {
                const uint32_t phys = body(0);
                const uint32_t size = body(1) & 0xFFFFFu;
                if (size && (phys & 0x1FFFFFFFu) + uint64_t(size) * 4u <= 0x20000000ull)
                {
                    ++g_indirectBuffers;
                    if (IbProfileEnabled())
                    {
                        static std::atomic<uint32_t> reports{0};
                        const uint32_t n = reports.fetch_add(1, std::memory_order_relaxed);
                        if (n < 128)
                            KLOG("PM4 IB profile n=%u frame=%llu depth=%d phys=%08X size=%u mask=%016llX select=%016llX\n",
                                 n + 1,
                                 static_cast<unsigned long long>(g_frames + 1), depth,
                                 phys, size,
                                 static_cast<unsigned long long>(g_binMask),
                                 static_cast<unsigned long long>(g_binSelect));
                    }

                    const bool timeIb = Pm4TimingProfileEnabled();
                    const auto ibStart = timeIb
                        ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{};
                    const uint64_t ibDrawSinkStart = timeIb ? g_drawSinkNs : 0;

                    ExecuteLinear(base, PhysicalToCached(phys), size, depth + 1);

                    if (timeIb)
                    {
                        const uint64_t elapsedNs = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - ibStart).count());
                        const uint64_t drawSinkNs = g_drawSinkNs - ibDrawSinkStart;
                        const uint64_t otherNs = elapsedNs > drawSinkNs ? elapsedNs - drawSinkNs : 0;
                        if (elapsedNs >= 100000u)
                        {
                            static std::atomic<uint32_t> slowIbReports{0};
                            const uint32_t n = slowIbReports.fetch_add(1, std::memory_order_relaxed);
                            if (n < 240)
                                KLOG("PM4 slow IB n=%u frame=%llu depth=%d phys=%08X size=%u cpu=%.3fus sink=%.3fus other=%.3fus stall=%u\n",
                                     n + 1,
                                     static_cast<unsigned long long>(g_frames + 1), depth,
                                     phys, size, double(elapsedNs) / 1000.0,
                                     double(drawSinkNs) / 1000.0,
                                     double(otherNs) / 1000.0,
                                     g_stallHit ? 1u : 0u);
                        }
                    }
                    if (g_stallHit)
                    {
                        NoteStall(fetch, position, depth);
                        return 0;
                    }
                }
            }
            break;

        case 0x60:
            g_binMask = (g_binMask & 0xFFFFFFFF00000000ull) | body(0);
            break;
        case 0x61:
            g_binMask = (g_binMask & 0x00000000FFFFFFFFull) | (uint64_t(body(0)) << 32);
            break;
        case 0x62:
            g_binSelect = (g_binSelect & 0xFFFFFFFF00000000ull) | body(0);
            break;
        case 0x63:
            g_binSelect = (g_binSelect & 0x00000000FFFFFFFFull) | (uint64_t(body(0)) << 32);
            break;

        case 0x50: // SET_BIN_MASK - high dword followed by low dword.
            if (bodyCount >= 2)
            {
                g_binMask = (uint64_t(body(0)) << 32) | body(1);
                static std::atomic<uint32_t> reports{0};
                const uint32_t n = reports.fetch_add(1, std::memory_order_relaxed);
                if (n < 16)
                    KLOG_DIAG("PM4 SET_BIN_MASK=%016llX\n",
                         static_cast<unsigned long long>(g_binMask));
            }
            break;
        case 0x51: // SET_BIN_SELECT - high dword followed by low dword.
            if (bodyCount >= 2)
            {
                g_binSelect = (uint64_t(body(0)) << 32) | body(1);
                static std::atomic<uint32_t> reports{0};
                const uint32_t n = reports.fetch_add(1, std::memory_order_relaxed);
                if (n < 16)
                    KLOG_DIAG("PM4 SET_BIN_SELECT=%016llX\n",
                         static_cast<unsigned long long>(g_binSelect));
            }
            break;

        case 0x34: // DRAW_INDX_BIN - diagnostic until exact execution semantics land.
        case 0x35: // DRAW_INDX_2_BIN
        case 0x4B: // SET_BIN_BASE_OFFSET
        {
            static std::array<std::atomic<uint32_t>, 128> reports{};
            const uint32_t n = reports[opcode].fetch_add(1, std::memory_order_relaxed);
            if (n < 8 && MojoRecompVerboseDiagnosticsEnabled())
            {
                KLOG("PM4 tiled packet op=%02X count=%u mask=%016llX select=%016llX body=",
                     opcode, bodyCount,
                     static_cast<unsigned long long>(g_binMask),
                     static_cast<unsigned long long>(g_binSelect));
                for (uint32_t i = 0; i < std::min<uint32_t>(bodyCount, 8u); ++i)
                    std::fprintf(stderr, "%s%08X", i ? "," : "", body(i));
                std::fprintf(stderr, "\n");
            }
            break;
        }

        default:
            if (!OpcodeName(opcode))
            {
                static std::array<std::atomic<bool>, 128> reported{};
                bool expected = false;
                if (reported[opcode].compare_exchange_strong(expected, true))
                    KLOG("PM4 unknown opcode %02X header=%08X body=%u depth=%d\n",
                         opcode, header, bodyCount, depth);
            }
            break;
    }

    ++g_packets;
    return bodyCount + 1u;
}

uint32_t ExecuteLinear(uint8_t* base, uint32_t va, uint32_t sizeDwords, int depth)
{
    Source source{base, va, 0};
    uint32_t position = 0;

    if (g_resumePlan.pending && depth > 0 && depth < 9 &&
        g_resumePlan.va[depth] == va && g_resumePlan.pos[depth] < sizeDwords)
    {
        position = g_resumePlan.pos[depth];
        g_resumePlan.va[depth] = 0;
    }

    while (position < sizeDwords)
    {
        const uint32_t consumed =
            ExecutePacket(base, source, position, sizeDwords - position, depth);
        if (!consumed)
            return position;
        position += consumed;
    }

    return position;
}

void PublishCursor(uint8_t* base, uint32_t cursor)
{
    const uint32_t slot = g_rptrSlot.load(std::memory_order_acquire);
    if (slot)
        GuestStore32(base, slot, cursor);
}

} // namespace

void Pm4_SetRingBuffer(uint32_t guestBase, uint32_t sizeBytes)
{
    g_ringBase.store(0, std::memory_order_release);
    g_ringDwords.store(sizeBytes / 4u, std::memory_order_relaxed);
    g_cursor.store(0, std::memory_order_relaxed);
    g_resumePlan = {};
    g_nextPlan = {};
    g_stallHit = false;
    g_binMask = ~0ull;
    g_binSelect = ~0ull;
    g_registers.fill(0);
    g_boundShaders[0] = {};
    g_boundShaders[1] = {};
    g_announcedShaders[0].clear();
    g_announcedShaders[1].clear();
    g_shaderBindCache[0].clear();
    g_shaderBindCache[1].clear();
    g_binkDrawSeenThisFrame = false;
    g_binkVideoCadenceActive.store(false, std::memory_order_release);
    g_ringBase.store(guestBase, std::memory_order_release);
    KLOG("PM4 ring configured at %08X (%u dwords)\n", guestBase, sizeBytes / 4u);
}

bool Pm4_RingInitialized()
{
    return g_ringBase.load(std::memory_order_acquire) != 0 &&
           g_ringDwords.load(std::memory_order_relaxed) != 0;
}

void Pm4_SetReadPointerSlot(uint32_t guestAddress)
{
    g_rptrSlot.store(guestAddress, std::memory_order_release);
}

void Pm4_SetReadPointerUpdateFrequency(uint32_t dwords)
{
    g_rptrUpdateFreq.store(std::max(1u, dwords), std::memory_order_release);
}

void Pm4_SetInterruptSink(void (*sink)())
{
    g_interruptSink = sink;
}

void Pm4_SetInterruptCommandSink(void (*sink)())
{
    g_interruptCommandSink = sink;
}

void Pm4_SetInterruptWakeSink(void (*sink)())
{
    g_interruptWakeSink = sink;
}

void Pm4_NotifyWorkAvailable()
{
    if (g_interruptWakeSink)
        g_interruptWakeSink();
}

void Pm4_RendererInterruptHandshake()
{
    const uint64_t ticket =
        g_rendererInterruptRequested.fetch_add(1, std::memory_order_seq_cst) + 1;
    g_rendererInterruptPending.store(true, std::memory_order_release);
    Pm4_NotifyWorkAvailable();
    uint32_t spins = 0;
    while (g_rendererInterruptDelivered.load(std::memory_order_acquire) < ticket)
    {
        if (++spins >= 20000)
            std::this_thread::yield();
    }
}

void Pm4_ServiceRendererInterrupts()
{
    ServiceRendererInterruptRequests();
}

void Pm4_SetShaderSink(void (*sink)(uint32_t, uint64_t, const uint8_t*, uint32_t))
{
    g_shaderSink = sink;
}

void Pm4_SetRegisterSink(void (*sink)(uint32_t, uint32_t))
{
    g_registerSink = sink;
}

void Pm4_SetRegisterBatchSink(void (*sink)(const uint32_t*, const uint32_t*, std::size_t))
{
    g_registerBatchSink = sink;
}

void Pm4_SetStoreSink(void (*sink)(uint8_t*, uint32_t, uint32_t))
{
    g_storeSink = sink;
}

void Pm4_SetDrawSink(void (*sink)(uint8_t*, const Pm4Draw&))
{
    g_drawSink = sink;
}

void Pm4_SetSwapSink(void (*sink)(uint8_t*, uint32_t, uint32_t, uint32_t))
{
    g_swapSink = sink;
}

bool Pm4_RegisterBinkPixelShaderContainer(const uint8_t* container,
                                          std::size_t sizeBytes)
{
    // ShaderContainer is big-endian. Its Shader struct contains the physical
    // microcode offset and size relative to the physical segment that begins at
    // virtualSize. This is the same payload PM4 hashes on IM_LOAD.
    constexpr uint32_t kPixelShaderContainerFamily = 0x102A1100u;
    constexpr std::size_t kHeaderBytes = 0x24u;
    if (!container || sizeBytes < kHeaderBytes)
        return false;

    const uint32_t flags = ReadBe32(container + 0x00u);
    const uint32_t virtualSize = ReadBe32(container + 0x04u);
    const uint32_t physicalSize = ReadBe32(container + 0x08u);
    const uint32_t shaderOffset = ReadBe32(container + 0x18u);
    if ((flags & 0xFFFFFF00u) != kPixelShaderContainerFamily ||
        virtualSize > sizeBytes ||
        physicalSize > sizeBytes - virtualSize ||
        shaderOffset > virtualSize || virtualSize - shaderOffset < 8u)
        return false;

    const uint32_t physicalOffset = ReadBe32(container + shaderOffset + 0u);
    const uint32_t shaderSize = ReadBe32(container + shaderOffset + 4u);
    if (!shaderSize || (shaderSize & 3u) != 0u ||
        physicalOffset > physicalSize || shaderSize > physicalSize - physicalOffset)
        return false;

    const std::size_t ucodeOffset = std::size_t(virtualSize) + physicalOffset;
    if (ucodeOffset > sizeBytes || shaderSize > sizeBytes - ucodeOffset)
        return false;

    const uint8_t* ucode = container + ucodeOffset;
    const uint32_t sizeDwords = shaderSize / 4u;
    if (!LooksLikeUcode(ucode, sizeDwords))
        return false;

    const uint64_t hash = Fnv1a(ucode, shaderSize);
    g_binkPixelShaderHash.store(hash, std::memory_order_release);
    KLOG("PM4 Bink shader registered from container: hash=%016llX size=%u dwords\n",
         static_cast<unsigned long long>(hash), sizeDwords);
    return true;
}

bool Pm4_BinkVideoCadenceActive()
{
    return g_binkVideoCadenceActive.load(std::memory_order_acquire);
}

const Pm4ShaderBinding& Pm4_BoundShader(uint32_t stage)
{
    static const Pm4ShaderBinding empty{};
    return stage < 2 ? g_boundShaders[stage] : empty;
}

const uint32_t* Pm4_Registers()
{
    return g_registers.data();
}

uint32_t Pm4_Execute(uint8_t* base, uint32_t writePtr)
{
    const bool runtimeTiming = Pm4RuntimeTimingEnabled();
    const auto executeStart = runtimeTiming
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    const uint64_t drawSinkStart = runtimeTiming ? g_drawSinkNs : 0;
    struct ExecuteTimingScope
    {
        bool enabled = false;
        std::chrono::steady_clock::time_point start;
        uint64_t drawSinkStart = 0;
        ~ExecuteTimingScope()
        {
            if (!enabled)
                return;
            const uint64_t ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count());
            g_executeNs.fetch_add(ns, std::memory_order_relaxed);
            const uint64_t drawSinkDelta = g_drawSinkNs >= drawSinkStart
                ? g_drawSinkNs - drawSinkStart
                : 0;
            g_executeDrawSinkNs.fetch_add(drawSinkDelta, std::memory_order_relaxed);
            g_executeCalls.fetch_add(1, std::memory_order_relaxed);
        }
    } timingScope{runtimeTiming, executeStart, drawSinkStart};

    ServiceRendererInterruptRequests();

    const uint32_t ringBase = g_ringBase.load(std::memory_order_acquire);
    const uint32_t ringDwords = g_ringDwords.load(std::memory_order_relaxed);
    if (!ringBase || !ringDwords)
        return writePtr;

    uint32_t cursor = g_cursor.load(std::memory_order_relaxed);
    const bool powerOfTwoRing = (ringDwords & (ringDwords - 1u)) == 0;
    const uint32_t target = powerOfTwoRing
        ? (writePtr & (ringDwords - 1u))
        : (writePtr % ringDwords);
    if (cursor == target)
        return cursor;

    Source source{base, ringBase, ringDwords};
    g_stallHit = false;
    g_nextPlan = {};
    const uint32_t rptrUpdateFreq =
        std::max(1u, g_rptrUpdateFreq.load(std::memory_order_acquire));
    uint32_t dwordsSinceWriteback = 0;

    uint32_t guard = ringDwords + 1u;
    while (cursor != target && guard--)
    {
        const uint32_t available =
            target >= cursor ? target - cursor : target + ringDwords - cursor;
        const uint32_t consumed = ExecutePacket(base, source, cursor, available, 0);
        if (!consumed)
            break;

        cursor += consumed;
        if (cursor >= ringDwords)
            cursor -= ringDwords;
        dwordsSinceWriteback += consumed;
        if (dwordsSinceWriteback >= rptrUpdateFreq)
        {
            PublishCursor(base, cursor);
            dwordsSinceWriteback = 0;
        }
    }

    if (g_stallHit)
        g_resumePlan = g_nextPlan;
    else
        g_resumePlan = {};

    g_cursor.store(cursor, std::memory_order_release);
    return cursor;
}

void Pm4_LogPacketProfile()
{
    if (!Pm4PacketProfileEnabled())
        return;

    static std::array<uint64_t, 128> prevOpcodePackets{};
    static std::array<uint64_t, 128> prevOpcodeDwords{};
    static std::array<uint64_t, 3> prevTypePackets{};
    static std::array<uint64_t, 3> prevTypeDwords{};

    KLOG("PM4 packet profile types:");
    for (uint32_t type = 0; type < 3; ++type)
    {
        const uint64_t packets = g_profileTypePackets[type] - prevTypePackets[type];
        const uint64_t dwords = g_profileTypeDwords[type] - prevTypeDwords[type];
        std::fprintf(stderr, " T%u=%llu/%lluDW", type,
                     static_cast<unsigned long long>(packets),
                     static_cast<unsigned long long>(dwords));
        prevTypePackets[type] = g_profileTypePackets[type];
        prevTypeDwords[type] = g_profileTypeDwords[type];
    }
    std::fprintf(stderr, "\n");

    struct Delta
    {
        uint32_t opcode = 0;
        uint64_t packets = 0;
        uint64_t dwords = 0;
    };
    std::array<Delta, 128> deltas{};
    for (uint32_t opcode = 0; opcode < 128; ++opcode)
    {
        deltas[opcode] = {
            opcode,
            g_profileOpcodePackets[opcode] - prevOpcodePackets[opcode],
            g_profileOpcodeDwords[opcode] - prevOpcodeDwords[opcode],
        };
        prevOpcodePackets[opcode] = g_profileOpcodePackets[opcode];
        prevOpcodeDwords[opcode] = g_profileOpcodeDwords[opcode];
    }
    std::sort(deltas.begin(), deltas.end(), [](const Delta& a, const Delta& b) {
        return a.dwords > b.dwords;
    });

    KLOG("PM4 packet profile top:");
    uint32_t shown = 0;
    for (const auto& delta : deltas)
    {
        if (!delta.dwords || shown == 8)
            break;
        const char* name = OpcodeName(delta.opcode);
        std::fprintf(stderr, " %s%02X=%llu/%lluDW",
                     name ? name : "OP", delta.opcode,
                     static_cast<unsigned long long>(delta.packets),
                     static_cast<unsigned long long>(delta.dwords));
        ++shown;
    }
    std::fprintf(stderr, "\n");
}

void Pm4_LogTimingProfile()
{
    if (!Pm4TimingProfileEnabled())
        return;

    static std::array<uint64_t, kTimingBucketCount> prevSamples{};
    static std::array<uint64_t, kTimingBucketCount> prevNs{};
    struct Entry
    {
        uint32_t bucket = 0;
        uint64_t samples = 0;
        uint64_t ns = 0;
    };
    std::vector<Entry> entries;
    entries.reserve(kTimingBucketCount);
    uint64_t totalSamples = 0;
    uint64_t totalNs = 0;
    for (uint32_t bucket = 0; bucket < kTimingBucketCount; ++bucket)
    {
        const uint64_t samples = g_timingSamples[bucket] - prevSamples[bucket];
        const uint64_t ns = g_timingNs[bucket] - prevNs[bucket];
        prevSamples[bucket] = g_timingSamples[bucket];
        prevNs[bucket] = g_timingNs[bucket];
        if (!samples)
            continue;
        entries.push_back({bucket, samples, ns});
        totalSamples += samples;
        totalNs += ns;
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.ns > b.ns;
    });

    KLOG("PM4 time profile: sampled=%llu sampleCpu=%.3fms top=",
         static_cast<unsigned long long>(totalSamples), double(totalNs) / 1.0e6);
    const size_t count = std::min<size_t>(8, entries.size());
    for (size_t i = 0; i < count; ++i)
    {
        const Entry& entry = entries[i];
        const double avgUs = entry.samples ? double(entry.ns) / double(entry.samples) / 1000.0 : 0.0;
        if (entry.bucket < 128)
            KLOG(" %s(0x%02X):%llux/%.2fus",
                 TimingBucketName(entry.bucket), entry.bucket,
                 static_cast<unsigned long long>(entry.samples), avgUs);
        else
            KLOG(" %s:%llux/%.2fus",
                 TimingBucketName(entry.bucket),
                 static_cast<unsigned long long>(entry.samples), avgUs);
    }
    KLOG("\n");
}

uint32_t Pm4_Cursor() { return g_cursor.load(std::memory_order_relaxed); }
void Pm4_IncrementCounter() { g_counter.fetch_add(1, std::memory_order_relaxed); }
uint32_t Pm4_Counter() { return g_counter.load(std::memory_order_relaxed); }
uint64_t Pm4_PacketCount() { return g_packets; }
uint64_t Pm4_IndirectBufferCount() { return g_indirectBuffers; }
uint64_t Pm4_InterruptCount() { return g_interrupts; }
uint64_t Pm4_GpuStoreCount() { return g_gpuStores; }
uint64_t Pm4_WaitCount() { return g_waits; }
uint64_t Pm4_WaitStallCount() { return g_waitStalls; }
uint64_t Pm4_DrawCount() { return g_draws; }
uint64_t Pm4_DrawSinkCpuNs() { return g_drawSinkNs; }
uint64_t Pm4_ExecuteCpuNs() { return g_executeNs.load(std::memory_order_relaxed); }
uint64_t Pm4_ExecuteDrawSinkCpuNs() { return g_executeDrawSinkNs.load(std::memory_order_relaxed); }
uint64_t Pm4_ExecuteCallCount() { return g_executeCalls.load(std::memory_order_relaxed); }
uint64_t Pm4_FrameCount() { return g_frames; }
uint64_t Pm4_ShaderBindCount() { return g_shaderBinds; }
uint64_t Pm4_ShaderCacheHitCount() { return g_shaderCacheHits; }
