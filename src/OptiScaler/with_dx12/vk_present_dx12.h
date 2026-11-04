#pragma once

#include <vulkan/vulkan.h>

// Plugin present callbacks for Vulkan games (frame generation off): the swapchain image is copied into a
// D3D12-shared image, the callbacks run on a D3D12 queue fenced against the game's queue, and the result is copied
// back before the present.
namespace VkPresentDx12
{
bool Wanted();
// Adds the transfer usages the round trip needs
void AdjustSwapchainInfo(VkPhysicalDevice pd, VkSwapchainCreateInfoKHR* info);
void SwapchainCreated(VkDevice device, VkPhysicalDevice pd, VkSwapchainKHR swapchain,
                      const VkSwapchainCreateInfoKHR* info);
void SwapchainDestroyed(VkDevice device, VkSwapchainKHR swapchain);
// HUD-less pick without FG (Hudfix_Vk): the swapchain's size and format once the bridge runs
bool HudlessSink(VkExtent2D* extent, VkFormat* format);
// Records the pick's copy into the shared HUD-less image, outside a render pass
bool CaptureHudless(VkCommandBuffer cb, VkImage image, VkImageLayout layout);
// Replaces the present's wait semaphores with the one the copy back signals
void QueuePresent(VkQueue queue, VkPresentInfoKHR* info);
} // namespace VkPresentDx12
