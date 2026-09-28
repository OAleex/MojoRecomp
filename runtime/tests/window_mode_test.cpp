#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstring>
#include <cstdio>
#include <thread>

#include "../host/window.h"

// HostWindow only needs this timer callback from debug_mode during this isolated
// test. Keeping the stub here avoids pulling the whole runtime into the window gate.
namespace mojorecomp::debug {
void PollHotkeys() {}
}

namespace {

using namespace std::chrono_literals;

int Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    HostWindow_Shutdown();
    return 1;
}

bool WaitForExtent(uint64_t& generation, uint32_t expectedWidth,
                   uint32_t expectedHeight, bool expectedMinimized)
{
    for (int i = 0; i < 100; ++i)
    {
        uint32_t width = 0;
        uint32_t height = 0;
        bool minimized = false;
        if (HostWindow_ConsumeResize(generation, width, height, minimized) &&
            width == expectedWidth && height == expectedHeight &&
            minimized == expectedMinimized)
            return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

} // namespace

int main()
{
    if (!HostWindow_Init(320, 180, true, false))
        return Fail("hidden host window creation failed");

    HWND hwnd = static_cast<HWND>(HostWindow_NativeHandle());
    if (!hwnd || !IsWindow(hwnd))
        return Fail("native HWND is invalid");
    if (IsWindowVisible(hwnd))
        return Fail("headless window unexpectedly became visible");

    uint64_t generation = 0;
    if (!WaitForExtent(generation, 320, 180, false))
        return Fail("initial client extent was not published");

    RECT outer{0, 0, 640, 360};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    if (!AdjustWindowRectEx(&outer, style, FALSE, 0))
        return Fail("AdjustWindowRectEx failed");
    if (!SetWindowPos(hwnd, nullptr, 0, 0, outer.right - outer.left,
                      outer.bottom - outer.top,
                      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
        return Fail("SetWindowPos resize failed");
    if (!WaitForExtent(generation, 640, 360, false))
        return Fail("resized client extent was not published");

    PostMessageW(hwnd, WM_SIZE, SIZE_MINIMIZED, 0);
    if (!WaitForExtent(generation, 0, 0, true))
        return Fail("minimized state was not published");
    PostMessageW(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(640, 360));
    if (!WaitForExtent(generation, 640, 360, false))
        return Fail("restored state was not published");

    const DWORD styleBeforeShortcuts =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    RECT rectBeforeShortcuts{};
    if (!GetWindowRect(hwnd, &rectBeforeShortcuts))
        return Fail("GetWindowRect before shortcut test failed");

    PostMessageW(hwnd, WM_KEYDOWN, VK_F11, 0);
    constexpr LPARAM kAltDown = static_cast<LPARAM>(1ull << 29);
    PostMessageW(hwnd, WM_SYSKEYDOWN, VK_RETURN, kAltDown);
    std::this_thread::sleep_for(50ms);

    const DWORD styleAfterShortcuts =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    RECT rectAfterShortcuts{};
    if (!GetWindowRect(hwnd, &rectAfterShortcuts))
        return Fail("GetWindowRect after shortcut test failed");
    if (styleAfterShortcuts != styleBeforeShortcuts ||
        std::memcmp(&rectAfterShortcuts, &rectBeforeShortcuts, sizeof(RECT)) != 0)
        return Fail("F11 or Alt+Enter changed the runtime display mode");

    HostWindow_Shutdown();
    std::puts("OK: hidden resize/minimize/restore passed and runtime fullscreen shortcuts are disabled.");
    return 0;
}
