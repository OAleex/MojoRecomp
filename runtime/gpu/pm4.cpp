#include "pm4.h"

#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
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
uint64_t g_collapsedSceneFrame = 0;
uint64_t g_frames = 0;

struct SceneIbReuseEntry
{
    uint32_t phys = 0;
    uint32_t size = 0;
};

std::vector<SceneIbReuseEntry> g_sceneIbReuse;
uint64_t g_sceneIbReuseFrame = 0;
uint64_t g_sceneIbSkips = 0;
uint64_t g_sceneIbSkippedDwords = 0;
uint64_t g_sceneRenderPacketSkips = 0;
uint64_t g_sceneRenderSkippedDwords = 0;
uint64_t g_sceneType0ReplaySkips = 0;
uint64_t g_sceneType0ReplaySkippedDwords = 0;
uint64_t g_sceneTailRedundantType0Skips = 0;
uint64_t g_sceneTailRedundantType0SkippedDwords = 0;
bool g_sceneIbCandidateActive = false;
bool g_sceneIbCandidateUnsafe = false;
uint32_t g_sceneIbCandidateReasons = 0;

constexpr uint32_t kSceneIbReasonStore = 1u << 0;
constexpr uint32_t kSceneIbReasonTileState = 1u << 1;
constexpr uint32_t kSceneIbReasonPredicatedState = 1u << 2;
constexpr uint32_t kSceneIbReasonWait = 1u << 3;
constexpr uint32_t kSceneIbReasonEvent = 1u << 4;
constexpr uint32_t kSceneIbReasonInterrupt = 1u << 5;
constexpr uint32_t kSceneIbReasonSwap = 1u << 6;

struct SceneType0ReplayKey
{
    uint32_t sourceVa = 0;
    uint32_t position = 0;
    uint32_t reg = 0;
    uint32_t bodyCount = 0;
    bool oneRegister = false;

    bool operator==(const SceneType0ReplayKey& other) const
    {
        return sourceVa == other.sourceVa &&
               position == other.position &&
               reg == other.reg &&
               bodyCount == other.bodyCount &&
               oneRegister == other.oneRegister;
    }
};

struct SceneType0ReplayKeyHash
{
    size_t operator()(const SceneType0ReplayKey& key) const noexcept
    {
        // Fast avalanche-style combine. These values are command-stream
        // addresses / packet descriptors, so avoid a linear container here:
        // dense scene passes can submit tens of thousands of matching packets.
        uint64_t h = (uint64_t(key.sourceVa) << 32) | key.position;
        h ^= (uint64_t(key.reg) << 17) ^ uint64_t(key.bodyCount);
        h ^= key.oneRegister ? 0x9E3779B97F4A7C15ull : 0ull;
        h ^= h >> 30;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 27;
        h *= 0x94D049BB133111EBull;
        h ^= h >> 31;
        return static_cast<size_t>(h);
    }
};

std::unordered_set<SceneType0ReplayKey, SceneType0ReplayKeyHash> g_sceneType0ReplayKeys;
uint64_t g_sceneType0ReplayFrame = 0;

struct SceneReplaySkeleton
{
    uint32_t sizeDwords = 0;
    uint32_t firstWord = 0;
    uint32_t middleWord = 0;
    uint32_t lastWord = 0;
    std::vector<uint32_t> essentialPositions;
};

std::unordered_map<uint64_t, SceneReplaySkeleton> g_sceneReplaySkeletons;
uint64_t g_sceneReplaySkeletonFrame = 0;

bool CollapseTiledDrawsEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_COLLAPSE_TILED_DRAWS");
        // Keep the old collapsed-tile path available only as an explicit
        // diagnostic. Xenos / ReXGlue execute each predicated Type-3 packet
        // according to BIN_SELECT & BIN_MASK; collapsing three EDRAM replays
        // into the first tile changes that command-stream semantics.
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

bool CollapseSceneExtentsEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_COLLAPSE_SCENE_EXTENTS");
        return value && value[0] != '0';
    }();
    return enabled;
}

bool CollapseDrawSignatureDiagnosticsEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_COLLAPSE_DRAW_SIGNATURES");
        return value && value[0] != '0';
    }();
    return enabled;
}

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

bool Type0ProfileEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_TYPE0_PROFILE");
        return value && value[0] != '0';
    }();
    return enabled;
}

bool Type0FastPathEnabled()
{
    // Keep this optimization opt-in until the title has had a full visual
    // regression pass. The generic WriteRegister path is already correct and
    // was the known-good baseline before the remaining frame-time work.
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_TYPE0_FAST");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

bool SceneTailRedundantType0SkipEnabled()
{
    // Diagnostic A/B only. Restrict the optimization to the collapsed tile-2
    // tail until the title has had a full visual regression pass.
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_SCENE_TAIL_REDUNDANT_TYPE0_SKIP");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

bool IsSceneTileWindow(uint32_t& tileIndex)
{
    const uint32_t tl = g_registers[xenos::kPaScWindowScissorTl];
    const uint32_t br = g_registers[xenos::kPaScWindowScissorBr];
    const uint32_t rawOffset = g_registers[xenos::kPaScWindowOffset];
    const uint32_t ox = rawOffset & 0x7FFFu;
    const uint32_t oy = (rawOffset >> 16) & 0x7FFFu;
    if (oy != 0)
        return false;

    // Crash's 1280x720 predicated EDRAM pass uses three 448-pixel logical
    // windows with a 32-pixel overlap. The second and third windows are replayed
    // with -416 / -832 window offsets onto the same physical raster tile.
    if (tl == 0x00000000u && br == 0x02D001C0u && ox == 0x0000u)
    {
        tileIndex = 0;
        return true;
    }
    if (tl == 0x000001A0u && br == 0x02D00360u && ox == 0x7E60u)
    {
        tileIndex = 1;
        return true;
    }
    if (tl == 0x00000340u && br == 0x02D00500u && ox == 0x7CC0u)
    {
        tileIndex = 2;
        return true;
    }
    return false;
}

bool SkipCollapsedSceneDraw(const Pm4Draw& draw)
{
    if (!CollapseTiledDrawsEnabled() || !draw.predicated)
        return false;

    const uint32_t mode = g_registers[xenos::kRbModeControl] & 7u;
    if (mode != 4 && mode != 5)
        return false;

    const uint64_t frame = g_frames + 1;
    uint32_t tileIndex = 0;
    const bool sceneWindow = IsSceneTileWindow(tileIndex);
    if (draw.indexed && sceneWindow)
        g_collapsedSceneFrame = frame;

    if (sceneWindow && g_collapsedSceneFrame == frame && tileIndex != 0)
        return true;

    // A six-rectangle transition group follows each tile replay. Once a real
    // indexed scene tile has been observed in this frame, the 0x0c / 0x30
    // transition groups belong to the redundant second and third host replays.
    const uint64_t tileSelect = draw.binSelect & 0x3Full;
    return g_collapsedSceneFrame == frame &&
           draw.primType == xenos::kRectangleList &&
           (tileSelect == 0x0Cull || tileSelect == 0x30ull);
}

bool IsSceneRasterOnlyOpcode(uint32_t opcode)
{
    switch (opcode)
    {
        case 0x22: // DRAW_INDX
        case 0x34: // DRAW_INDX_BIN
        case 0x35: // DRAW_INDX_2_BIN
        case 0x36: // DRAW_INDX_2
        case 0x27: // IM_LOAD
        case 0x2B: // IM_LOAD_IMMEDIATE
        case 0x2D: // SET_CONSTANT
        case 0x2E: // LOAD_CONSTANT_CONTEXT
        case 0x2F: // LOAD_ALU_CONSTANT
        case 0x3B: // INVALIDATE_STATE
        case 0x4A: // SET_SHADER_BASES
        case 0x55: // SET_CONSTANT2
        case 0x56: // SET_SHADER_CONSTANTS
            return true;
        default:
            return false;
    }
}

bool SkipCollapsedSceneRenderPacket(uint32_t opcode, uint32_t header)
{
    (void)header;
    if (!CollapseTiledDrawsEnabled())
        return false;

    const uint64_t frame = g_frames + 1;
    if (g_collapsedSceneFrame != frame)
        return false;

    const uint32_t mode = g_registers[xenos::kRbModeControl] & 7u;
    if (mode != 4 && mode != 5)
        return false;

    uint32_t tileIndex = UINT32_MAX;
    if (!IsSceneTileWindow(tileIndex) || tileIndex == 0)
        return false;

    // These packets only prepare/issue raster work for the selected EDRAM bin.
    // Some of Crash's shader/state packets are not themselves predicated even
    // though they live inside the same tile replay. The first tile has already
    // been expanded to the full logical 1280x720 surface by the host renderer,
    // so all raster setup/draw packets for tiles 2/3 are redundant. Keep all
    // synchronization, events, writes, bin controls and resolves alive.
    // State/signature diagnostics must replay the *entire* second/third-tile PM4
    // state stream, not only the DRAW packets. Otherwise constants/fetch state
    // remain frozen at the end of tile 0 and any cross-tile comparison is false.
    // Actual raster work is still stopped by SkipCollapsedSceneDraw before the
    // renderer, so this has no visual/GPU cost beyond parser/state decoding.
    if (CollapseDrawSignatureDiagnosticsEnabled())
        return false;
    if (opcode == 0x5A && CollapseSceneExtentsEnabled())
        return true;
    return IsSceneRasterOnlyOpcode(opcode);
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

void ResetSceneIbReuse(uint64_t frame)
{
    if (g_sceneIbReuseFrame == frame)
        return;
    g_sceneIbReuseFrame = frame;
    g_sceneIbReuse.clear();
}

bool HasReusableSceneIb(uint32_t phys, uint32_t size)
{
    return std::any_of(g_sceneIbReuse.begin(), g_sceneIbReuse.end(),
                       [&](const SceneIbReuseEntry& entry) {
                           return entry.phys == phys && entry.size == size;
                       });
}

void RememberReusableSceneIb(uint32_t phys, uint32_t size)
{
    if (!HasReusableSceneIb(phys, size))
        g_sceneIbReuse.push_back({phys, size});
}

void ResetSceneType0Replay(uint64_t frame)
{
    if (g_sceneType0ReplayFrame == frame)
        return;
    g_sceneType0ReplayFrame = frame;
    g_sceneType0ReplayKeys.clear();
}

bool IsShaderConstantType0Range(uint32_t reg, uint32_t bodyCount, bool oneRegister)
{
    if (reg < xenos::kAluConstantBase || reg >= 0x5000u || !bodyCount)
        return false;
    if (oneRegister)
        return true;
    const uint64_t end = uint64_t(reg) + bodyCount - 1u;
    return end < 0x5000u;
}

bool HasSceneType0ReplayKey(uint32_t sourceVa, uint32_t position, uint32_t reg,
                            uint32_t bodyCount, bool oneRegister)
{
    return g_sceneType0ReplayKeys.find(
               {sourceVa, position, reg, bodyCount, oneRegister}) !=
           g_sceneType0ReplayKeys.end();
}

void RememberSceneType0ReplayKey(uint32_t sourceVa, uint32_t position, uint32_t reg,
                                 uint32_t bodyCount, bool oneRegister)
{
    g_sceneType0ReplayKeys.insert({sourceVa, position, reg, bodyCount, oneRegister});
}

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
void (*g_shaderSink)(uint32_t, uint64_t, const uint8_t*, uint32_t) = nullptr;
void (*g_drawSink)(uint8_t*, const Pm4Draw&) = nullptr;
void (*g_swapSink)(uint8_t*, uint32_t, uint32_t, uint32_t) = nullptr;

Pm4ShaderBinding g_boundShaders[2]{};
std::vector<uint8_t> g_shaderStaging;
std::vector<uint64_t> g_announcedShaders[2];

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
    if (g_sceneIbCandidateActive)
    {
        g_sceneIbCandidateUnsafe = true;
        g_sceneIbCandidateReasons |= kSceneIbReasonStore;
    }
    const uint32_t va = PhysicalToCached(physicalAddress & ~3u);
    if (va < kPhysicalBase || uint64_t(va) + 4 > kPhysicalEnd)
        return false;
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

uint64_t ShaderFingerprint(const uint8_t* code, uint32_t sizeDwords)
{
    // A fixed-cost content stamp lets repeated IM_LOAD packets reuse the fully
    // validated shader hash without rescanning/copying the complete microcode.
    // Sample evenly across the blob rather than only its prefix/suffix so a
    // same-address rewrite is overwhelmingly likely to invalidate the cache.
    if (!code || !sizeDwords)
        return 0;

    uint64_t hash = 0xCBF29CE484222325ull;
    auto mix32 = [&](uint32_t value) {
        for (uint32_t shift = 0; shift < 32; shift += 8)
        {
            hash ^= uint8_t(value >> shift);
            hash *= 0x100000001B3ull;
        }
    };

    mix32(sizeDwords);
    constexpr uint32_t kSamples = 24;
    const uint32_t last = sizeDwords - 1u;
    for (uint32_t i = 0; i < kSamples; ++i)
    {
        const uint32_t at = kSamples == 1 ? 0u :
            uint32_t((uint64_t(last) * i) / (kSamples - 1u));
        uint32_t value = 0;
        std::memcpy(&value, code + size_t(at) * 4u, sizeof(value));
        mix32(value);
    }
    return hash;
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
    if (g_sceneIbCandidateActive &&
        (index == xenos::kPaScWindowScissorTl ||
         index == xenos::kPaScWindowScissorBr ||
         index == xenos::kPaScWindowOffset))
    {
        g_sceneIbCandidateUnsafe = true;
        g_sceneIbCandidateReasons |= kSceneIbReasonTileState;
    }
    g_registers[index] = value;

    if (index >= kScratch0 && index <= kScratch7)
    {
        const uint32_t scratch = index - kScratch0;
        if (g_registers[kScratchUmsk] & (1u << scratch))
            StoreGpuRaw(base, g_registers[kScratchAddr] + scratch * 4u, value);
    }
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
        if (wrapDwords)
            position %= wrapDwords;
        return GuestLoad32(base, va + position * 4u);
    }
};

bool IsRedundantSceneTailType0(const Source& source, uint32_t position,
                               uint32_t available, uint32_t& consumed)
{
    consumed = 0;
    if (!SceneTailRedundantType0SkipEnabled() || !available)
        return false;

    const uint32_t header = source(position);
    if ((header >> 30) != 0)
        return false;

    const uint32_t bodyCount = ((header >> 16) & 0x3FFFu) + 1u;
    consumed = bodyCount + 1u;
    if (available < consumed)
        return false;

    const uint32_t reg = header & 0x7FFFu;
    const bool oneRegister = ((header >> 15) & 1u) != 0;
    if (reg >= kRegisterCount)
        return false;

    auto hasScratchSideEffect = [](uint32_t index) {
        return index >= kScratch0 && index <= kScratch7;
    };

    if (oneRegister)
    {
        if (hasScratchSideEffect(reg))
            return false;
        // Intermediate writes to an ordinary register are not observable until
        // the next packet. Only the final value needs to match current state.
        return g_registers[reg] == source(position + bodyCount);
    }

    if (uint64_t(reg) + bodyCount > kRegisterCount)
        return false;
    for (uint32_t i = 0; i < bodyCount; ++i)
    {
        const uint32_t index = reg + i;
        if (hasScratchSideEffect(index) ||
            g_registers[index] != source(position + 1u + i))
            return false;
    }
    return true;
}

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

void ResetSceneReplaySkeletons(uint64_t frame)
{
    if (g_sceneReplaySkeletonFrame == frame)
        return;
    g_sceneReplaySkeletonFrame = frame;
    g_sceneReplaySkeletons.clear();
}

uint64_t SceneReplaySkeletonKey(uint32_t va, uint32_t sizeDwords)
{
    return (uint64_t(va) << 32) | sizeDwords;
}

bool ClassifySceneReplayPacket(const Source& source, uint32_t position,
                               uint32_t available, uint32_t& consumed,
                               bool& essential)
{
    if (!available)
        return false;

    const uint32_t header = source(position);
    const uint32_t type = header >> 30;
    if (type == 2)
    {
        consumed = 1;
        essential = false;
        return true;
    }
    if (type == 1)
    {
        if (available < 3)
            return false;
        consumed = 3;
        essential = true;
        return true;
    }

    const uint32_t bodyCount = ((header >> 16) & 0x3FFFu) + 1u;
    consumed = bodyCount + 1u;
    if (available < consumed)
        return false;

    if (type == 0)
    {
        const uint32_t reg = header & 0x7FFFu;
        const bool oneRegister = ((header >> 15) & 1u) != 0;
        essential = !IsShaderConstantType0Range(reg, bodyCount, oneRegister);
        return true;
    }

    const uint32_t opcode = (header >> 8) & 0x7Fu;
    const bool collapsedExtent = opcode == 0x5A && CollapseSceneExtentsEnabled();
    essential = opcode != 0x10 && !IsSceneRasterOnlyOpcode(opcode) && !collapsedExtent;
    return true;
}

const SceneReplaySkeleton* BuildSceneReplaySkeleton(const Source& source,
                                                    uint32_t sizeDwords)
{
    const uint64_t key = SceneReplaySkeletonKey(source.va, sizeDwords);
    auto existing = g_sceneReplaySkeletons.find(key);
    if (existing != g_sceneReplaySkeletons.end())
        return &existing->second;

    SceneReplaySkeleton skeleton{};
    skeleton.sizeDwords = sizeDwords;
    if (sizeDwords)
    {
        skeleton.firstWord = source(0);
        skeleton.middleWord = source(sizeDwords / 2u);
        skeleton.lastWord = source(sizeDwords - 1u);
    }

    uint32_t position = 0;
    while (position < sizeDwords)
    {
        uint32_t consumed = 0;
        bool essential = true;
        if (!ClassifySceneReplayPacket(source, position, sizeDwords - position,
                                       consumed, essential) || !consumed)
            return nullptr;
        if (essential)
            skeleton.essentialPositions.push_back(position);
        position += consumed;
    }

    auto [it, inserted] = g_sceneReplaySkeletons.emplace(key, std::move(skeleton));
    (void)inserted;
    return &it->second;
}

void LogSceneTailIbClassification(const Source& source, uint32_t sizeDwords,
                                  uint32_t tileIndex)
{
    if (!Pm4TimingProfileEnabled() || tileIndex != 2 ||
        sizeDwords < 8000u || sizeDwords > 16000u)
        return;

    static std::atomic<uint32_t> reports{0};
    const uint32_t report = reports.fetch_add(1, std::memory_order_relaxed);
    if (report >= 3)
        return;

    std::array<uint32_t, 128> totalOpcodes{};
    std::array<uint32_t, 128> essentialOpcodes{};
    uint32_t totalPackets = 0;
    uint32_t essentialPackets = 0;
    uint32_t type0Packets = 0;
    uint32_t type1Packets = 0;
    uint32_t type2Packets = 0;
    struct Type0Count
    {
        uint32_t reg = 0;
        uint32_t bodyCount = 0;
        bool oneRegister = false;
        uint32_t packets = 0;
        uint64_t writes = 0;
    };
    std::vector<Type0Count> type0Entries;
    std::array<uint32_t, 128> type0RangePackets{};
    std::array<uint64_t, 128> type0RangeWrites{};
    uint32_t position = 0;
    while (position < sizeDwords)
    {
        uint32_t consumed = 0;
        bool essential = true;
        if (!ClassifySceneReplayPacket(source, position, sizeDwords - position,
                                       consumed, essential) || !consumed)
            break;

        const uint32_t header = source(position);
        const uint32_t type = header >> 30;
        ++totalPackets;
        if (essential)
            ++essentialPackets;
        if (type == 0)
        {
            ++type0Packets;
            if (essential)
            {
                const uint32_t bodyCount = ((header >> 16) & 0x3FFFu) + 1u;
                const uint32_t reg = header & 0x7FFFu;
                const bool oneRegister = ((header >> 15) & 1u) != 0;
                auto it = std::find_if(type0Entries.begin(), type0Entries.end(),
                    [&](const Type0Count& entry) {
                        return entry.reg == reg && entry.bodyCount == bodyCount &&
                               entry.oneRegister == oneRegister;
                    });
                if (it == type0Entries.end())
                {
                    type0Entries.push_back({reg, bodyCount, oneRegister, 1u, bodyCount});
                }
                else
                {
                    ++it->packets;
                    it->writes += bodyCount;
                }
                const uint32_t range = std::min<uint32_t>(reg >> 8, 127u);
                ++type0RangePackets[range];
                type0RangeWrites[range] += bodyCount;
            }
        }
        else if (type == 1)
            ++type1Packets;
        else if (type == 2)
            ++type2Packets;
        else
        {
            const uint32_t opcode = (header >> 8) & 0x7Fu;
            ++totalOpcodes[opcode];
            if (essential)
                ++essentialOpcodes[opcode];
        }
        position += consumed;
    }

    struct OpcodeCount
    {
        uint32_t opcode = 0;
        uint32_t essential = 0;
        uint32_t total = 0;
    };
    std::vector<OpcodeCount> entries;
    for (uint32_t opcode = 0; opcode < 128; ++opcode)
        if (totalOpcodes[opcode])
            entries.push_back({opcode, essentialOpcodes[opcode], totalOpcodes[opcode]});
    std::sort(entries.begin(), entries.end(), [](const OpcodeCount& a, const OpcodeCount& b) {
        if (a.essential != b.essential)
            return a.essential > b.essential;
        return a.total > b.total;
    });

    KLOG("PM4 tail IB classify n=%u va=%08X size=%u packets=%u essential=%u type0=%u type1=%u type2=%u top=",
         report + 1, source.va, sizeDwords, totalPackets, essentialPackets,
         type0Packets, type1Packets, type2Packets);
    const size_t count = std::min<size_t>(10, entries.size());
    for (size_t i = 0; i < count; ++i)
    {
        const auto& entry = entries[i];
        const char* name = OpcodeName(entry.opcode);
        KLOG(" %s%02X=%u/%u",
             name ? name : "OP", entry.opcode, entry.essential, entry.total);
    }
    KLOG("\n");

    std::array<uint32_t, 128> rangeOrder{};
    for (uint32_t i = 0; i < rangeOrder.size(); ++i)
        rangeOrder[i] = i;
    std::sort(rangeOrder.begin(), rangeOrder.end(), [&](uint32_t a, uint32_t b) {
        return type0RangeWrites[a] > type0RangeWrites[b];
    });
    KLOG("PM4 tail Type0 ranges n=%u top=", report + 1);
    uint32_t shownRanges = 0;
    for (uint32_t range : rangeOrder)
    {
        if (!type0RangePackets[range] || shownRanges == 10)
            break;
        KLOG(" %04X-%04X:%u pkt/%llu writes", range << 8,
             (range << 8) | 0xFFu, type0RangePackets[range],
             static_cast<unsigned long long>(type0RangeWrites[range]));
        ++shownRanges;
    }
    KLOG("\n");

    std::sort(type0Entries.begin(), type0Entries.end(),
              [](const Type0Count& a, const Type0Count& b) {
                  if (a.writes != b.writes)
                      return a.writes > b.writes;
                  return a.packets > b.packets;
              });
    KLOG("PM4 tail Type0 exact n=%u top=", report + 1);
    const size_t type0ReportCount = std::min<size_t>(16, type0Entries.size());
    for (size_t i = 0; i < type0ReportCount; ++i)
    {
        const auto& entry = type0Entries[i];
        KLOG(" reg=%04X count=%u one=%u:%u pkt/%llu writes",
             entry.reg, entry.bodyCount, entry.oneRegister ? 1u : 0u,
             entry.packets, static_cast<unsigned long long>(entry.writes));
    }
    KLOG("\n");
    if (SceneTailRedundantType0SkipEnabled())
        KLOG("PM4 tail redundant Type0 skip: packets=%llu dwords=%llu\n",
             static_cast<unsigned long long>(g_sceneTailRedundantType0Skips),
             static_cast<unsigned long long>(g_sceneTailRedundantType0SkippedDwords));
}

const SceneReplaySkeleton* FindSceneReplaySkeleton(const Source& source,
                                                   uint32_t sizeDwords)
{
    const auto it = g_sceneReplaySkeletons.find(
        SceneReplaySkeletonKey(source.va, sizeDwords));
    if (it == g_sceneReplaySkeletons.end())
        return nullptr;
    const SceneReplaySkeleton& skeleton = it->second;
    if (skeleton.sizeDwords != sizeDwords)
        return nullptr;
    if (sizeDwords &&
        (source(0) != skeleton.firstWord ||
         source(sizeDwords / 2u) != skeleton.middleWord ||
         source(sizeDwords - 1u) != skeleton.lastWord))
        return nullptr;
    return &skeleton;
}

uint32_t ExecutePacket(uint8_t* base, const Source& fetch, uint32_t position,
                       uint32_t available, int depth)
{
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
        if (CollapseTiledDrawsEnabled() &&
            IsShaderConstantType0Range(reg, bodyCount, oneRegister))
        {
            const uint64_t frame = g_frames + 1;
            const uint32_t mode = g_registers[xenos::kRbModeControl] & 7u;
            uint32_t tileIndex = UINT32_MAX;
            if ((mode == 4 || mode == 5) && IsSceneTileWindow(tileIndex))
            {
                ResetSceneType0Replay(frame);
                if (tileIndex == 0)
                {
                    RememberSceneType0ReplayKey(fetch.va, position, reg, bodyCount, oneRegister);
                }
                else if (g_collapsedSceneFrame == frame &&
                         HasSceneType0ReplayKey(fetch.va, position, reg, bodyCount, oneRegister))
                {
                    // The exact same constant packet already ran from the same
                    // command-list address on tile 0. Reapplying it cannot
                    // change register state, and the host already rendered the
                    // full logical surface from that first tile state.
                    ++g_sceneType0ReplaySkips;
                    g_sceneType0ReplaySkippedDwords += bodyCount + 1u;
                    ++g_packets;
                    return bodyCount + 1u;
                }
            }
        }

        // Shader constant Type-0 packets are pure register-file updates. They
        // never overlap the scratch registers or the scene tile/window state
        // that WriteRegister observes for side effects, so avoid the generic
        // per-dword callback path here. Large scene IBs contain many of these
        // packets and walking them through WriteRegister was measurable CPU
        // overhead even after the duplicated raster work had been collapsed.
        //
        // The guest command stream stores big-endian dwords, so keep the exact
        // GuestLoad32 byte swap. Ring-buffer sources can wrap, therefore only
        // use the contiguous pointer fast path for ordinary (non-wrapping) IBs.
        if (Type0FastPathEnabled() &&
            IsShaderConstantType0Range(reg, bodyCount, oneRegister))
        {
            if (oneRegister)
            {
                // Nothing can observe intermediate writes inside one Type-0
                // packet, so only its final value is architecturally visible.
                g_registers[reg] = fetch(position + bodyCount);
            }
            else if (!fetch.wrapDwords)
            {
                const auto* source = reinterpret_cast<const volatile uint32_t*>(
                    fetch.base + fetch.va + uint64_t(position + 1u) * 4u);
                uint32_t i = 0;
                for (; i + 4u <= bodyCount; i += 4u)
                {
                    g_registers[reg + i + 0u] = __builtin_bswap32(source[i + 0u]);
                    g_registers[reg + i + 1u] = __builtin_bswap32(source[i + 1u]);
                    g_registers[reg + i + 2u] = __builtin_bswap32(source[i + 2u]);
                    g_registers[reg + i + 3u] = __builtin_bswap32(source[i + 3u]);
                }
                for (; i < bodyCount; ++i)
                    g_registers[reg + i] = __builtin_bswap32(source[i]);
            }
            else
            {
                for (uint32_t i = 0; i < bodyCount; ++i)
                    g_registers[reg + i] = fetch(position + 1u + i);
            }
            ++g_packets;
            return bodyCount + 1u;
        }
        if (Type0ProfileEnabled() && bodyCount >= 16)
        {
            const uint64_t frame = g_frames + 1;
            uint32_t tileIndex = UINT32_MAX;
            if (g_collapsedSceneFrame == frame && IsSceneTileWindow(tileIndex) && tileIndex != 0)
            {
                static std::atomic<uint32_t> reports{0};
                const uint32_t n = reports.fetch_add(1, std::memory_order_relaxed);
                if (n < 600)
                    KLOG("PM4 type0 profile n=%u frame=%llu tile=%u reg=%04X count=%u one=%u end=%04X\n",
                         n + 1,
                         static_cast<unsigned long long>(frame), tileIndex,
                         reg, bodyCount, oneRegister ? 1u : 0u,
                         oneRegister ? reg : reg + bodyCount - 1u);
            }
        }
        for (uint32_t i = 0; i < bodyCount; ++i)
            WriteRegister(base, oneRegister ? reg : reg + i, fetch(position + 1u + i));
        ++g_packets;
        return bodyCount + 1u;
    }

    const uint32_t opcode = (header >> 8) & 0x7Fu;
    ProfilePacketOpcode(opcode, bodyCount + 1u);
    if (SkipCollapsedSceneRenderPacket(opcode, header))
    {
        if (opcode == 0x22 || opcode == 0x34 || opcode == 0x35 || opcode == 0x36)
            ++g_draws;
        ++g_sceneRenderPacketSkips;
        g_sceneRenderSkippedDwords += bodyCount + 1u;
        ++g_packets;
        return bodyCount + 1u;
    }
    if (g_sceneIbCandidateActive)
    {
        // Replaying an IB is only removable when tile selection can change
        // nothing except raster draws/state that is identical across tiles.
        // A predicated non-draw packet can take a different path under another
        // bin select, so keep that IB conservative. Guest-visible writes,
        // waits, queries, interrupts and swaps are also observable side effects.
        if ((header & 1u) && opcode != 0x22 && opcode != 0x36 &&
            opcode != 0x34 && opcode != 0x35)
        {
            g_sceneIbCandidateUnsafe = true;
            g_sceneIbCandidateReasons |= kSceneIbReasonPredicatedState;
        }
        switch (opcode)
        {
            case 0x23: // VIZ_QUERY
            case 0x26: // WAIT_FOR_IDLE
            case 0x3C: // WAIT_REG_MEM
            case 0x52: // WAIT_REG_EQ
            case 0x53: // WAIT_REG_GTE
            case 0x5C: // WAIT_UNTIL_READ
            case 0x5D: // WAIT_IB_PFD_COMPLETE
                g_sceneIbCandidateUnsafe = true;
                g_sceneIbCandidateReasons |= kSceneIbReasonWait;
                break;
            case 0x3D: // MEM_WRITE
            case 0x3E: // REG_TO_MEM
            case 0x44: // COND_EXEC
            case 0x45: // COND_WRITE
            case 0x4F: // MEM_WRITE_CNTR
                g_sceneIbCandidateUnsafe = true;
                g_sceneIbCandidateReasons |= kSceneIbReasonStore;
                break;
            case 0x46: // EVENT_WRITE
            case 0x58: // EVENT_WRITE_SHD
            case 0x59: // EVENT_WRITE_CFL
            case 0x5A: // EVENT_WRITE_EXT
            case 0x5B: // EVENT_WRITE_ZPD
                g_sceneIbCandidateUnsafe = true;
                g_sceneIbCandidateReasons |= kSceneIbReasonEvent;
                break;
            case 0x54: // INTERRUPT
                g_sceneIbCandidateUnsafe = true;
                g_sceneIbCandidateReasons |= kSceneIbReasonInterrupt;
                break;
            case 0x64: // XE_SWAP
                g_sceneIbCandidateUnsafe = true;
                g_sceneIbCandidateReasons |= kSceneIbReasonSwap;
                break;
            default:
                break;
        }
    }
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
            const uint32_t packetPosition = fetch.wrapDwords
                                                ? (position % fetch.wrapDwords)
                                                : position;
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

            if (CollapseDrawSignatureDiagnosticsEnabled())
            {
                static uint64_t signatureFrame = 0;
                static std::unordered_set<uint64_t> tile0Signatures;
                static uint32_t tileDraws[3]{};
                static uint32_t duplicateDraws[3]{};
                static uint32_t uniqueDraws[3]{};

                const uint64_t frame = g_frames + 1;
                if (signatureFrame != frame)
                {
                    if (signatureFrame != 0 && (tileDraws[0] || tileDraws[1] || tileDraws[2]))
                    {
                        KLOG("[collapse draw signatures] frame=%llu tile0=%u tile1=%u tile2=%u "
                             "dup1=%u unique1=%u dup2=%u unique2=%u\n",
                             static_cast<unsigned long long>(signatureFrame),
                             tileDraws[0], tileDraws[1], tileDraws[2],
                             duplicateDraws[1], uniqueDraws[1],
                             duplicateDraws[2], uniqueDraws[2]);
                    }
                    signatureFrame = frame;
                    tile0Signatures.clear();
                    std::fill(std::begin(tileDraws), std::end(tileDraws), 0u);
                    std::fill(std::begin(duplicateDraws), std::end(duplicateDraws), 0u);
                    std::fill(std::begin(uniqueDraws), std::end(uniqueDraws), 0u);
                }

                uint32_t signatureTile = UINT32_MAX;
                if (IsSceneTileWindow(signatureTile) && signatureTile < 3)
                {
                    struct DrawStateProbe
                    {
                        uint64_t signature = 0;
                        uint64_t vs = 0;
                        uint64_t ps = 0;
                        uint64_t normalizedState = 0;
                        uint32_t matchedTiles = 0;
                        uint32_t comparedTiles = 0;
                        std::vector<uint32_t> sampledState;
                    };
                    static uint64_t stateProbeFrame = 0;
                    static std::vector<DrawStateProbe> stateProbes;
                    static uint32_t exactStateMatches[3]{};
                    static uint32_t stateMismatches[3]{};
                    static uint32_t sampledComparisons[3]{};
                    static std::array<uint32_t, kRegisterCount> diffRegCounts1{};
                    static std::array<uint32_t, kRegisterCount> diffRegCounts2{};

                    uint64_t signature = 1469598103934665603ull;
                    auto mix = [&](uint32_t word) {
                        signature ^= uint64_t(word);
                        signature *= 1099511628211ull;
                    };
                    mix(opcode);
                    mix(bodyCount);
                    for (uint32_t i = 0; i < bodyCount; ++i)
                        mix(body(i));

                    if (stateProbeFrame != frame)
                    {
                        if (stateProbeFrame != 0 && !stateProbes.empty())
                        {
                            KLOG("[collapse normalized state] frame=%llu tile0=%u "
                                 "exact1=%u mismatch1=%u exact2=%u mismatch2=%u\n",
                                 static_cast<unsigned long long>(stateProbeFrame),
                                 static_cast<unsigned>(stateProbes.size()),
                                 exactStateMatches[1], stateMismatches[1],
                                 exactStateMatches[2], stateMismatches[2]);

                            auto logTopDiffs = [&](uint32_t tile,
                                                   const std::array<uint32_t, kRegisterCount>& counts) {
                                std::vector<std::pair<uint32_t, uint32_t>> ranked;
                                auto collect = [&](uint32_t begin, uint32_t end) {
                                    for (uint32_t reg = begin; reg < end; ++reg)
                                    {
                                        if (reg == xenos::kPaScWindowOffset ||
                                            reg == xenos::kPaScWindowScissorTl ||
                                            reg == xenos::kPaScWindowScissorBr || !counts[reg])
                                            continue;
                                        ranked.push_back({counts[reg], reg});
                                    }
                                };
                                collect(0x2000u, 0x2320u);
                                collect(xenos::kAluConstantBase,
                                        xenos::kLoopConstantBase + 32u);
                                std::sort(ranked.begin(), ranked.end(),
                                          [](const auto& a, const auto& b) {
                                              if (a.first != b.first)
                                                  return a.first > b.first;
                                              return a.second < b.second;
                                          });
                                KLOG("[collapse state diffs] frame=%llu tile=%u sampled=%u regs=%u top=",
                                     static_cast<unsigned long long>(stateProbeFrame), tile,
                                     sampledComparisons[tile],
                                     static_cast<unsigned>(ranked.size()));
                                const size_t count = std::min<size_t>(16, ranked.size());
                                for (size_t i = 0; i < count; ++i)
                                    KLOG(" %04X:%u", ranked[i].second, ranked[i].first);
                                KLOG("\n");
                            };
                            logTopDiffs(1, diffRegCounts1);
                            logTopDiffs(2, diffRegCounts2);
                        }
                        stateProbeFrame = frame;
                        stateProbes.clear();
                        std::fill(std::begin(exactStateMatches), std::end(exactStateMatches), 0u);
                        std::fill(std::begin(stateMismatches), std::end(stateMismatches), 0u);
                        std::fill(std::begin(sampledComparisons), std::end(sampledComparisons), 0u);
                        diffRegCounts1.fill(0);
                        diffRegCounts2.fill(0);
                    }

                    // Hash the renderer-visible state while normalizing only the
                    // three tile-window registers. Matching is occurrence-based:
                    // each tile-0 entry may be consumed once by tile 1 and once by
                    // tile 2, so repeated packets with different constants cannot
                    // accidentally compare against the wrong object.
                    uint64_t normalizedState = 1469598103934665603ull;
                    auto mixState = [&](uint32_t reg, uint32_t value) {
                        normalizedState ^= uint64_t(reg);
                        normalizedState *= 1099511628211ull;
                        normalizedState ^= uint64_t(value);
                        normalizedState *= 1099511628211ull;
                    };
                    for (uint32_t reg = 0x2000u; reg < 0x2320u; ++reg)
                    {
                        if (reg == xenos::kPaScWindowOffset ||
                            reg == xenos::kPaScWindowScissorTl ||
                            reg == xenos::kPaScWindowScissorBr)
                            continue;
                        mixState(reg, g_registers[reg]);
                    }
                    for (uint32_t reg = xenos::kAluConstantBase;
                         reg < xenos::kLoopConstantBase + 32u; ++reg)
                        mixState(reg, g_registers[reg]);

                    if (draw.indexed && signatureTile == 0)
                    {
                        DrawStateProbe probe{signature,
                                             g_boundShaders[0].hash,
                                             g_boundShaders[1].hash,
                                             normalizedState,
                                             0u,
                                             0u,
                                             {}};
                        // Keep a bounded full-state sample so the diagnostic can
                        // identify the actual registers behind a hash mismatch.
                        if (stateProbes.size() >= 96u && stateProbes.size() < 224u)
                        {
                            probe.sampledState.reserve(
                                (0x2320u - 0x2000u) +
                                (xenos::kLoopConstantBase + 32u - xenos::kAluConstantBase));
                            for (uint32_t reg = 0x2000u; reg < 0x2320u; ++reg)
                                probe.sampledState.push_back(g_registers[reg]);
                            for (uint32_t reg = xenos::kAluConstantBase;
                                 reg < xenos::kLoopConstantBase + 32u; ++reg)
                                probe.sampledState.push_back(g_registers[reg]);
                        }
                        stateProbes.push_back(std::move(probe));
                    }
                    else if (draw.indexed && signatureTile != 0)
                    {
                        const uint32_t tileBit = 1u << signatureTile;
                        auto probeIt = std::find_if(
                            stateProbes.begin(), stateProbes.end(),
                            [&](const DrawStateProbe& probe) {
                                return (probe.comparedTiles & tileBit) == 0 &&
                                       probe.signature == signature &&
                                       probe.vs == g_boundShaders[0].hash &&
                                       probe.ps == g_boundShaders[1].hash;
                            });
                        if (probeIt != stateProbes.end())
                        {
                            probeIt->comparedTiles |= tileBit;
                            if (probeIt->normalizedState == normalizedState)
                            {
                                probeIt->matchedTiles |= tileBit;
                                ++exactStateMatches[signatureTile];
                            }
                            else
                            {
                                ++stateMismatches[signatureTile];
                            }

                            if (!probeIt->sampledState.empty())
                            {
                                auto& counts = signatureTile == 1 ? diffRegCounts1 : diffRegCounts2;
                                size_t stateIndex = 0;
                                for (uint32_t reg = 0x2000u; reg < 0x2320u; ++reg, ++stateIndex)
                                {
                                    if (reg == xenos::kPaScWindowOffset ||
                                        reg == xenos::kPaScWindowScissorTl ||
                                        reg == xenos::kPaScWindowScissorBr)
                                        continue;
                                    if (probeIt->sampledState[stateIndex] != g_registers[reg])
                                        ++counts[reg];
                                }
                                for (uint32_t reg = xenos::kAluConstantBase;
                                     reg < xenos::kLoopConstantBase + 32u; ++reg, ++stateIndex)
                                {
                                    if (probeIt->sampledState[stateIndex] != g_registers[reg])
                                        ++counts[reg];
                                }
                                ++sampledComparisons[signatureTile];
                            }
                        }
                        else
                        {
                            ++stateMismatches[signatureTile];
                        }
                    }

                    ++tileDraws[signatureTile];
                    if (signatureTile == 0)
                    {
                        tile0Signatures.insert(signature);
                    }
                    else if (tile0Signatures.find(signature) != tile0Signatures.end())
                    {
                        ++duplicateDraws[signatureTile];
                    }
                    else
                    {
                        ++uniqueDraws[signatureTile];
                        static uint32_t uniqueReports = 0;
                        if (uniqueReports++ < 64)
                            KLOG("[collapse draw unique] frame=%llu tile=%u sig=%016llX "
                                 "indexed=%u count=%u indexVa=%08X mask=%016llX select=%016llX\n",
                                 static_cast<unsigned long long>(frame), signatureTile,
                                 static_cast<unsigned long long>(signature),
                                 draw.indexed ? 1u : 0u, draw.indexCount, draw.indexVa,
                                 static_cast<unsigned long long>(draw.binMask),
                                 static_cast<unsigned long long>(draw.binSelect));
                    }
                }
            }

            // The Vulkan host path renders Crash's three predicated EDRAM
            // windows as one full-size pass. Drop the exact second/third scene
            // replays before entering renderer_probe/Vulkan so their atomics,
            // shader lookups and register decoding do not consume CPU time.
            // Resolve-mode draw packets are deliberately excluded above.
            if (SkipCollapsedSceneDraw(draw))
                break;

            if (g_boundShaders[0].hash == 0x6D73B356D2B5E61Bull)
            {
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

            const auto drawSinkStart = std::chrono::steady_clock::now();
            g_drawSink(base, draw);
            g_drawSinkNs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - drawSinkStart).count());
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
                    for (uint32_t i = 1; i < bodyCount && start + i - 1 < kRegisterCount; ++i)
                        WriteRegister(base, start + i - 1, body(i));
            }
            break;

        case 0x55: // SET_CONSTANT2
        case 0x56: // SET_SHADER_CONSTANTS
            if (bodyCount >= 2)
            {
                const uint32_t start = body(0) & 0xFFFFu;
                if (start < kRegisterCount)
                    for (uint32_t i = 1; i < bodyCount && start + i - 1 < kRegisterCount; ++i)
                        WriteRegister(base, start + i - 1, body(i));
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
                    for (uint32_t i = 0; i < clipped; ++i)
                        WriteRegister(base, start + i, GuestLoad32(base, source + i * 4u));
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
                        g_registers[reg] &= ~0x80000000u;
                    value = reg < kRegisterCount ? g_registers[reg] : 0;
                }

                if (!EvalWaitCondition(info, value, mask, ref))
                {
                    const uint64_t n = ++g_waitStalls;
                    if (n <= 8)
                        KLOG_DIAG("PM4 WAIT_REG_MEM stall #%llu depth=%d %s=%08X "
                             "value=%08X mask=%08X ref=%08X func=%u\n",
                             static_cast<unsigned long long>(n), depth,
                             (info & 0x10u) ? "mem" : "reg", poll, value, mask, ref,
                             info & 7u);
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
            if (g_interruptSink)
                g_interruptSink();
            break;
        }

        case 0x64: // XE_SWAP
            ++g_frames;
            if (bodyCount >= 4 && body(0) == 0x53574150u && g_swapSink)
                g_swapSink(base, body(1), body(2), body(3));
            g_counter.fetch_add(1, std::memory_order_relaxed);
            break;

        case 0x37:
        case 0x3F:
            if (bodyCount >= 2 && depth < 8)
            {
                const uint32_t phys = body(0);
                const uint32_t size = body(1) & 0xFFFFFu;
                if (size && (phys & 0x1FFFFFFFu) + uint64_t(size) * 4u <= 0x20000000ull)
                {
                    if (IbProfileEnabled())
                    {
                        static std::atomic<uint32_t> reports{0};
                        uint32_t tileIndex = UINT32_MAX;
                        const bool sceneWindow = IsSceneTileWindow(tileIndex);
                        const uint64_t frame = g_frames + 1;
                        if (sceneWindow && g_collapsedSceneFrame == frame)
                        {
                            const uint32_t n = reports.fetch_add(1, std::memory_order_relaxed);
                            if (n < 800)
                                KLOG("PM4 IB profile n=%u frame=%llu depth=%d tile=%u phys=%08X size=%u mask=%016llX select=%016llX\n",
                                     n + 1,
                                     static_cast<unsigned long long>(frame), depth, tileIndex,
                                     phys, size,
                                     static_cast<unsigned long long>(g_binMask),
                                     static_cast<unsigned long long>(g_binSelect));
                        }
                    }
                    const uint64_t frame = g_frames + 1;
                    uint32_t sceneTile = UINT32_MAX;
                    const bool sceneWindow = IsSceneTileWindow(sceneTile) &&
                                             g_collapsedSceneFrame == frame;
                    const bool topLevelSceneIb = CollapseTiledDrawsEnabled() &&
                                                 depth == 0 && sceneWindow;
                    if (topLevelSceneIb)
                        ResetSceneIbReuse(frame);

                    ++g_indirectBuffers;

                    // If this exact command list already ran on the first
                    // logical EDRAM tile and proved render-only, the host's
                    // collapsed full-width pass has already consumed its work.
                    // Skip the second/third decode entirely instead of walking
                    // thousands of identical PM4 packets just to discard draws.
                    if (topLevelSceneIb && sceneTile != 0 &&
                        HasReusableSceneIb(phys, size))
                    {
                        ++g_sceneIbSkips;
                        g_sceneIbSkippedDwords += size;
                        break;
                    }

                    const bool candidate = topLevelSceneIb && sceneTile == 0 &&
                                           !g_sceneIbCandidateActive;
                    if (candidate)
                    {
                        g_sceneIbCandidateActive = true;
                        g_sceneIbCandidateUnsafe = false;
                        g_sceneIbCandidateReasons = 0;
                    }

                    const bool timeSceneIb = Pm4TimingProfileEnabled() && sceneWindow;
                    const auto sceneIbStart = timeSceneIb
                        ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{};
                    const uint64_t sceneIbDrawSinkStart = timeSceneIb ? g_drawSinkNs : 0;

                    // Classify the raw top-level tile-2 tail before its command
                    // stream mutates bin/scissor state. Doing this here avoids
                    // losing the original tile identity inside ExecuteLinear.
                    if (topLevelSceneIb && sceneTile == 2 &&
                        size >= 8000u && size <= 16000u)
                    {
                        const Source tailSource{base, PhysicalToCached(phys), 0};
                        LogSceneTailIbClassification(tailSource, size, sceneTile);
                    }

                    ExecuteLinear(base, PhysicalToCached(phys), size, depth + 1);

                    if (timeSceneIb)
                    {
                        const uint64_t elapsedNs = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - sceneIbStart).count());
                        const uint64_t drawSinkNs = g_drawSinkNs - sceneIbDrawSinkStart;
                        const uint64_t otherNs = elapsedNs > drawSinkNs ? elapsedNs - drawSinkNs : 0;
                        if (elapsedNs >= 100000u)
                        {
                            static std::atomic<uint32_t> slowIbReports{0};
                            const uint32_t n = slowIbReports.fetch_add(1, std::memory_order_relaxed);
                            if (n < 240)
                                KLOG("PM4 slow IB n=%u frame=%llu depth=%d tile=%u phys=%08X size=%u cpu=%.3fus sink=%.3fus other=%.3fus stall=%u\n",
                                     n + 1,
                                     static_cast<unsigned long long>(frame), depth, sceneTile,
                                     phys, size, double(elapsedNs) / 1000.0,
                                     double(drawSinkNs) / 1000.0,
                                     double(otherNs) / 1000.0,
                                     g_stallHit ? 1u : 0u);
                        }
                    }

                    if (candidate)
                    {
                        const bool unsafe = g_sceneIbCandidateUnsafe || g_stallHit;
                        const uint32_t reasons = g_sceneIbCandidateReasons;
                        g_sceneIbCandidateActive = false;
                        g_sceneIbCandidateUnsafe = false;
                        g_sceneIbCandidateReasons = 0;
                        uint32_t afterTile = UINT32_MAX;
                        const bool stillFirstTile = IsSceneTileWindow(afterTile) &&
                            afterTile == 0 &&
                            g_collapsedSceneFrame == frame &&
                            g_frames + 1 == frame;
                        if (!unsafe && stillFirstTile)
                            RememberReusableSceneIb(phys, size);
                        else if (size >= 1000)
                        {
                            static std::atomic<uint32_t> rejectReports{0};
                            const uint32_t n = rejectReports.fetch_add(1, std::memory_order_relaxed);
                            if (n < 80)
                                KLOG_DIAG("PM4 scene IB not reusable n=%u frame=%llu phys=%08X size=%u reasons=%02X stall=%u stillTile0=%u\n",
                                          n + 1,
                                          static_cast<unsigned long long>(frame),
                                          phys, size, reasons, g_stallHit ? 1u : 0u,
                                          stillFirstTile ? 1u : 0u);
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
            if (g_sceneIbCandidateActive) { g_sceneIbCandidateUnsafe = true; g_sceneIbCandidateReasons |= kSceneIbReasonTileState; }
            g_binMask = (g_binMask & 0xFFFFFFFF00000000ull) | body(0);
            break;
        case 0x61:
            if (g_sceneIbCandidateActive) { g_sceneIbCandidateUnsafe = true; g_sceneIbCandidateReasons |= kSceneIbReasonTileState; }
            g_binMask = (g_binMask & 0x00000000FFFFFFFFull) | (uint64_t(body(0)) << 32);
            break;
        case 0x62:
            if (g_sceneIbCandidateActive) { g_sceneIbCandidateUnsafe = true; g_sceneIbCandidateReasons |= kSceneIbReasonTileState; }
            g_binSelect = (g_binSelect & 0xFFFFFFFF00000000ull) | body(0);
            break;
        case 0x63:
            if (g_sceneIbCandidateActive) { g_sceneIbCandidateUnsafe = true; g_sceneIbCandidateReasons |= kSceneIbReasonTileState; }
            g_binSelect = (g_binSelect & 0x00000000FFFFFFFFull) | (uint64_t(body(0)) << 32);
            break;

        case 0x50: // SET_BIN_MASK - high dword followed by low dword.
            if (bodyCount >= 2)
            {
                if (g_sceneIbCandidateActive) { g_sceneIbCandidateUnsafe = true; g_sceneIbCandidateReasons |= kSceneIbReasonTileState; }
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
                if (g_sceneIbCandidateActive) { g_sceneIbCandidateUnsafe = true; g_sceneIbCandidateReasons |= kSceneIbReasonTileState; }
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

    bool recordSceneSkeleton = false;
    SceneReplaySkeleton recordedSkeleton{};
    uint64_t sceneFrame = 0;
    if (CollapseTiledDrawsEnabled() && depth > 0 && position == 0)
    {
        sceneFrame = g_frames + 1;
        const uint32_t mode = g_registers[xenos::kRbModeControl] & 7u;
        uint32_t tileIndex = UINT32_MAX;
        if ((mode == 4 || mode == 5) && IsSceneTileWindow(tileIndex) && tileIndex == 0)
        {
            ResetSceneReplaySkeletons(sceneFrame);
            recordSceneSkeleton = true;
            recordedSkeleton.sizeDwords = sizeDwords;
            if (sizeDwords)
            {
                recordedSkeleton.firstWord = source(0);
                recordedSkeleton.middleWord = source(sizeDwords / 2u);
                recordedSkeleton.lastWord = source(sizeDwords - 1u);
            }
        }
    }

    if (CollapseTiledDrawsEnabled() && depth > 0 &&
        !CollapseDrawSignatureDiagnosticsEnabled())
    {
        const uint64_t frame = g_frames + 1;
        const uint32_t mode = g_registers[xenos::kRbModeControl] & 7u;
        uint32_t tileIndex = UINT32_MAX;
        if ((mode == 4 || mode == 5) && g_collapsedSceneFrame == frame &&
            IsSceneTileWindow(tileIndex) && tileIndex != 0)
        {
            ResetSceneReplaySkeletons(frame);
            const SceneReplaySkeleton* skeleton =
                FindSceneReplaySkeleton(source, sizeDwords);
            if (!skeleton)
                skeleton = BuildSceneReplaySkeleton(source, sizeDwords);

            LogSceneTailIbClassification(source, sizeDwords, tileIndex);

            if (skeleton)
            {
                if (Pm4TimingProfileEnabled() && tileIndex == 2 &&
                    sizeDwords >= 12000u && sizeDwords <= 14000u)
                {
                    static std::atomic<uint32_t> skeletonReports{0};
                    const uint32_t n = skeletonReports.fetch_add(1, std::memory_order_relaxed);
                    if (n < 6)
                        KLOG("PM4 tail IB skeleton n=%u va=%08X size=%u essentialPositions=%zu\n",
                             n + 1, source.va, sizeDwords, skeleton->essentialPositions.size());
                }
                auto it = std::lower_bound(skeleton->essentialPositions.begin(),
                                           skeleton->essentialPositions.end(), position);
                for (; it != skeleton->essentialPositions.end(); ++it)
                {
                    const uint32_t packetPosition = *it;
                    if (tileIndex == 2 && sizeDwords >= 8000u && sizeDwords <= 16000u)
                    {
                        uint32_t redundantConsumed = 0;
                        if (IsRedundantSceneTailType0(source, packetPosition,
                                                      sizeDwords - packetPosition,
                                                      redundantConsumed))
                        {
                            ProfilePacketType(0, redundantConsumed);
                            ++g_packets;
                            ++g_sceneTailRedundantType0Skips;
                            g_sceneTailRedundantType0SkippedDwords += redundantConsumed;
                            position = packetPosition + redundantConsumed;
                            continue;
                        }
                    }
                    const bool timeTailPacket = Pm4TimingProfileEnabled() &&
                        tileIndex == 2 && sizeDwords >= 8000u && sizeDwords <= 16000u;
                    const uint32_t packetHeader = timeTailPacket ? source(packetPosition) : 0u;
                    const auto tailPacketStart = timeTailPacket
                        ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{};
                    const uint32_t consumed = ExecutePacket(
                        base, source, packetPosition, sizeDwords - packetPosition, depth);
                    if (timeTailPacket)
                    {
                        const uint64_t elapsedNs = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - tailPacketStart).count());
                        if (elapsedNs >= 20000u)
                        {
                            static std::atomic<uint32_t> packetReports{0};
                            const uint32_t n = packetReports.fetch_add(1, std::memory_order_relaxed);
                            if (n < 96)
                            {
                                const uint32_t bucket = TimingBucketForHeader(packetHeader);
                                KLOG("PM4 tail packet n=%u va=%08X size=%u pos=%u bucket=%u name=%s cpu=%.3fus consumed=%u stall=%u\n",
                                     n + 1, source.va, sizeDwords, packetPosition, bucket,
                                     TimingBucketName(bucket), double(elapsedNs) / 1000.0,
                                     consumed, g_stallHit ? 1u : 0u);
                            }
                        }
                    }
                    if (!consumed)
                        return packetPosition;
                    position = packetPosition + consumed;

                    // An essential state packet may leave the collapsed tile
                    // window (for example a scissor/copy transition). From that
                    // exact point onward resume the normal parser so no work from
                    // the following pass is accidentally dropped.
                    uint32_t currentTile = UINT32_MAX;
                    const uint32_t currentMode =
                        g_registers[xenos::kRbModeControl] & 7u;
                    if ((currentMode != 4 && currentMode != 5) ||
                        g_collapsedSceneFrame != frame ||
                        !IsSceneTileWindow(currentTile) || currentTile == 0)
                    {
                        if (Pm4TimingProfileEnabled() && tileIndex == 2 &&
                            sizeDwords >= 12000u && sizeDwords <= 14000u)
                        {
                            static std::atomic<uint32_t> breakReports{0};
                            const uint32_t n = breakReports.fetch_add(1, std::memory_order_relaxed);
                            if (n < 12)
                                KLOG("PM4 tail IB skeleton break n=%u va=%08X size=%u packetPos=%u nextPos=%u mode=%u currentTile=%u\n",
                                     n + 1, source.va, sizeDwords, packetPosition, position,
                                     currentMode, currentTile);
                        }
                        break;
                    }
                }

                if (it == skeleton->essentialPositions.end())
                    return sizeDwords;
            }
        }
    }

    while (position < sizeDwords)
    {
        if (recordSceneSkeleton)
        {
            uint32_t classified = 0;
            bool essential = true;
            if (!ClassifySceneReplayPacket(source, position, sizeDwords - position,
                                           classified, essential) || !classified)
            {
                recordSceneSkeleton = false;
            }
            else if (essential)
            {
                recordedSkeleton.essentialPositions.push_back(position);
            }
        }

        const uint32_t consumed =
            ExecutePacket(base, source, position, sizeDwords - position, depth);
        if (!consumed)
            return position;
        position += consumed;
    }

    if (recordSceneSkeleton && sceneFrame == g_sceneReplaySkeletonFrame)
    {
        g_sceneReplaySkeletons[SceneReplaySkeletonKey(va, sizeDwords)] =
            std::move(recordedSkeleton);
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
    g_sceneIbReuse.clear();
    g_sceneIbReuseFrame = 0;
    g_sceneType0ReplayKeys.clear();
    g_sceneType0ReplayFrame = 0;
    g_sceneReplaySkeletons.clear();
    g_sceneReplaySkeletonFrame = 0;
    g_sceneIbCandidateActive = false;
    g_sceneIbCandidateUnsafe = false;
    g_sceneIbCandidateReasons = 0;
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

void Pm4_SetShaderSink(void (*sink)(uint32_t, uint64_t, const uint8_t*, uint32_t))
{
    g_shaderSink = sink;
}

void Pm4_SetDrawSink(void (*sink)(uint8_t*, const Pm4Draw&))
{
    g_drawSink = sink;
}

void Pm4_SetSwapSink(void (*sink)(uint8_t*, uint32_t, uint32_t, uint32_t))
{
    g_swapSink = sink;
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
    const uint32_t ringBase = g_ringBase.load(std::memory_order_acquire);
    const uint32_t ringDwords = g_ringDwords.load(std::memory_order_relaxed);
    if (!ringBase || !ringDwords)
        return writePtr;

    uint32_t cursor = g_cursor.load(std::memory_order_relaxed) % ringDwords;
    const uint32_t target = writePtr % ringDwords;
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
        const uint32_t available = (target + ringDwords - cursor) % ringDwords;
        const uint32_t consumed = ExecutePacket(base, source, cursor, available, 0);
        if (!consumed)
            break;

        cursor = (cursor + consumed) % ringDwords;
        g_cursor.store(cursor, std::memory_order_relaxed);
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
uint64_t Pm4_FrameCount() { return g_frames; }
uint64_t Pm4_ShaderBindCount() { return g_shaderBinds; }
uint64_t Pm4_ShaderCacheHitCount() { return g_shaderCacheHits; }
uint64_t Pm4_SceneIbSkipCount() { return g_sceneIbSkips; }
uint64_t Pm4_SceneIbSkippedDwords() { return g_sceneIbSkippedDwords; }
uint64_t Pm4_SceneRenderPacketSkipCount() { return g_sceneRenderPacketSkips; }
uint64_t Pm4_SceneRenderSkippedDwords() { return g_sceneRenderSkippedDwords; }
uint64_t Pm4_SceneType0ReplaySkipCount() { return g_sceneType0ReplaySkips; }
uint64_t Pm4_SceneType0ReplaySkippedDwords() { return g_sceneType0ReplaySkippedDwords; }
