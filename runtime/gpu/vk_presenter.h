#pragma once

#include <cstdint>

struct Pm4Draw;

bool VkPresenter_Init(void* nativeWindow, uint32_t width, uint32_t height);
bool VkPresenter_Active();
struct VkPresenterExtentInfo {
    uint32_t logicalWidth = 0;
    uint32_t logicalHeight = 0;
    uint32_t internalWidth = 0;
    uint32_t internalHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint32_t resolutionScale = 1;
    int32_t contentX = 0;
    int32_t contentY = 0;
    uint32_t contentWidth = 0;
    uint32_t contentHeight = 0;
    float sceneScaleX = 1.0f;
    float sceneScaleY = 1.0f;
    float uiScaleX = 1.0f;
    float uiScaleY = 1.0f;
};
bool VkPresenter_GetExtentInfo(VkPresenterExtentInfo& out);
bool VkPresenter_SetOutputExtent(uint32_t width, uint32_t height);
bool VkPresenter_ValidateConfiguredPostProcess();
bool VkPresenter_Draw(uint8_t* guestBase, const Pm4Draw& draw,
                      const uint32_t* registers, uint64_t vsHash, uint64_t psHash);
bool VkPresenter_Present(uint32_t frontBuffer, uint32_t width, uint32_t height);
void VkPresenter_Shutdown();
uint64_t VkPresenter_FrameCount();
uint64_t VkPresenter_DrawCount();
