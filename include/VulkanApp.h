// VulkanApp.h
#pragma once
#include <vulkan/vulkan.h>
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
    uint32_t imageCount = 3;
    uint32_t minImageCount = 2;
    std::vector<VkFramebuffer> swapchainFramebuffers;
    std::vector<VkFence> imagesInFlight;

    bool rebuildSwapchain(ANativeWindow* window);
    void cleanupSwapchainResources();
    void cleanupFrameData();
    bool createFramebuffers();
    bool createFrameData();
    bool handleWindowResize(ANativeWindow* window);
    
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
    uint32_t maxFramesInFlight = 2; // 限制并行帧数
    
    bool swapchainRebuildRequired = false;

    size_t currentFrame = 0;

    bool init(ANativeWindow* window);
    void cleanup();
};

