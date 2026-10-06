#include "window.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "../kernel/klog.h"
#include "../debug_mode.h"

namespace {

std::atomic<HWND> g_window{nullptr};
HINSTANCE g_instance = nullptr;
std::atomic<bool> g_alive{false};
std::atomic<bool> g_shutdownRequested{false};
constexpr wchar_t kWindowClass[] = L"MojoRecompVulkanWindow";
constexpr UINT kApplyTitleMessage = WM_APP + 0x4D43;
constexpr UINT_PTR kDebugTimerId = 0x4D43;
std::mutex g_titleMutex;
std::wstring g_pendingTitle;
std::thread g_windowThread;
std::mutex g_windowInitMutex;
std::condition_variable g_windowInitCv;
bool g_windowInitDone = false;
bool g_windowInitOk = false;
std::atomic<uint32_t> g_clientWidth{0};
std::atomic<uint32_t> g_clientHeight{0};
std::atomic<uint64_t> g_resizeGeneration{0};
std::atomic<bool> g_minimized{false};
std::atomic<bool> g_fullscreen{false};
RECT g_windowedRect{};
DWORD g_windowedStyle = WS_OVERLAPPEDWINDOW;
bool g_haveWindowedRect = false;

void PublishClientExtent(HWND hwnd, bool minimized)
{
    RECT client{};
    uint32_t width = 0;
    uint32_t height = 0;
    if (!minimized && GetClientRect(hwnd, &client))
    {
        width = static_cast<uint32_t>(std::max<LONG>(0, client.right - client.left));
        height = static_cast<uint32_t>(std::max<LONG>(0, client.bottom - client.top));
    }
    g_clientWidth.store(width, std::memory_order_release);
    g_clientHeight.store(height, std::memory_order_release);
    g_minimized.store(minimized || width == 0 || height == 0, std::memory_order_release);
    g_resizeGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void ApplyFullscreen(HWND hwnd, bool fullscreen)
{
    if (fullscreen == g_fullscreen.load(std::memory_order_acquire))
        return;

    if (fullscreen)
    {
        g_windowedStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        g_haveWindowedRect = GetWindowRect(hwnd, &g_windowedRect) != FALSE;
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(monitor, &info))
            return;
        // Keep visibility controlled by ShowWindow so a headless hardware or
        // renderer probe never becomes visible merely because fullscreen was
        // selected in a config file.
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP,
                     info.rcMonitor.left, info.rcMonitor.top,
                     info.rcMonitor.right - info.rcMonitor.left,
                     info.rcMonitor.bottom - info.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        g_fullscreen.store(true, std::memory_order_release);
        KLOG("host window mode: fullscreen %ldx%ld\n",
             info.rcMonitor.right - info.rcMonitor.left,
             info.rcMonitor.bottom - info.rcMonitor.top);
    }
    else
    {
        SetWindowLongPtrW(hwnd, GWL_STYLE,
                          g_windowedStyle ? g_windowedStyle : WS_OVERLAPPEDWINDOW);
        const RECT restore = g_haveWindowedRect ? g_windowedRect : RECT{100, 100, 1380, 820};
        SetWindowPos(hwnd, nullptr, restore.left, restore.top,
                     restore.right - restore.left, restore.bottom - restore.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
        g_fullscreen.store(false, std::memory_order_release);
        KLOG("host window mode: windowed\n");
    }
    PublishClientExtent(hwnd, false);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
        case WM_CLOSE:
        {
            const bool shutdownRequested = g_shutdownRequested.load(std::memory_order_acquire);
            DestroyWindow(hwnd);
            if (!shutdownRequested)
            {
                KLOG("host window closed by user; terminating runtime\n");
                ExitProcess(0);
            }
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, kDebugTimerId);
            g_alive.store(false, std::memory_order_release);
            if (g_window.load(std::memory_order_acquire) == hwnd)
                g_window.store(nullptr, std::memory_order_release);
            PostQuitMessage(0);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            PublishClientExtent(hwnd, wparam == SIZE_MINIMIZED);
            return 0;
        case WM_TIMER:
            if (wparam == kDebugTimerId)
            {
                // This timer belongs to the Win32 window thread, not to guest
                // execution. Debug pause can therefore freeze guest time and
                // still receive F6/F7/F8 plus keep the window responsive.
                mojorecomp::debug::PollHotkeys();
                return 0;
            }
            break;
        case kApplyTitleMessage:
        {
            std::wstring title;
            {
                std::lock_guard lock(g_titleMutex);
                title = g_pendingTitle;
            }
            if (!title.empty())
                SetWindowTextW(hwnd, title.c_str());
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void WindowThreadMain(uint32_t width, uint32_t height, bool hidden, bool fullscreen)
{
    g_instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = g_instance;
    wc.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(101));
    wc.hIconSm = LoadIconW(g_instance, MAKEINTRESOURCEW(101));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    constexpr DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRectEx(&rect, style, FALSE, 0);

    HWND window = CreateWindowExW(
        0, kWindowClass, L"MojoRecomp - Crash of the Titans",
        style, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, g_instance, nullptr);

    {
        std::lock_guard lock(g_windowInitMutex);
        g_window.store(window, std::memory_order_release);
        g_windowInitOk = window != nullptr;
        g_windowInitDone = true;
    }
    g_windowInitCv.notify_all();

    if (!window)
    {
        KLOG("host window creation failed: win32=%lu\n", GetLastError());
        return;
    }

    g_alive.store(true, std::memory_order_release);
    SetTimer(window, kDebugTimerId, 8, nullptr);
    PublishClientExtent(window, false);
    if (fullscreen)
        ApplyFullscreen(window, true);
    if (!hidden)
    {
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
    }

    KLOG("host window ready: hwnd=%p client=%ux%u hidden=%u ownerTid=%lu\n",
         static_cast<void*>(window), width, height, hidden ? 1u : 0u,
         static_cast<unsigned long>(GetCurrentThreadId()));

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

} // namespace

bool HostWindow_Init(uint32_t width, uint32_t height, bool hidden, bool fullscreen)
{
    static std::once_flag shutdownHook;
    std::call_once(shutdownHook, [] { std::atexit(HostWindow_Shutdown); });

    if (g_window.load(std::memory_order_acquire))
        return true;

    if (g_windowThread.joinable())
        g_windowThread.join();

    {
        std::lock_guard lock(g_windowInitMutex);
        g_windowInitDone = false;
        g_windowInitOk = false;
    }
    g_fullscreen.store(false, std::memory_order_release);
    g_clientWidth.store(width, std::memory_order_release);
    g_clientHeight.store(height, std::memory_order_release);
    g_minimized.store(false, std::memory_order_release);
    g_shutdownRequested.store(false, std::memory_order_release);
    g_resizeGeneration.store(0, std::memory_order_release);
    g_haveWindowedRect = false;
    g_windowThread = std::thread(WindowThreadMain, width, height, hidden, fullscreen);

    std::unique_lock lock(g_windowInitMutex);
    g_windowInitCv.wait(lock, [] { return g_windowInitDone; });
    return g_windowInitOk;
}

void HostWindow_Pump()
{
    // The dedicated window thread owns and continuously pumps Win32 messages.
    // Kept as a compatibility no-op for the renderer's existing swap seam.
}

void HostWindow_Shutdown()
{
    g_shutdownRequested.store(true, std::memory_order_release);
    const HWND window = g_window.load(std::memory_order_acquire);
    if (window)
        PostMessageW(window, WM_CLOSE, 0, 0);
    if (g_windowThread.joinable() &&
        g_windowThread.get_id() != std::this_thread::get_id())
        g_windowThread.join();
    g_window.store(nullptr, std::memory_order_release);
    g_alive.store(false, std::memory_order_release);
}

void* HostWindow_NativeHandle()
{
    return static_cast<void*>(g_window.load(std::memory_order_acquire));
}

bool HostWindow_Alive()
{
    return g_alive.load(std::memory_order_acquire) &&
           g_window.load(std::memory_order_acquire) != nullptr;
}

bool HostWindow_AcceptsInput()
{
    const HWND window = g_window.load(std::memory_order_acquire);
    if (!window || g_minimized.load(std::memory_order_acquire))
        return false;
    return GetForegroundWindow() == window;
}

void HostWindow_SetTitle(const wchar_t* title)
{
    const HWND window = g_window.load(std::memory_order_acquire);
    if (!window || !title || !*title)
        return;

    // SetWindowTextW uses a synchronous window message when called from a
    // different thread. The title's input path can run on a guest worker while
    // the Win32 window is owned/pumped elsewhere, so doing that directly can
    // deadlock the whole game. Publish the desired title and let WindowProc
    // apply it asynchronously on the owner thread instead.
    {
        std::lock_guard lock(g_titleMutex);
        g_pendingTitle.assign(title);
    }
    PostMessageW(window, kApplyTitleMessage, 0, 0);
}

bool HostWindow_ConsumeResize(uint64_t& generation, uint32_t& width,
                              uint32_t& height, bool& minimized)
{
    const uint64_t current = g_resizeGeneration.load(std::memory_order_acquire);
    if (current == generation)
        return false;
    width = g_clientWidth.load(std::memory_order_acquire);
    height = g_clientHeight.load(std::memory_order_acquire);
    minimized = g_minimized.load(std::memory_order_acquire);
    generation = current;
    return true;
}
