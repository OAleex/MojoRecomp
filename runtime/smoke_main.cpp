#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "ppc_recomp_shared.h"

int main() {
    constexpr uint64_t image_lo = PPC_IMAGE_BASE;
    constexpr uint64_t image_hi = PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
    constexpr uint64_t code_lo = PPC_CODE_BASE;
    constexpr uint64_t code_hi = PPC_CODE_BASE + PPC_CODE_SIZE;

    std::size_t count = 0;
    std::size_t failures = 0;
    uint64_t previous = 0;
    uint64_t first = UINT64_MAX;
    uint64_t last = 0;

    for (const PPCFuncMapping* m = PPCFuncMappings;
         m->guest != 0 || m->host != nullptr; ++m, ++count) {
        const uint64_t guest = m->guest;
        if (!m->host) {
            std::fprintf(stderr, "null host function at mapping %zu (0x%08" PRIX64 ")\n",
                         count, guest);
            ++failures;
        }
        if (guest < image_lo || guest >= image_hi) {
            std::fprintf(stderr, "mapping outside image at %zu: 0x%08" PRIX64 "\n",
                         count, guest);
            ++failures;
        }
        if ((guest & 3u) != 0) {
            std::fprintf(stderr, "unaligned mapping at %zu: 0x%08" PRIX64 "\n",
                         count, guest);
            ++failures;
        }
        if (count && guest <= previous) {
            std::fprintf(stderr,
                         "mapping order failure at %zu: 0x%08" PRIX64
                         " after 0x%08" PRIX64 "\n",
                         count, guest, previous);
            ++failures;
        }
        previous = guest;
        if (guest < first) first = guest;
        if (guest > last) last = guest;
    }

    std::printf("MojoRecomp XenonRecomp image smoke\n");
    std::printf("  image  0x%08" PRIX64 "..0x%08" PRIX64 " (%" PRIu64 " bytes)\n",
                image_lo, image_hi, uint64_t(PPC_IMAGE_SIZE));
    std::printf("  code   0x%08" PRIX64 "..0x%08" PRIX64 " (%" PRIu64 " bytes)\n",
                code_lo, code_hi, uint64_t(PPC_CODE_SIZE));
    std::printf("  maps   %zu entries, 0x%08" PRIX64 "..0x%08" PRIX64 "\n",
                count, first, last);

    if (count == 0) {
        std::fprintf(stderr, "FAIL: mapping table is empty\n");
        return 1;
    }
    if (failures) {
        std::fprintf(stderr, "FAIL: %zu mapping/link invariant failure(s)\n", failures);
        return 1;
    }

    std::puts("OK: the complete translated image links and its mapping table is sane.");
    return 0;
}
