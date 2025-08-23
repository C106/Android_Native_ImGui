// VulkanApp.h
#pragma once
#include <android/native_window.h>
#include "vk-bootstrap/VkBootstrap.h"
struct FrameData {
    VkCommandPool   cmdPool{};
    VkCommandBuffer cmd{};
    VkFramebuffer   framebuffer{};
    VkSemaphore     imageAvailable{};
    VkSemaphore     renderFinished{};
    VkFence         inFlight{};
};
struct VulkanApp {
    VkInstance instance{};
    VkDevice device{};
    VkPhysicalDevice physicalDevice{};
    VkSurfaceKHR surface{};
    VkSwapchainKHR swapchain{};
    VkQueue graphicsQueue{};
    uint32_t graphicsQueueFamily{};
    VkRenderPass renderPass{};
    VkDescriptorPool descriptorPool{};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    std::vector<FrameData> frames;
    uint32_t imageCount = 0;
    uint32_t minImageCount = 0;

    bool init(ANativeWindow* window);
    void cleanup();
};

