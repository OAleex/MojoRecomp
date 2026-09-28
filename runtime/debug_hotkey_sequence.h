#pragma once

#include <cstdint>

namespace mojorecomp::debug {

struct DebugActivationSequence
{
    uint32_t presses = 0;
    int64_t lastPressMs = -1;
};

inline bool RegisterDebugActivationPress(DebugActivationSequence& sequence,
                                         int64_t nowMs) noexcept
{
    constexpr uint32_t kRequiredPresses = 10;
    constexpr int64_t kTimeoutMs = 1000;

    if (sequence.lastPressMs < 0 || nowMs - sequence.lastPressMs >= kTimeoutMs)
        sequence.presses = 1;
    else
        ++sequence.presses;
    sequence.lastPressMs = nowMs;

    if (sequence.presses < kRequiredPresses)
        return false;

    sequence = {};
    sequence.lastPressMs = -1;
    return true;
}

} // namespace mojorecomp::debug
