#pragma once

#include "debug_mode.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace mojorecomp::debug {

struct DebugOverlayText
{
    bool visible = false;
    std::array<char, 128> left{};
    std::array<std::array<char, 128>, 2> right{};
    uint32_t rightCount = 0;
};

inline DebugOverlayText BuildOverlayText(const DebugOverlaySnapshot& snapshot)
{
    DebugOverlayText text{};
    if (!snapshot.enabled && !snapshot.showPerformance && !snapshot.notification[0])
        return text;

    text.visible = true;
    if (snapshot.showPerformance && snapshot.runtimeLabel[0])
    {
        if (snapshot.enabled)
            std::snprintf(text.left.data(), text.left.size(), "%s | DEBUG MODE",
                          snapshot.runtimeLabel.data());
        else
            std::snprintf(text.left.data(), text.left.size(), "%s",
                          snapshot.runtimeLabel.data());
    }
    else if (snapshot.enabled)
    {
        std::snprintf(text.left.data(), text.left.size(), "DEBUG MODE");
    }

    if (snapshot.notification[0] && text.rightCount < text.right.size())
    {
        std::snprintf(text.right[text.rightCount].data(),
                      text.right[text.rightCount].size(), "%s",
                      snapshot.notification.data());
        ++text.rightCount;
    }

    if (snapshot.showPerformance && text.rightCount < text.right.size())
    {
        auto& line = text.right[text.rightCount++];
        const int written = std::snprintf(
            line.data(), line.size(), "%.1f FPS | %.2f ms | %.2fx",
            snapshot.fps, snapshot.frameMs, snapshot.speed);
        size_t used = written > 0
            ? static_cast<size_t>(written) < line.size()
                ? static_cast<size_t>(written)
                : line.size() - 1u
            : 0u;
        auto append = [&](const char* suffix) {
            if (used >= line.size() - 1u)
                return;
            const int added = std::snprintf(line.data() + used, line.size() - used,
                                            "%s", suffix);
            if (added > 0)
                used += std::min(static_cast<size_t>(added),
                                 line.size() - used - 1u);
        };
        if (snapshot.fastForward)
            append(" | FAST");
        if (snapshot.paused)
            append(" | PAUSED");
    }
    return text;
}

} // namespace mojorecomp::debug
