#pragma once

#include <cstdint>

namespace mojorecomp::gpu {

enum class VulkanAdapterClass : uint8_t
{
    Other,
    Integrated,
    Discrete,
    Virtual,
    Cpu,
};

struct VulkanAdapterCapabilities
{
    bool graphicsPresent = false;
    bool swapchain = false;
    bool api13 = false;
    bool shaderInt64 = false;
    bool bufferDeviceAddress = false;
    bool runtimeDescriptorArray = false;
    bool dynamicRendering = false;
    bool sampledImageArrayDynamicIndexing = false;
    bool descriptorLimits = false;
    bool extentSupported = true;
};

inline VulkanAdapterClass VulkanAdapterClassFor(uint32_t vulkanDeviceType)
{
    // VkPhysicalDeviceType values are stable Vulkan ABI constants. Keeping the
    // policy header independent of Vulkan headers lets pure policy tests stay
    // toolchain-only while both Vulkan callers share the same classification.
    switch (vulkanDeviceType)
    {
        case 2: return VulkanAdapterClass::Discrete;
        case 1: return VulkanAdapterClass::Integrated;
        case 3: return VulkanAdapterClass::Virtual;
        case 4: return VulkanAdapterClass::Cpu;
        default: return VulkanAdapterClass::Other;
    }
}

template<typename Limits>
inline bool VulkanDescriptorLimitsAreRendererCompatible(
    const Limits& limits, uint32_t textureSlots)
{
    return limits.maxBoundDescriptorSets >= 4 &&
           limits.maxPerStageDescriptorSampledImages >= 3 * textureSlots &&
           limits.maxDescriptorSetSampledImages >= 3 * textureSlots &&
           limits.maxPerStageDescriptorSamplers >= textureSlots &&
           limits.maxDescriptorSetSamplers >= textureSlots &&
           limits.maxPerStageResources >= 4 * textureSlots;
}

template<typename Limits>
inline VulkanAdapterCapabilities VulkanAdapterCapabilitiesFor(
    bool graphicsPresent, bool swapchain, bool api13, bool shaderInt64,
    bool bufferDeviceAddress, bool runtimeDescriptorArray,
    bool dynamicRendering, bool sampledImageArrayDynamicIndexing,
    const Limits& limits, bool extentSupported,
    uint32_t textureSlots)
{
    return {
        graphicsPresent,
        swapchain,
        api13,
        shaderInt64,
        bufferDeviceAddress,
        runtimeDescriptorArray,
        dynamicRendering,
        sampledImageArrayDynamicIndexing,
        VulkanDescriptorLimitsAreRendererCompatible(limits, textureSlots),
        extentSupported,
    };
}

inline bool VulkanAdapterIsRendererCompatible(
    const VulkanAdapterCapabilities& caps)
{
    return caps.graphicsPresent &&
           caps.swapchain &&
           caps.api13 &&
           caps.shaderInt64 &&
           caps.bufferDeviceAddress &&
           caps.runtimeDescriptorArray &&
           caps.dynamicRendering &&
           caps.sampledImageArrayDynamicIndexing &&
           caps.descriptorLimits &&
           caps.extentSupported;
}

inline uint32_t VulkanAdapterPreference(VulkanAdapterClass deviceClass)
{
    switch (deviceClass)
    {
        case VulkanAdapterClass::Discrete: return 400;
        case VulkanAdapterClass::Integrated: return 300;
        case VulkanAdapterClass::Virtual: return 200;
        case VulkanAdapterClass::Other: return 100;
        case VulkanAdapterClass::Cpu: return 0;
    }
    return 0;
}

} // namespace mojorecomp::gpu
