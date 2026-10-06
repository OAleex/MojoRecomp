#pragma once

#include <cstddef>
#include <cstdint>

// Xenos command processor. It decodes PM4 in guest order, maintains register
// state and dispatches renderer work; rasterization remains behind the sinks.

void Pm4_SetRingBuffer(uint32_t guestBase, uint32_t sizeBytes);
bool Pm4_RingInitialized();

// Guest virtual address registered through VdEnableRingBufferRPtrWriteBack.
void Pm4_SetReadPointerSlot(uint32_t guestAddress);

// Number of ring dwords consumed between intermediate RPTR writebacks. The
// guest-provided RB_BLKSZ is converted to this frequency by the video driver.
// Pm4_Execute still publishes its final/stalled cursor through the graphics
// pump, so the guest never loses the last bit of progress in a submitted batch.
void Pm4_SetReadPointerUpdateFrequency(uint32_t dwords);

// Called exactly where an INTERRUPT packet is reached in the command stream.
void Pm4_SetInterruptSink(void (*sink)());
void Pm4_SetInterruptCommandSink(void (*sink)());
void Pm4_SetInterruptWakeSink(void (*sink)());
void Pm4_NotifyWorkAvailable();
void Pm4_RendererInterruptHandshake();
void Pm4_ServiceRendererInterrupts();

// Xenos exposes a command-processor counter that advances with guest vblank
// progress and swaps. EVENT_WRITE_SHD may write this counter back to guest
// memory instead of the packet's immediate value.
void Pm4_IncrementCounter();
uint32_t Pm4_Counter();

struct Pm4ShaderBinding
{
    uint32_t ucodeVa = 0;
    uint32_t sizeDwords = 0;
    uint64_t hash = 0;
};

struct Pm4Draw
{
    uint32_t primType = 0;
    uint32_t indexCount = 0;
    // Diagnostic provenance of the packet that issued this draw. Keeping this
    // with the draw lets the renderer correlate visually duplicated work with
    // the exact PM4/IB address without changing execution semantics.
    uint32_t packetVa = 0;
    uint32_t packetSourceVa = 0;
    uint32_t packetPosition = 0;
    uint32_t packetDepth = 0;
    uint32_t packetHeader = 0;
    bool predicateForced = false;
    bool predicated = false;
    uint64_t binMask = ~0ull;
    uint64_t binSelect = ~0ull;
    bool indexed = false;
    uint32_t indexVa = 0;
    bool index32 = false;
    uint32_t indexEndian = 0;
    uint32_t indexEndianTop = 0;
    uint32_t indexSizeDword = 0;
};

// Renderer-facing seams. Shader notifications are sent once per distinct
// (stage, microcode hash) seen by the command processor; draw and swap callbacks
// remain strictly stream-positioned.
void Pm4_SetShaderSink(void (*sink)(uint32_t type, uint64_t hash,
                                    const uint8_t* code, uint32_t sizeDwords));
void Pm4_SetRegisterSink(void (*sink)(uint32_t index, uint32_t value));
void Pm4_SetRegisterBatchSink(void (*sink)(const uint32_t* indices,
                                          const uint32_t* values,
                                          std::size_t count));
void Pm4_SetStoreSink(void (*sink)(uint8_t* base, uint32_t guestAddress,
                                   uint32_t value));
void Pm4_SetDrawSink(void (*sink)(uint8_t* base, const Pm4Draw& draw));
void Pm4_SetSwapSink(void (*sink)(uint8_t* base, uint32_t frontBuffer,
                                  uint32_t width, uint32_t height));

// Crash's pre-rendered movies use the named binkdecompress shader container.
// Registering its container lets PM4 derive the actual microcode hash at
// runtime, rather than baking a title-build-specific hash into cadence logic.
bool Pm4_RegisterBinkPixelShaderContainer(const uint8_t* container,
                                          std::size_t sizeBytes);
bool Pm4_BinkVideoCadenceActive();

const Pm4ShaderBinding& Pm4_BoundShader(uint32_t stage);
const uint32_t* Pm4_Registers();

// Execute from the parser's current cursor up to the kicked write pointer.
// Returns the ring-relative cursor actually reached.
uint32_t Pm4_Execute(uint8_t* base, uint32_t writePtr);

uint32_t Pm4_Cursor();
uint64_t Pm4_PacketCount();
uint64_t Pm4_IndirectBufferCount();
uint64_t Pm4_InterruptCount();
uint64_t Pm4_GpuStoreCount();
uint64_t Pm4_WaitCount();
uint64_t Pm4_WaitStallCount();
uint64_t Pm4_DrawCount();
uint64_t Pm4_DrawSinkCpuNs();
uint64_t Pm4_ExecuteCpuNs();
uint64_t Pm4_ExecuteDrawSinkCpuNs();
uint64_t Pm4_ExecuteCallCount();
uint64_t Pm4_FrameCount();
uint64_t Pm4_ShaderBindCount();
uint64_t Pm4_ShaderCacheHitCount();
void Pm4_LogPacketProfile();
void Pm4_LogTimingProfile();
