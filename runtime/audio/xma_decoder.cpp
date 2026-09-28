#include "xma_decoder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "../kernel/klog.h"
#include "../kernel/memory.h"

#ifndef MOJORECOMP_HAS_FFMPEG
#define MOJORECOMP_HAS_FFMPEG 0
#endif

#if MOJORECOMP_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}
#endif

namespace mojorecomp::audio {
namespace {

constexpr uint32_t kPhysicalCachedBase = 0xA0000000u;

uint32_t PhysicalToCached(uint32_t physical)
{
    return kPhysicalCachedBase | (physical & 0x1FFFFFFFu);
}

constexpr uint32_t kContextCount = 320;
constexpr uint32_t kPacketBytes = 2048;
constexpr uint32_t kPacketHeaderBits = 32;
constexpr uint32_t kPacketDataBytes = kPacketBytes - 4;
constexpr uint32_t kFrameHeaderBits = 15;
constexpr uint32_t kMaxFrameLength = 0x7FFF;
constexpr uint32_t kMaxFrameSizeInPacketBits = 0x4000 - kPacketHeaderBits;
constexpr uint32_t kSamplesPerFrame = 512;
constexpr uint32_t kOutputBlockBytes = 256;
constexpr uint32_t kDecoderStartPadding = 192;
constexpr int kSampleRates[4] = {24000, 32000, 44100, 48000};

uint32_t LoadGuestWord(uint32_t guest, uint32_t word)
{
    const auto* p = reinterpret_cast<const uint32_t*>(g_guestMemory.Translate(guest));
    return _byteswap_ulong(p[word]);
}

void StoreGuestWord(uint32_t guest, uint32_t word, uint32_t value)
{
    auto* p = reinterpret_cast<uint32_t*>(g_guestMemory.Translate(guest));
    p[word] = _byteswap_ulong(value);
}

uint32_t PacketFrameOffset(const uint8_t* packet)
{
    return (((uint32_t(packet[0]) & 3u) << 13) |
            (uint32_t(packet[1]) << 5) | (uint32_t(packet[2]) >> 3)) + 32u;
}

uint8_t PacketSkipCount(const uint8_t* packet) { return packet[3]; }
uint8_t PacketFrameCount(const uint8_t* packet) { return packet[0] >> 2; }
uint8_t PacketMetadata(const uint8_t* packet) { return packet[2] & 7u; }
bool IsXma2Packet(const uint8_t* packet) { return PacketMetadata(packet) == 1; }

struct BitReader {
    const uint8_t* data{};
    size_t bits{};
    size_t at{};

    uint64_t Peek(size_t count) const
    {
        uint64_t value = 0;
        for (size_t i = 0; i < count; ++i)
        {
            const size_t bit = at + i;
            if (bit >= bits) break;
            value = (value << 1) | ((data[bit >> 3] >> (7 - (bit & 7))) & 1u);
        }
        return value;
    }

    size_t Remaining() const { return at < bits ? bits - at : 0; }

    void SetOffset(size_t offset) { at = std::min(offset, bits); }
    void Advance(size_t count) { at = std::min(at + count, bits); }

    uint64_t Read(size_t count)
    {
        const uint64_t value = Peek(count);
        Advance(count);
        return value;
    }

    size_t Copy(uint8_t* dst, size_t count)
    {
        const size_t startPadding = at & 7u;
        for (size_t i = 0; i < count && at < bits; ++i, ++at)
        {
            const uint8_t bit = (data[at >> 3] >> (7 - (at & 7))) & 1u;
            const size_t outBit = startPadding + i;
            const uint8_t mask = uint8_t(1u << (7 - (outBit & 7)));
            if (bit) dst[outBit >> 3] |= mask;
            else dst[outBit >> 3] &= uint8_t(~mask);
        }
        return startPadding;
    }
};

struct PacketInfo {
    uint8_t frameCount{};
    uint8_t currentFrame{};
    uint32_t currentFrameSize{};

    bool IsLastFrameInPacket() const
    {
        return frameCount && currentFrame == uint8_t(frameCount - 1);
    }
};

PacketInfo GetPacketInfo(const uint8_t* packet, uint32_t frameOffset)
{
    PacketInfo info{};
    const uint32_t firstFrameOffset = PacketFrameOffset(packet);
    BitReader stream{packet, kPacketBytes * 8u, firstFrameOffset};

    if (frameOffset < firstFrameOffset)
    {
        info.currentFrame = 0;
        info.currentFrameSize = firstFrameOffset - frameOffset;
    }

    for (;;)
    {
        if (stream.Remaining() < kFrameHeaderBits)
            break;

        const uint32_t frameSize = uint32_t(stream.Peek(kFrameHeaderBits));
        if (!frameSize || frameSize == kMaxFrameLength)
            break;

        if (stream.at == frameOffset)
        {
            info.currentFrame = info.frameCount;
            info.currentFrameSize = frameSize;
        }
        ++info.frameCount;

        if (frameSize > stream.Remaining())
            break;

        // XMA frame_size includes the 1-bit sequence flag at the end. The old
        // ReXGlue/Xenia parser consumes that bit separately; treating it as the
        // start of the next frame shifts every following header by one bit.
        stream.Advance(frameSize - 1);
        if (stream.Read(1) == 0)
            break;
    }

    if (IsXma2Packet(packet))
    {
        const uint8_t declared = PacketFrameCount(packet);
        if (declared > info.frameCount)
        {
            if (!info.currentFrameSize)
                info.currentFrame = info.frameCount;
            info.frameCount = declared;
        }
    }
    return info;
}

struct ContextView {
    uint32_t guest{};
    uint32_t d[10]{};
    uint32_t initial[10]{};

    explicit ContextView(uint32_t g) : guest(g)
    {
        for (uint32_t i = 0; i < 10; ++i)
        {
            d[i] = LoadGuestWord(g, i);
            initial[i] = d[i];
        }
    }

    bool InputValid(uint32_t i) const { return (d[0] & (1u << (20 + i))) != 0; }
    bool AnyInputValid() const { return InputValid(0) || InputValid(1); }
    void SetInputValid(uint32_t i, bool v)
    {
        const uint32_t mask = 1u << (20 + i);
        d[0] = v ? (d[0] | mask) : (d[0] & ~mask);
    }
    uint32_t PacketCount(uint32_t i) const { return i ? (d[1] & 0xFFFu) : (d[0] & 0xFFFu); }
    uint32_t InputPtr(uint32_t i) const { return d[5 + i] & 0x1FFFFFFFu; }
    uint32_t CurrentBuffer() const { return d[4] >> 31; }
    void SetCurrentBuffer(uint32_t i) { d[4] = (d[4] & 0x7FFFFFFFu) | ((i & 1u) << 31); }
    uint32_t InputReadBits() const { return d[2] & 0x03FFFFFFu; }
    void SetInputReadBits(uint32_t v) { d[2] = (d[2] & ~0x03FFFFFFu) | (v & 0x03FFFFFFu); }
    uint32_t OutputBlocks() const { return (d[0] >> 22) & 0x1Fu; }
    uint32_t OutputWriteBlocks() const { return (d[0] >> 27) & 0x1Fu; }
    void SetOutputWriteBlocks(uint32_t v) { d[0] = (d[0] & ~(0x1Fu << 27)) | ((v & 0x1Fu) << 27); }
    uint32_t OutputReadBlocks() const { return d[9] & 0x1Fu; }
    uint32_t OutputPtr() const { return d[7] & 0x1FFFFFFFu; }
    bool OutputValid() const { return (d[1] >> 31) != 0; }
    void SetOutputValid(bool v) { d[1] = v ? (d[1] | 0x80000000u) : (d[1] & 0x7FFFFFFFu); }
    uint32_t SubframeDecodeCount() const { return (d[1] >> 20) & 0xFu; }
    uint32_t OutputPadding() const { return (d[1] >> 24) & 7u; }
    uint32_t SampleRateId() const { return (d[1] >> 27) & 3u; }
    bool Stereo() const { return ((d[1] >> 29) & 1u) != 0; }
    void SetError(uint32_t e) { d[2] = (d[2] & 0x03FFFFFFu) | ((e & 0x1Fu) << 26); }

    void SwapInput()
    {
        const uint32_t current = CurrentBuffer();
        SetInputValid(current, false);
        SetCurrentBuffer(current ^ 1u);
        SetInputReadBits(kPacketHeaderBits);
    }

    void Store()
    {
        // XMA contexts are guest-visible and the title may update immutable
        // configuration fields while the asynchronous decoder is working. The
        // known-good ReXGlue implementation reloads the live context and merges
        // only fields the hardware itself owns. Writing all ten DWORDs here can
        // resurrect an old sample rate, buffer pointer or subframe configuration
        // when a short sound reuses a context, causing wrong pitch or missing SFX.
        uint32_t fresh[10]{};
        for (uint32_t i = 0; i < 10; ++i)
            fresh[i] = LoadGuestWord(guest, i);

        // DWORD 0: hardware owns loop_count and output write offset. Input-valid
        // bits are only cleared by the decoder; never re-set a bit the guest has
        // changed since this work item began.
        fresh[0] = (fresh[0] & ~(0xFFu << 12)) | (d[0] & (0xFFu << 12));
        fresh[0] = (fresh[0] & ~(0x1Fu << 27)) | (d[0] & (0x1Fu << 27));
        for (uint32_t inputIndex = 0; inputIndex < 2; ++inputIndex)
        {
            const uint32_t mask = 1u << (20 + inputIndex);
            if ((initial[0] & mask) && !(d[0] & mask))
                fresh[0] &= ~mask;
        }

        // DWORD 1: preserve rate/channels/subframe/loop configuration. Hardware
        // may only invalidate the output buffer.
        if ((initial[1] & 0x80000000u) && !(d[1] & 0x80000000u))
            fresh[1] &= ~0x80000000u;

        // DWORD 2: decoder-owned input read offset + error status. Preserve the
        // guest's error-set bit.
        fresh[2] = (fresh[2] & 0x80000000u) | (d[2] & 0x7FFFFFFFu);

        // DWORD 4: only current_buffer is decoder-owned; loop_end and packet
        // metadata may be changed by the title for the next short sound.
        fresh[4] = (fresh[4] & 0x7FFFFFFFu) | (d[4] & 0x80000000u);

        // DWORD 9: preserve stop/interrupt flags, merge the output read offset.
        fresh[9] = (fresh[9] & ~0x1Fu) | (d[9] & 0x1Fu);

        for (uint32_t i = 0; i < 10; ++i)
            StoreGuestWord(guest, i, fresh[i]);
    }
};

void RingWrite(uint8_t* ring, uint32_t capacity, uint32_t& writeOffset,
               const uint8_t* src, uint32_t bytes)
{
    if (!capacity || !bytes) return;
    const uint32_t first = std::min(bytes, capacity - writeOffset);
    std::memcpy(ring + writeOffset, src, first);
    if (bytes > first) std::memcpy(ring, src + first, bytes - first);
    writeOffset = (writeOffset + bytes) % capacity;
}

#if MOJORECOMP_HAS_FFMPEG
struct DecoderState {
    std::mutex mutex;
    AVCodecContext* codecContext{};
    const AVCodec* codec{};
    AVPacket* packet{};
    AVFrame* frame{};
    int sampleRate{};
    int channels{};
    std::array<uint8_t, kPacketDataBytes * 2> input{};
    std::array<uint8_t, 4097> xmaFrame{};
    std::array<uint8_t, 2048> pcm{};
    std::array<uint8_t, 2048> decodedFrame{};
    std::array<uint8_t, 2048> carryFrame{};
    uint32_t pcmAt{};
    uint32_t pcmBytes{};
    bool carryValid{};
    uint32_t streamIndex{};
    bool streamIndexValid{};

    bool EnsureCodec(int rate, int ch)
    {
        if (!codec) codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
        if (!codec) return false;
        if (codecContext && sampleRate == rate && channels == ch) return true;
        if (codecContext) avcodec_free_context(&codecContext);
        codecContext = avcodec_alloc_context3(codec);
        if (!codecContext) return false;
        codecContext->sample_rate = rate;
        codecContext->channels = ch;
        codecContext->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
        if (avcodec_open2(codecContext, codec, nullptr) < 0)
        {
            avcodec_free_context(&codecContext);
            return false;
        }
        sampleRate = rate;
        channels = ch;
        pcmAt = pcmBytes = 0;
        carryValid = false;
        streamIndex = 0;
        streamIndexValid = false;
        return true;
    }

    bool EnsureObjects()
    {
        if (!packet) packet = av_packet_alloc();
        if (!frame) frame = av_frame_alloc();
        return packet && frame;
    }

    void Reset()
    {
        if (codecContext) avcodec_free_context(&codecContext);
        sampleRate = channels = 0;
        pcmAt = pcmBytes = 0;
        carryValid = false;
    }

    ~DecoderState()
    {
        if (codecContext) avcodec_free_context(&codecContext);
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
    }
};

std::array<DecoderState, kContextCount> g_states;

bool BoundaryTraceEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_XMA_DIAGNOSTICS");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

bool IsStreamingInput(const ContextView& c)
{
    return c.InputPtr(0) >= 0x0F000000u || c.InputPtr(1) >= 0x0F000000u;
}

void LearnXmaBlockStreamIndex(DecoderState& state, const ContextView& c, uint32_t current,
                              uint32_t contextId)
{
    if (state.streamIndexValid)
        return;

    const uint32_t next = current ^ 1u;
    if (!c.InputValid(current) || !c.InputValid(next))
        return;

    const uint32_t currentPtr = c.InputPtr(current);
    const uint32_t nextPtr = c.InputPtr(next);
    const uint32_t currentCount = c.PacketCount(current);
    const uint32_t nextCount = c.PacketCount(next);
    if (!currentPtr || !nextPtr || !currentCount || !nextCount || nextPtr <= currentPtr)
        return;

    const uint64_t nextBlockBytes = uint64_t(nextCount) * kPacketBytes;
    if (uint64_t(nextPtr) < nextBlockBytes)
        return;

    const uint32_t previousBlockBase = uint32_t(uint64_t(nextPtr) - nextBlockBytes);
    if (currentPtr < previousBlockBase || currentPtr >= nextPtr)
        return;

    const uint32_t phaseBytes = currentPtr - previousBlockBase;
    if ((phaseBytes % kPacketBytes) != 0)
        return;

    // The title hands each hardware context a view beginning at that stream's
    // first packet in the current XMA block. The view then ends exactly at the
    // next block base. For a 3-stream/6-channel asset this looks like:
    //   stream 0: base + 0 packets, count 32
    //   stream 1: base + 1 packet,  count 31
    //   stream 2: base + 2 packets, count 30
    // while the following input buffer is the common 32-packet block base.
    // XMA2 guarantees the first packets of every block are in stream order.
    // Preserve that stream index across block swaps instead of resetting all
    // contexts to packet 0 (which collapses 5.1 into one stereo pair).
    const uint32_t streamIndex = phaseBytes / kPacketBytes;
    if (streamIndex >= nextCount ||
        uint64_t(currentPtr) + uint64_t(currentCount) * kPacketBytes != uint64_t(nextPtr))
        return;

    state.streamIndex = streamIndex;
    state.streamIndexValid = true;
    if (BoundaryTraceEnabled() && contextId < 3 && IsStreamingInput(c))
        KLOG("XMA boundary ctx=%u event=learn-stream stream=%u cur=%u curptr=%08X curpackets=%u nextptr=%08X nextpackets=%u blockbase=%08X\n",
             contextId, streamIndex, current, currentPtr, currentCount, nextPtr, nextCount,
             previousBlockBase);
}

bool SetXmaBlockStreamStart(DecoderState& state, ContextView& c, uint32_t contextId,
                            const char* event)
{
    if (!state.streamIndexValid)
        return false;

    const uint32_t current = c.CurrentBuffer();
    if (!c.InputValid(current) || !c.InputPtr(current))
        return false;

    const uint32_t packetCount = c.PacketCount(current);
    if (state.streamIndex >= packetCount)
        return false;

    const uint32_t packetBits = kPacketBytes * 8u;
    const auto* base = static_cast<const uint8_t*>(
        g_guestMemory.Translate(PhysicalToCached(c.InputPtr(current))));
    const uint8_t* packet = base + state.streamIndex * kPacketBytes;
    const uint32_t frameOffset = PacketFrameOffset(packet);
    if (frameOffset > kMaxFrameSizeInPacketBits)
        return false;

    c.SetInputReadBits(state.streamIndex * packetBits + frameOffset);
    if (BoundaryTraceEnabled() && contextId < 3 && IsStreamingInput(c))
        KLOG("XMA boundary ctx=%u event=%s stream=%u cur=%u packets=%u ptr=%08X frameoff=%u read=%u\n",
             contextId, event, state.streamIndex, current, packetCount, c.InputPtr(current),
             frameOffset, c.InputReadBits());
    return true;
}

void SwapXmaBlockInput(DecoderState& state, ContextView& c, uint32_t contextId,
                       const char* event)
{
    const uint32_t old = c.CurrentBuffer();
    LearnXmaBlockStreamIndex(state, c, old, contextId);
    c.SwapInput();
    if (!SetXmaBlockStreamStart(state, c, contextId, event) &&
        BoundaryTraceEnabled() && contextId < 3 && IsStreamingInput(c))
    {
        KLOG("XMA boundary ctx=%u event=%s-fallback old=%u new=%u streamvalid=%u stream=%u read=%u\n",
             contextId, event, old, c.CurrentBuffer(), state.streamIndexValid ? 1u : 0u,
             state.streamIndex, c.InputReadBits());
    }
}

void DumpLogicalPcm(uint32_t contextId, const DecoderState& state, const ContextView& c)
{
    if (contextId >= 3 || state.pcmBytes == 0 || !IsStreamingInput(c))
        return;
    const char* directory = std::getenv("MOJORECOMP_XMA_PCM_DUMP_DIR");
    if (!directory || !directory[0])
        return;

    char path[1024]{};
    std::snprintf(path, sizeof(path), "%s\\ctx%u.raw", directory, contextId);
    if (FILE* file = std::fopen(path, "ab"))
    {
        std::fwrite(state.pcm.data(), 1, state.pcmBytes, file);
        std::fclose(file);
    }
}

bool DecodeOneFrame(DecoderState& state, ContextView& c, uint32_t contextId)
{
    uint32_t current = c.CurrentBuffer();
    LearnXmaBlockStreamIndex(state, c, current, contextId);
    if (!c.InputValid(current))
    {
        const uint32_t old = current;
        SwapXmaBlockInput(state, c, contextId, "invalid-swap");
        current = c.CurrentBuffer();
        if (BoundaryTraceEnabled() && contextId < 3 && IsStreamingInput(c))
            KLOG("XMA boundary ctx=%u event=invalid-swap old=%u new=%u read=%u in0=%08X in1=%08X valid=%u%u\n",
                 contextId, old, current, c.InputReadBits(), c.InputPtr(0), c.InputPtr(1),
                 c.InputValid(0) ? 1u : 0u, c.InputValid(1) ? 1u : 0u);
        if (!c.InputValid(current)) return false;
    }

    const uint32_t packetCount = c.PacketCount(current);
    const uint32_t inputBytes = packetCount * kPacketBytes;
    if (!packetCount || !c.InputPtr(current)) return false;
    uint32_t readBits = std::max(c.InputReadBits(), kPacketHeaderBits);
    if (readBits >= inputBytes * 8u)
    {
        if (BoundaryTraceEnabled() && contextId < 3 && IsStreamingInput(c))
            KLOG("XMA boundary ctx=%u event=read-end-swap cur=%u packets=%u read=%u bytes=%u in0=%08X in1=%08X\n",
                 contextId, current, packetCount, readBits, inputBytes, c.InputPtr(0), c.InputPtr(1));
        SwapXmaBlockInput(state, c, contextId, "read-end-swap");
        return false;
    }

    const uint32_t packetIndex = readBits / (kPacketBytes * 8u);
    const uint8_t* input = static_cast<const uint8_t*>(
        g_guestMemory.Translate(PhysicalToCached(c.InputPtr(current))));
    const uint8_t* packet = input + packetIndex * kPacketBytes;
    uint32_t relative = readBits % (kPacketBytes * 8u);
    const uint32_t firstFrame = PacketFrameOffset(packet);
    if (relative < firstFrame)
    {
        relative = firstFrame;
        readBits = packetIndex * kPacketBytes * 8u + relative;
    }

    const uint32_t skip = PacketSkipCount(packet);
    const auto nextPacketReadOffset = [&](uint32_t index) {
        while (index < packetCount)
        {
            const uint8_t* candidate = input + index * kPacketBytes;
            const uint32_t offset = PacketFrameOffset(candidate);
            if (offset <= kMaxFrameSizeInPacketBits)
                return index * kPacketBytes * 8u + offset;
            ++index;
        }
        return kPacketHeaderBits;
    };

    // 0xFF means this packet contains no newly-starting frame. ReXGlue skips to
    // the next packet with a valid first-frame offset rather than attempting to
    // interpret the packet payload as a frame header.
    if (skip == 0xFF)
    {
        const uint32_t next = nextPacketReadOffset(packetIndex + 1);
        if (next == kPacketHeaderBits)
        {
            if (BoundaryTraceEnabled() && contextId < 3 && IsStreamingInput(c))
                KLOG("XMA boundary ctx=%u event=ff-swap cur=%u packet=%u packets=%u read=%u\n",
                     contextId, current, packetIndex, packetCount, readBits);
            SwapXmaBlockInput(state, c, contextId, "ff-swap");
        }
        else
            c.SetInputReadBits(next);
        return false;
    }

    PacketInfo packetInfo = GetPacketInfo(packet, relative);
    uint32_t frameAt = relative;
    uint32_t frameSize = packetInfo.currentFrameSize;
    const uint32_t nextPacketIndex = packetIndex + uint32_t(skip) + 1u;
    const uint32_t crossBufferPacketIndex =
        nextPacketIndex >= packetCount ? nextPacketIndex - packetCount : 0u;
    if (BoundaryTraceEnabled() && contextId < 3 && IsStreamingInput(c) &&
        nextPacketIndex >= packetCount)
    {
        const uint32_t nextBuffer = current ^ 1u;
        KLOG("XMA boundary ctx=%u event=cross cur=%u packet=%u packets=%u skip=%u next=%u cross=%u nextbuf=%u nextvalid=%u nextpackets=%u read=%u in0=%08X in1=%08X\n",
             contextId, current, packetIndex, packetCount, skip, nextPacketIndex,
             crossBufferPacketIndex, nextBuffer, c.InputValid(nextBuffer) ? 1u : 0u,
             c.PacketCount(nextBuffer), readBits, c.InputPtr(0), c.InputPtr(1));
    }
    const uint8_t* nextPacket = nullptr;
    if (nextPacketIndex < packetCount)
        nextPacket = input + nextPacketIndex * kPacketBytes;
    else
    {
        const uint32_t nextBuffer = current ^ 1u;
        if (c.InputValid(nextBuffer) && c.InputPtr(nextBuffer))
        {
            const uint32_t nextPacketCount = c.PacketCount(nextBuffer);
            const uint32_t nextBufferPacketIndex =
                state.streamIndexValid ? state.streamIndex : crossBufferPacketIndex;
            if (nextBufferPacketIndex < nextPacketCount)
            {
                const auto* nextBufferBase = static_cast<const uint8_t*>(
                    g_guestMemory.Translate(PhysicalToCached(c.InputPtr(nextBuffer))));
                nextPacket = nextBufferBase + nextBufferPacketIndex * kPacketBytes;
            }
        }
    }

    std::fill(state.input.begin(), state.input.end(), 0);
    std::memcpy(state.input.data(), packet + 4, kPacketDataBytes);
    if (nextPacket) std::memcpy(state.input.data() + kPacketDataBytes, nextPacket + 4, kPacketDataBytes);

    // A frame header may straddle the packet payload boundary. Reconstruct the
    // two payloads exactly as ReXGlue does and read the header from the combined
    // bitstream in that case.
    if (!frameSize && nextPacket)
    {
        BitReader combined{state.input.data(), state.input.size() * 8u,
                           relative - kPacketHeaderBits};
        frameSize = uint32_t(combined.Peek(kFrameHeaderBits));
    }
    if (!frameSize)
    {
        // Match ReXGlue exactly. A zero frame size at a packet/buffer boundary
        // is not a parser error. The old decoder reaches bits_to_copy == 0,
        // swaps the input buffer and returns. Marking error_status=4 here makes
        // Crash treat a normal streamed-dialogue boundary as a broken stream,
        // which can restart/reuse the previous short reaction instead of
        // advancing to the next voice block.
        const uint32_t old = c.CurrentBuffer();
        SwapXmaBlockInput(state, c, contextId, "zero-frame-swap");
        if (BoundaryTraceEnabled() && contextId < 3 && IsStreamingInput(c))
            KLOG("XMA boundary ctx=%u event=zero-frame-swap old=%u new=%u cross=%u read=%u\n",
                 contextId, old, c.CurrentBuffer(), crossBufferPacketIndex, c.InputReadBits());
        return false;
    }
    if (frameSize == kMaxFrameLength)
    {
        static std::atomic<uint32_t> frameScanReports{0};
        const uint32_t report = frameScanReports.fetch_add(1, std::memory_order_relaxed);
        if (report < 16)
            KLOG("XMA decoder: no frame ctx=%08X buf=%u packets=%u ptr=%08X read=%u packet=%u first=%u hdr=%02X %02X %02X %02X\n",
                 c.guest, current, packetCount, c.InputPtr(current), readBits,
                 packetIndex, firstFrame, packet[0], packet[1], packet[2], packet[3]);
        c.SetError(4);
        return false;
    }

    const auto advanceInput = [&]() {
        const uint32_t packetBits = kPacketBytes * 8u;
        const uint32_t remaining = packetBits - relative;
        const uint32_t bitsToCopy = std::min(remaining, frameSize);

        if (!packetInfo.IsLastFrameInPacket())
        {
            const uint32_t nextFrameOffset = (readBits + bitsToCopy) % packetBits;
            c.SetInputReadBits(packetIndex * packetBits + nextFrameOffset);
        }
        else
        {
            uint32_t next = nextPacketReadOffset(nextPacketIndex);
            if (next == kPacketHeaderBits)
            {
                const uint32_t oldBuffer = c.CurrentBuffer();
                LearnXmaBlockStreamIndex(state, c, oldBuffer, contextId);
                c.SwapInput();
                if (c.InputValid(c.CurrentBuffer()) && c.InputPtr(c.CurrentBuffer()))
                {
                    const auto* p = static_cast<const uint8_t*>(
                        g_guestMemory.Translate(PhysicalToCached(c.InputPtr(c.CurrentBuffer()))));
                    const uint32_t nextCount = c.PacketCount(c.CurrentBuffer());

                    // XMA packets from multi-stream assets are interleaved. The
                    // packet skip count may step past the end of the current
                    // guest buffer; that overshoot is the phase of this stream
                    // in the next shared buffer. ReXGlue historically reset all
                    // streams to packet 0 here, causing 3 stereo cutscene
                    // contexts to converge onto the same pair of channels after
                    // the first buffer boundary. Preserve the overshoot instead.
                    uint32_t phasePacket = state.streamIndexValid
                        ? state.streamIndex : crossBufferPacketIndex;
                    while (phasePacket < nextCount)
                    {
                        const uint8_t* phasedPacket = p + phasePacket * kPacketBytes;
                        const uint32_t phaseOffset = PacketFrameOffset(phasedPacket);
                        if (phaseOffset <= kMaxFrameSizeInPacketBits)
                        {
                            c.SetInputReadBits(phasePacket * packetBits + phaseOffset);
                            if (BoundaryTraceEnabled() && contextId < 3 && IsStreamingInput(c))
                                KLOG("XMA boundary ctx=%u event=phase-swap old=%u new=%u oldpacket=%u oldcount=%u skip=%u next=%u cross=%u phase=%u phaseoff=%u newcount=%u newread=%u\n",
                                     contextId, oldBuffer, c.CurrentBuffer(), packetIndex, packetCount,
                                     skip, nextPacketIndex, crossBufferPacketIndex, phasePacket,
                                     phaseOffset, nextCount, c.InputReadBits());
                            return;
                        }
                        ++phasePacket;
                    }

                    // No usable packet at this stream phase in the next buffer.
                    if (BoundaryTraceEnabled() && contextId < 3 && IsStreamingInput(c))
                        KLOG("XMA boundary ctx=%u event=phase-miss old=%u new=%u cross=%u newcount=%u\n",
                             contextId, oldBuffer, c.CurrentBuffer(), crossBufferPacketIndex, nextCount);
                    c.SwapInput();
                }
            }
            else
                c.SetInputReadBits(next);
        }
    };

    BitReader bits{state.input.data(), state.input.size() * 8u, frameAt - kPacketHeaderBits};
    if (bits.Remaining() < frameSize)
    {
        KLOG("XMA decoder: short frame ctx=%08X frameAt=%u frameSize=%u remaining=%zu\n",
             c.guest, frameAt, frameSize, bits.Remaining());
        c.SetError(4);
        return false;
    }
    std::fill(state.xmaFrame.begin(), state.xmaFrame.end(), 0);
    const size_t paddingStart = bits.Copy(state.xmaFrame.data() + 1, frameSize);
    const size_t paddingEnd = (8u - ((8u + paddingStart + frameSize) & 7u)) & 7u;
    state.xmaFrame[0] = uint8_t(((paddingStart & 7u) << 5) | ((paddingEnd & 7u) << 2));

    const int rate = kSampleRates[c.SampleRateId()];
    const int channels = c.Stereo() ? 2 : 1;
    if (!state.EnsureObjects() || !state.EnsureCodec(rate, channels))
    {
        KLOG("XMA decoder: codec setup failed ctx=%08X rate=%d channels=%d\n",
             c.guest, rate, channels);
        return false;
    }
    av_packet_unref(state.packet);
    state.packet->data = state.xmaFrame.data();
    state.packet->size = int(1 + ((paddingStart + frameSize + 7u) / 8u));
    av_frame_unref(state.frame);
    const int sent = avcodec_send_packet(state.codecContext, state.packet);
    if (sent < 0)
    {
        char error[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(sent, error, sizeof(error));
        KLOG("XMA decoder: send failed ctx=%08X frame=%u error=%d %s\n",
             c.guest, frameSize, sent, error);
        // Match the old ReXGlue XmaContext path: a malformed frame is dropped,
        // but the bitstream cursor still advances. Otherwise the hardware
        // context retries the same bad frame forever and all later audio stalls.
        state.pcmAt = state.pcmBytes = 0;
        state.carryValid = false;
        advanceInput();
        return false;
    }
    const int receive = avcodec_receive_frame(state.codecContext, state.frame);
    if (receive < 0)
    {
        char error[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(receive, error, sizeof(error));
        KLOG("XMA decoder: receive failed ctx=%08X frame=%u error=%d %s\n",
             c.guest, frameSize, receive, error);
        state.pcmAt = state.pcmBytes = 0;
        state.carryValid = false;
        advanceInput();
        return false;
    }

    const uint32_t samples = std::min<uint32_t>(state.frame->nb_samples, kSamplesPerFrame);
    std::fill(state.decodedFrame.begin(), state.decodedFrame.end(), 0);
    uint32_t out = 0;
    for (uint32_t i = 0; i < samples; ++i)
    {
        for (int ch = 0; ch < channels; ++ch)
        {
            const auto* src = reinterpret_cast<const float*>(state.frame->extended_data[ch]);
            const float f = std::clamp(src[i], -1.0f, 1.0f);
            const int16_t s = int16_t(std::lrintf(f * 32767.0f));
            if (out + 2 > state.decodedFrame.size()) break;
            state.decodedFrame[out++] = uint8_t(uint16_t(s) >> 8);
            state.decodedFrame[out++] = uint8_t(uint16_t(s));
        }
    }

    // FFmpeg exposes the XMA decoder's 192-sample start padding as ordinary
    // output. The Xbox hardware's logical 512-sample frame is instead the tail
    // (samples 192..511) of one decoded block followed by the padding head
    // (samples 0..191) of the next block. ReXGlue preserves that adjacency with
    // a one-frame carry. Without it every logical frame loses 192 samples and
    // audio duration becomes much too short.
    const uint32_t frameBytes = kSamplesPerFrame * uint32_t(channels) * 2u;
    const uint32_t padBytes = kDecoderStartPadding * uint32_t(channels) * 2u;
    const uint32_t carryBytes = frameBytes - padBytes;
    if (state.carryValid)
    {
        std::memcpy(state.pcm.data(), state.carryFrame.data(), carryBytes);
        std::memcpy(state.pcm.data() + carryBytes, state.decodedFrame.data(), padBytes);
        state.pcmAt = 0;
        state.pcmBytes = frameBytes;
        DumpLogicalPcm(contextId, state, c);
    }
    else
    {
        // The first decoder block only primes the carry; there isn't a complete
        // hardware-aligned logical frame until the following block arrives.
        state.pcmAt = state.pcmBytes = 0;
    }
    std::memcpy(state.carryFrame.data(), state.decodedFrame.data() + padBytes, carryBytes);
    state.carryValid = true;

    advanceInput();
    return true;
}
#endif

} // namespace

bool XmaDecoderAvailable()
{
#if MOJORECOMP_HAS_FFMPEG
    static const bool available = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES) != nullptr;
    return available;
#else
    return false;
#endif
}

void XmaDecoderReset(uint32_t contextId)
{
#if MOJORECOMP_HAS_FFMPEG
    if (contextId >= kContextCount) return;
    std::lock_guard lock(g_states[contextId].mutex);
    g_states[contextId].Reset();
#else
    (void)contextId;
#endif
}

void XmaDecoderRelease(uint32_t contextId) { XmaDecoderReset(contextId); }

bool XmaDecoderWork(uint32_t contextId, uint32_t contextGuest)
{
#if !MOJORECOMP_HAS_FFMPEG
    (void)contextId; (void)contextGuest;
    return false;
#else
    if (contextId >= kContextCount || !contextGuest) return false;
    DecoderState& state = g_states[contextId];
    std::lock_guard lock(state.mutex);
    ContextView c(contextGuest);
    if (!c.OutputValid() || !c.OutputBlocks() || !c.OutputPtr()) return false;


    const uint32_t capacity = c.OutputBlocks() * kOutputBlockBytes;
    uint32_t read = (c.OutputReadBlocks() * kOutputBlockBytes) % capacity;
    uint32_t write = (c.OutputWriteBlocks() * kOutputBlockBytes) % capacity;
    uint32_t freeBytes = read == write ? capacity :
                         (write < read ? read - write : capacity - write + read);
    const uint32_t subframeBlocks = std::max(1u, c.SubframeDecodeCount());
    const uint32_t minimumFreeBlocks = subframeBlocks + c.OutputPadding();
    if (freeBytes < minimumFreeBlocks * kOutputBlockBytes) return false;
    auto* output = static_cast<uint8_t*>(
        g_guestMemory.Translate(PhysicalToCached(c.OutputPtr())));

    // Match ReXGlue's Work() granularity: one hardware kick performs a complete
    // work pass, repeatedly decoding and draining subframes until the writable
    // part of the output ring can no longer accept another decode quantum. The
    // old migration returned after one 256-byte group and needed a permanently
    // enabled host worker to make progress; that worker then retried exhausted
    // cutscene contexts forever.
    uint32_t writableBlocks = freeBytes / kOutputBlockBytes;
    bool didWork = false;
    for (uint32_t workPass = 0;
         workPass < 64 && writableBlocks >= minimumFreeBlocks;
         ++workPass)
    {
        if (state.pcmAt >= state.pcmBytes)
        {
            state.pcmAt = state.pcmBytes = 0;

            // The first FFmpeg block only primes the 192-sample carry, so one
            // logical frame may require two decoder calls. Also allow a buffer
            // swap/end marker to be consumed without treating it as a hard
            // failure, but stop if the decoder makes no progress.
            bool producedPcm = false;
            for (uint32_t decodeAttempt = 0; decodeAttempt < 6; ++decodeAttempt)
            {
                if (!c.AnyInputValid())
                    break;
                const uint32_t beforeOffset = c.InputReadBits();
                const uint32_t beforeBuffer = c.CurrentBuffer();
                const bool decoded = DecodeOneFrame(state, c, contextId);
                if (state.pcmAt < state.pcmBytes)
                {
                    producedPcm = true;
                    break;
                }
                if (!decoded &&
                    beforeOffset == c.InputReadBits() &&
                    beforeBuffer == c.CurrentBuffer())
                    break;
            }
            if (!producedPcm)
                break;
        }

        const uint32_t remainingBytes = state.pcmBytes - state.pcmAt;
        const uint32_t remainingBlocks = remainingBytes / kOutputBlockBytes;
        const uint32_t blocksToWrite =
            std::min({subframeBlocks, remainingBlocks, writableBlocks});
        if (!blocksToWrite)
            break;

        const uint32_t bytesToWrite = blocksToWrite * kOutputBlockBytes;
        RingWrite(output, capacity, write, state.pcm.data() + state.pcmAt, bytesToWrite);
        state.pcmAt += bytesToWrite;
        writableBlocks -= blocksToWrite;
        didWork = true;

        const bool frameFinished = state.pcmAt >= state.pcmBytes;
        if (frameFinished)
        {
            state.pcmAt = state.pcmBytes = 0;
            const uint32_t headroom = std::min(c.OutputPadding(), writableBlocks);
            writableBlocks -= headroom;
        }
    }

    c.SetOutputWriteBlocks(write / kOutputBlockBytes);

    // Equal offsets after consuming the complete writable budget mean full, not
    // empty. This is the same ambiguity handled by ReXGlue's output ring.
    if (!writableBlocks && write == read)
        c.SetOutputValid(false);

    c.Store();
    return didWork;
#endif
}

} // namespace mojorecomp::audio
