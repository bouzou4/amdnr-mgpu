#pragma once

#include "SysUtils.h"
#include <vulkan/vulkan.h>

namespace MenuOverlayVk
{
void CreateSwapchain(VkDevice device, VkPhysicalDevice pd, VkInstance instance, HWND hwnd,
                     const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                     VkSwapchainKHR* pSwapchain);
void DeviceCreated(VkDevice device, const VkDeviceCreateInfo* pCreateInfo);
bool QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo);
// Family of a queue the game created (DeviceCreated)
bool QueueFamily(VkQueue queue, uint32_t* family);
void DestroyVulkanObjects(bool shutdown);

// The menu stays on its window while that exists, games can have more than one
bool OnOtherWindow(HWND hwnd);
} // namespace MenuOverlayVk
