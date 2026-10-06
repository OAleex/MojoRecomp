#pragma once

#include <cstdint>

// Win32 window seam for Vulkan presentation. The dedicated owner thread pumps
// messages and publishes resize state without exposing Win32 types to callers.
bool HostWindow_Init(uint32_t width, uint32_t height, bool hidden, bool fullscreen = false);
void HostWindow_Pump();
void HostWindow_Shutdown();
void* HostWindow_NativeHandle();
bool HostWindow_Alive();
bool HostWindow_AcceptsInput();
void HostWindow_SetTitle(const wchar_t* title);
bool HostWindow_ConsumeResize(uint64_t& generation, uint32_t& width,
                              uint32_t& height, bool& minimized);
