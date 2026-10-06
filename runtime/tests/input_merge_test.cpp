#include "../host/input_merge.h"
#include "../host/input.h"

#include <cstdint>
#include <cstdio>
#include <limits>

int main()
{
    static_assert(kHostInputPlayerCount == 2);
    using mojorecomp::input::MergeDigitalAxis;

    int16_t leftX = 12345;
    int16_t leftY = -23456;
    MergeDigitalAxis(leftX, false, false);
    MergeDigitalAxis(leftY, false, false);
    if (leftX != 12345 || leftY != -23456)
        return 1;

    MergeDigitalAxis(leftX, true, false);
    if (leftX != std::numeric_limits<int16_t>::min())
        return 2;
    MergeDigitalAxis(leftX, false, true);
    if (leftX != std::numeric_limits<int16_t>::max())
        return 3;
    MergeDigitalAxis(leftX, true, true);
    if (leftX != 0)
        return 4;

    std::puts("PASS: neutral keyboard preserves physical analog axes");
    return 0;
}
