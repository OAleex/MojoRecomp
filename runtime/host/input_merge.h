#pragma once

#include <cstdint>
#include <limits>

namespace mojorecomp::input {

// Merge one pair of digital keyboard directions into an already-populated
// physical controller axis. A neutral keyboard must leave the physical value
// untouched; otherwise polling the keyboard after SDL/XInput would erase an
// analog stick simply because no keyboard key is held.
inline void MergeDigitalAxis(int16_t& axis, bool negative, bool positive)
{
    if (!negative && !positive)
        return;
    if (negative == positive)
    {
        axis = 0;
        return;
    }
    axis = negative ? std::numeric_limits<int16_t>::min()
                    : std::numeric_limits<int16_t>::max();
}

} // namespace mojorecomp::input
