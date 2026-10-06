#include "hardware_probe.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../host/window.h"
#include "texture_abi.h"
#include "vulkan_adapter_policy.h"

namespace mojorecomp::gpu {
namespace {

std::string EscapeJson(const char* text)
{
    std::string out;
    for (const unsigned char c : std::string(text ? text : ""))
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char buffer[7]{};
                    std::snprintf(buffer, sizeof(buffer), "\\u%04X", c);
                    out += buffer;
                }
                else
                    out.push_back(static_cast<char>(c));
                break;
        }
    }
    return out;
}

const char* PresentModeName(VkPresentModeKHR mode)
{
    switch (mode)
    {
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
        case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo_relaxed";
        default: return nullptr;
    }
}

template <typename T>
bool LoadGlobal(HMODULE module, T& target, const char* name)
{
    target = reinterpret_cast<T>(GetProcAddress(module, name));
    return target != nullptr;
}

template <typename T>
bool LoadInstance(PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkInstance instance,
                  T& target, const char* name)
{
    target = reinterpret_cast<T>(getInstanceProcAddr(instance, name));
    return target != nullptr;
}

} // namespace

int RunHardwareProbeJson()
{
    HMODULE vulkan = LoadLibraryW(L"vulkan-1.dll");
    if (!vulkan)
    {
        std::fprintf(stderr, "hardware probe: Vulkan loader not found\n");
        return 1;
    }

    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_vkCreateInstance createInstance = nullptr;
    if (!LoadGlobal(vulkan, getInstanceProcAddr, "vkGetInstanceProcAddr") ||
        !LoadGlobal(vulkan, createInstance, "vkCreateInstance"))
    {
        std::fprintf(stderr, "hardware probe: Vulkan entry points unavailable\n");
        FreeLibrary(vulkan);
        return 1;
    }

    if (!HostWindow_Init(64, 64, true, false))
    {
        std::fprintf(stderr, "hardware probe: could not create hidden native surface window\n");
        FreeLibrary(vulkan);
        return 1;
    }

    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "MojoRecomp Hardware Probe";
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &application;
    instanceInfo.enabledExtensionCount = 2;
    instanceInfo.ppEnabledExtensionNames = extensions;

    VkInstance instance = VK_NULL_HANDLE;
    if (createInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS)
    {
        std::fprintf(stderr, "hardware probe: vkCreateInstance failed\n");
        HostWindow_Shutdown();
        FreeLibrary(vulkan);
        return 1;
    }

    PFN_vkDestroyInstance destroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices enumeratePhysicalDevices = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties enumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 getPhysicalDeviceFeatures2 = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR getSurfaceSupport = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR getPresentModes = nullptr;
    PFN_vkCreateWin32SurfaceKHR createWin32Surface = nullptr;
    PFN_vkDestroySurfaceKHR destroySurface = nullptr;
    const bool loaded =
        LoadInstance(getInstanceProcAddr, instance, destroyInstance, "vkDestroyInstance") &&
        LoadInstance(getInstanceProcAddr, instance, enumeratePhysicalDevices, "vkEnumeratePhysicalDevices") &&
        LoadInstance(getInstanceProcAddr, instance, enumerateDeviceExtensionProperties,
                     "vkEnumerateDeviceExtensionProperties") &&
        LoadInstance(getInstanceProcAddr, instance, getPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties") &&
        LoadInstance(getInstanceProcAddr, instance, getPhysicalDeviceFeatures2, "vkGetPhysicalDeviceFeatures2") &&
        LoadInstance(getInstanceProcAddr, instance, getQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties") &&
        LoadInstance(getInstanceProcAddr, instance, getSurfaceSupport, "vkGetPhysicalDeviceSurfaceSupportKHR") &&
        LoadInstance(getInstanceProcAddr, instance, getPresentModes, "vkGetPhysicalDeviceSurfacePresentModesKHR") &&
        LoadInstance(getInstanceProcAddr, instance, createWin32Surface, "vkCreateWin32SurfaceKHR") &&
        LoadInstance(getInstanceProcAddr, instance, destroySurface, "vkDestroySurfaceKHR");
    if (!loaded)
    {
        std::fprintf(stderr, "hardware probe: required Vulkan instance entry points unavailable\n");
        if (destroyInstance) destroyInstance(instance, nullptr);
        HostWindow_Shutdown();
        FreeLibrary(vulkan);
        return 1;
    }

    VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.hinstance = GetModuleHandleW(nullptr);
    surfaceInfo.hwnd = static_cast<HWND>(HostWindow_NativeHandle());
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (createWin32Surface(instance, &surfaceInfo, nullptr, &surface) != VK_SUCCESS)
    {
        std::fprintf(stderr, "hardware probe: could not create Vulkan surface\n");
        destroyInstance(instance, nullptr);
        HostWindow_Shutdown();
        FreeLibrary(vulkan);
        return 1;
    }

    uint32_t physicalCount = 0;
    enumeratePhysicalDevices(instance, &physicalCount, nullptr);
    std::vector<VkPhysicalDevice> physicalDevices(physicalCount);
    if (!physicalCount ||
        enumeratePhysicalDevices(instance, &physicalCount, physicalDevices.data()) != VK_SUCCESS)
    {
        std::fprintf(stderr, "hardware probe: no Vulkan adapters found\n");
        destroySurface(instance, surface, nullptr);
        destroyInstance(instance, nullptr);
        HostWindow_Shutdown();
        FreeLibrary(vulkan);
        return 1;
    }

    VkPhysicalDevice selected = VK_NULL_HANDLE;
    uint32_t bestPreference = 0;
    bool haveCompatibleAdapter = false;
    for (VkPhysicalDevice candidate : physicalDevices)
    {
        VkPhysicalDeviceProperties candidateProperties{};
        getPhysicalDeviceProperties(candidate, &candidateProperties);

        bool graphicsPresent = false;
        uint32_t queueCount = 0;
        getQueueFamilyProperties(candidate, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        getQueueFamilyProperties(candidate, &queueCount, queues.data());
        for (uint32_t q = 0; q < queueCount; ++q)
        {
            VkBool32 present = VK_FALSE;
            getSurfaceSupport(candidate, q, surface, &present);
            if ((queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
            {
                graphicsPresent = true;
                break;
            }
        }

        bool hasSwapchain = false;
        uint32_t extensionCount = 0;
        if (enumerateDeviceExtensionProperties(
                candidate, nullptr, &extensionCount, nullptr) == VK_SUCCESS &&
            extensionCount)
        {
            std::vector<VkExtensionProperties> extensions(extensionCount);
            if (enumerateDeviceExtensionProperties(
                    candidate, nullptr, &extensionCount, extensions.data()) == VK_SUCCESS)
            {
                for (const auto& extension : extensions)
                {
                    if (std::strcmp(extension.extensionName,
                                    VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
                    {
                        hasSwapchain = true;
                        break;
                    }
                }
            }
        }

        VkPhysicalDeviceVulkan12Features candidate12{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features candidate13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        candidate13.pNext = &candidate12;
        VkPhysicalDeviceFeatures2 candidateFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        candidateFeatures.pNext = &candidate13;
        getPhysicalDeviceFeatures2(candidate, &candidateFeatures);

        const VulkanAdapterCapabilities caps = VulkanAdapterCapabilitiesFor(
            graphicsPresent,
            hasSwapchain,
            candidateProperties.apiVersion >= VK_API_VERSION_1_3,
            candidateFeatures.features.shaderInt64 == VK_TRUE,
            candidate12.bufferDeviceAddress == VK_TRUE,
            candidate12.runtimeDescriptorArray == VK_TRUE,
            candidate13.dynamicRendering == VK_TRUE,
            candidateFeatures.features.shaderSampledImageArrayDynamicIndexing == VK_TRUE,
            candidateProperties.limits,
            true,
            mojorecomp::texture_abi::kSlots);

        const VulkanAdapterClass deviceClass =
            VulkanAdapterClassFor(uint32_t(candidateProperties.deviceType));

        const bool compatible = VulkanAdapterIsRendererCompatible(caps);
        const uint32_t preference = VulkanAdapterPreference(deviceClass);
        if (!compatible || (haveCompatibleAdapter && preference <= bestPreference))
            continue;

        selected = candidate;
        bestPreference = preference;
        haveCompatibleAdapter = true;
    }

    if (!selected)
    {
        std::fprintf(stderr, "hardware probe: no renderer-compatible Vulkan adapter found\n");
        destroySurface(instance, surface, nullptr);
        destroyInstance(instance, nullptr);
        HostWindow_Shutdown();
        FreeLibrary(vulkan);
        return 1;
    }

    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    getPhysicalDeviceProperties(selected, &properties);
    getPhysicalDeviceFeatures2(selected, &features);

    uint32_t modeCount = 0;
    getPresentModes(selected, surface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    if (modeCount)
        getPresentModes(selected, surface, &modeCount, modes.data());
    std::vector<std::string> modeNames;
    for (const VkPresentModeKHR mode : modes)
    {
        if (const char* name = PresentModeName(mode))
        {
            if (std::find(modeNames.begin(), modeNames.end(), name) == modeNames.end())
                modeNames.emplace_back(name);
        }
    }

    std::printf("{\n");
    std::printf("  \"adapter\": \"%s\",\n", EscapeJson(properties.deviceName).c_str());
    std::printf("  \"vendor_id\": %u,\n", properties.vendorID);
    std::printf("  \"device_id\": %u,\n", properties.deviceID);
    std::printf("  \"vulkan_api\": \"%u.%u.%u\",\n",
                VK_VERSION_MAJOR(properties.apiVersion),
                VK_VERSION_MINOR(properties.apiVersion),
                VK_VERSION_PATCH(properties.apiVersion));
    std::printf("  \"sampler_anisotropy\": %s,\n",
                features.features.samplerAnisotropy ? "true" : "false");
    std::printf("  \"max_anisotropy\": %.0f,\n",
                features.features.samplerAnisotropy
                    ? properties.limits.maxSamplerAnisotropy : 1.0f);
    std::printf("  \"bc_texture_compression\": %s,\n",
                features.features.textureCompressionBC ? "true" : "false");
    std::printf("  \"supported_present_modes\": [");
    for (size_t i = 0; i < modeNames.size(); ++i)
        std::printf("%s\"%s\"", i ? ", " : "", modeNames[i].c_str());
    std::printf("],\n");
    std::printf("  \"max_internal_extent\": [%u, %u]\n",
                properties.limits.maxImageDimension2D,
                properties.limits.maxImageDimension2D);
    std::printf("}\n");

    destroySurface(instance, surface, nullptr);
    destroyInstance(instance, nullptr);
    HostWindow_Shutdown();
    FreeLibrary(vulkan);
    return 0;
}

} // namespace mojorecomp::gpu
