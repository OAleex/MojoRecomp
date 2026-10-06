// Synthetic PM4 regression: screen-extent writeback is not EVENT_WRITE_SHD.
// No executable, shader, or other game data is used by this test.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../gpu/pm4.h"
#include "../gpu/xenos.h"
#include "../title_resources.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

void StoreBe32(uint8_t* destination, uint32_t value)
{
    destination[0] = static_cast<uint8_t>(value >> 24);
    destination[1] = static_cast<uint8_t>(value >> 16);
    destination[2] = static_cast<uint8_t>(value >> 8);
    destination[3] = static_cast<uint8_t>(value);
}

} // namespace

int main()
{
    auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 1ull << 32,
                                                   MEM_RESERVE, PAGE_NOACCESS));
    if (!base) return 1;
    const bool committed = VirtualAlloc(base + 0xA0001000u, 8192,
        MEM_COMMIT, PAGE_READWRITE) != nullptr;
    if (!committed) { VirtualFree(base, 0, MEM_RELEASE); return 1; }
    auto store = [&](uint32_t address, uint32_t value) {
        value = __builtin_bswap32(value);
        std::memcpy(base + address, &value, sizeof(value));
    };
    // Two-dword EVENT_WRITE_EXT: event initiator and endian-tagged destination.
    // It produces six uint16 extents, NOT an immediate data dword.
    store(0xA0001000u, 0xC0015A00u);
    store(0xA0001004u, 0x0000001Au);
    store(0xA0001008u, 0x00002001u); // 8-in-16 endian
    std::memset(base + 0xA0002000u, 0xCD, 16);
    Pm4_SetRingBuffer(0xA0001000u, 4096);
    const bool consumed = Pm4_Execute(base, 3) == 3;
    uint16_t extent[6]{};
    for (unsigned i = 0; i < 6; ++i)
        extent[i] = (uint16_t(base[0xA0002000u + i * 2]) << 8) |
                     base[0xA0002001u + i * 2];
    // A conservative result may be larger, but must include a full-screen draw
    // so the CPU's next-frame bin mask does not discard the middle/right tiles.
    const bool valid = consumed && extent[0] == 0 && extent[2] == 0 &&
        extent[1] >= 1280 / 8 && extent[1] <= 8192 / 8 &&
        extent[3] >= 720 / 8 && extent[3] <= 8192 / 8 &&
        extent[4] == 0 && extent[5] == 1 && base[0xA000200Cu] == 0xCD;
    std::printf("%s: EVENT_WRITE_EXT bounds x=%u..%u y=%u..%u z=%u..%u\n",
        valid ? "PASS" : "FAIL", extent[0], extent[1], extent[2], extent[3], extent[4], extent[5]);

    // Regression for the shader-constant Type-0 fast path. Sequential packets
    // must preserve every big-endian guest dword, while one-register packets
    // must leave the final body value in the selected register.
    constexpr uint32_t kSequentialCount = 8;
    constexpr uint32_t kSequentialReg = xenos::kAluConstantBase + 0x20;
    uint32_t at = 3;
    store(0xA0001000u + at++ * 4u,
          ((kSequentialCount - 1u) << 16) | kSequentialReg);
    for (uint32_t i = 0; i < kSequentialCount; ++i)
        store(0xA0001000u + at++ * 4u, 0x10203040u + i * 0x01010101u);

    constexpr uint32_t kOneRegCount = 4;
    constexpr uint32_t kOneReg = xenos::kAluConstantBase + 0x40;
    store(0xA0001000u + at++ * 4u,
          ((kOneRegCount - 1u) << 16) | (1u << 15) | kOneReg);
    for (uint32_t i = 0; i < kOneRegCount; ++i)
        store(0xA0001000u + at++ * 4u, 0xA0B0C000u + i);

    const bool constantsConsumed = Pm4_Execute(base, at) == at;
    const uint32_t* regs = Pm4_Registers();
    bool constantsValid = constantsConsumed;
    for (uint32_t i = 0; i < kSequentialCount; ++i)
        constantsValid &= regs[kSequentialReg + i] == 0x10203040u + i * 0x01010101u;
    constantsValid &= regs[kOneReg] == 0xA0B0C003u;
    std::printf("%s: Type-0 shader constant fast path\n",
                constantsValid ? "PASS" : "FAIL");

    // A ShaderContainer uses the 0x102A11xx family. The low byte contains
    // container flags and must not make a structurally valid pixel shader fail.
    std::vector<uint8_t> binkContainer(0x24u + 12u, 0);
    StoreBe32(binkContainer.data() + 0x00u, 0x102A117Fu);
    StoreBe32(binkContainer.data() + 0x04u, 0x24u);
    StoreBe32(binkContainer.data() + 0x08u, 12u);
    StoreBe32(binkContainer.data() + 0x18u, 0x1Cu);
    StoreBe32(binkContainer.data() + 0x1Cu, 0u);
    StoreBe32(binkContainer.data() + 0x20u, 12u);
    StoreBe32(binkContainer.data() + 0x24u, 0x00001000u);
    StoreBe32(binkContainer.data() + 0x28u, 0x00001000u);
    StoreBe32(binkContainer.data() + 0x2Cu, 0u);
    const bool binkFamilyValid = Pm4_RegisterBinkPixelShaderContainer(
        binkContainer.data(), binkContainer.size());
    binkContainer[0] = 0x11;
    const bool binkRejectsWrongFamily = !Pm4_RegisterBinkPixelShaderContainer(
        binkContainer.data(), binkContainer.size());
    const bool binkPathValid = CotTitleResources_IsBinkShaderPath(
        "D:\\SHADERS\\BinkDecompress.out") &&
        CotTitleResources_IsBinkShaderPath("D:/shaders/binkdecompress.out") &&
        !CotTitleResources_IsBinkShaderPath("D:\\movies\\intro.bik");
    std::printf("%s: Bink ShaderContainer 0x102A11xx family\n",
                binkFamilyValid && binkRejectsWrongFamily && binkPathValid
                    ? "PASS" : "FAIL");

    VirtualFree(base, 0, MEM_RELEASE);
    return valid && constantsValid && binkFamilyValid && binkRejectsWrongFamily &&
           binkPathValid ? 0 : 1;
}
