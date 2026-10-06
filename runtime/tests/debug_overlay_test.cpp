#include "../debug_mode.h"
#include "../debug_hotkey_sequence.h"
#include "../debug_overlay_layout.h"

#include <cstdio>
#include <string_view>

int main()
{
    using mojorecomp::debug::BuildOverlayText;
    using mojorecomp::debug::DebugActivationSequence;
    using mojorecomp::debug::DebugOverlaySnapshot;
    using mojorecomp::debug::RegisterDebugActivationPress;

    DebugActivationSequence sequence{};
    sequence.lastPressMs = -1;
    for (int i = 0; i < 9; ++i)
    {
        if (RegisterDebugActivationPress(sequence, i * 100))
            return 10;
    }
    if (!RegisterDebugActivationPress(sequence, 900))
        return 11;

    sequence = {};
    sequence.lastPressMs = -1;
    for (int i = 0; i < 5; ++i)
    {
        if (RegisterDebugActivationPress(sequence, i * 100))
            return 12;
    }
    if (RegisterDebugActivationPress(sequence, 1400))
        return 13;
    for (int i = 1; i < 9; ++i)
    {
        if (RegisterDebugActivationPress(sequence, 1400 + i * 100))
            return 14;
    }
    if (!RegisterDebugActivationPress(sequence, 2300))
        return 15;

    DebugOverlaySnapshot snapshot{};
    auto text = BuildOverlayText(snapshot);
    if (text.visible || text.rightCount != 0)
        return 1;

    snapshot.showPerformance = true;
    snapshot.speed = 1.0;
    snapshot.fps = 30.0;
    snapshot.frameMs = 33.333;
    std::snprintf(snapshot.runtimeLabel.data(), snapshot.runtimeLabel.size(),
                  "%s", "Crash of the Titans - 0.2.0-alpha Runtime");
    text = BuildOverlayText(snapshot);
    if (!text.visible ||
        std::string_view(text.left.data()) != "Crash of the Titans - 0.2.0-alpha Runtime" ||
        text.rightCount != 1 ||
        std::string_view(text.right[0].data()) !=
            "30.0 FPS | 33.33 ms | 1.00x")
        return 2;

    snapshot.enabled = true;
    snapshot.fastForward = true;
    snapshot.paused = true;
    std::snprintf(snapshot.notification.data(), snapshot.notification.size(),
                  "%s", "All episodes unlocked");
    text = BuildOverlayText(snapshot);
    if (std::string_view(text.left.data()) !=
            "Crash of the Titans - 0.2.0-alpha Runtime | DEBUG MODE" ||
        text.rightCount != 2 ||
        std::string_view(text.right[0].data()) != "All episodes unlocked" ||
        std::string_view(text.right[1].data()) !=
            "30.0 FPS | 33.33 ms | 1.00x | FAST | PAUSED")
        return 3;

    snapshot.showPerformance = false;
    text = BuildOverlayText(snapshot);
    if (!text.visible || std::string_view(text.left.data()) != "DEBUG MODE" ||
        text.rightCount != 1 ||
        std::string_view(text.right[0].data()) != "All episodes unlocked")
        return 4;

    snapshot.notification[0] = '\0';
    text = BuildOverlayText(snapshot);
    if (!text.visible || text.rightCount != 0)
        return 5;

    std::puts("PASS: debug overlay visibility, F9, state, and notification layout");
    return 0;
}
