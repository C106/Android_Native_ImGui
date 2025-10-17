#include "VulkanApp.h"
#include "Utils.h"
#include "vk-bootstrap/VkBootstrap.h"

#include <vector>


extern VkExtent2D gSwapchainExtent; // 方便给 frame_render 用


bool VulkanApp::init(ANativeWindow* window) {
    // 1. 创建 Instance - 修复验证层问题
    vkb::InstanceBuilder builder;
    
    // 在 Android 上验证层可能不可用，特别是在 release 构建中
    builder.enable_extension(VK_KHR_SURFACE_EXTENSION_NAME)
           .enable_extension(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)
           .set_app_name("ImGuiVulkanAndroid")
           .require_api_version(1, 1, 0);
    
    // 只在 debug 构建时启用验证层
    #ifdef DEBUG
    if (builder.check_validation_layer_support()) {
        builder.use_default_debug_messenger()
               .request_validation_layers();
    }
    #endif
    
    auto inst_ret = builder.build();
    if (!inst_ret) {
        __android_log_print(ANDROID_LOG_ERROR, "VulkanApp", 
                          "Failed to create instance: %s", inst_ret.error().message().c_str());
        return false;
    }
    auto vkb_inst = inst_ret.value();
    instance = vkb_inst.instance;   
        
    // 创建 Android 表面
    VkAndroidSurfaceCreateInfoKHR surface_info = {};
    surface_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    surface_info.window = window;
    VkResult result = vkCreateAndroidSurfaceKHR(instance, &surface_info, nullptr, &surface);
    if (result != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, "VulkanApp", 
                          "Failed to create Android surface: %d", result);
        return false;
    }
    
    // 2. 选择 PhysicalDevice - 添加更灵活的选择
    vkb::PhysicalDeviceSelector selector{ vkb_inst };
    selector.add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    
    auto phys_ret = selector.set_surface(surface)
                           .set_minimum_version(1, 1)  // 确保最低版本
                           .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)  // 优先独显
                           .allow_any_gpu_device_type(true)  // 但允许任何类型
                           .select();
    if (!phys_ret) {
        __android_log_print(ANDROID_LOG_ERROR, "VulkanApp", 
                          "Failed to select physical device: %s", phys_ret.error().message().c_str());
        return false;
    }
    auto phys = phys_ret.value();
    physicalDevice = phys.physical_device;

    // 3. 创建 Device
    vkb::DeviceBuilder dev_builder{ phys };
    auto dev_ret = dev_builder.build();
    if (!dev_ret) {
        __android_log_print(ANDROID_LOG_ERROR, "VulkanApp", 
                          "Failed to create device: %s", dev_ret.error().message().c_str());
        return false;
    }
    auto vkb_dev = dev_ret.value();
    device = vkb_dev.device;
    
    auto graphics_queue_ret = vkb_dev.get_queue(vkb::QueueType::graphics);
    if (!graphics_queue_ret) {
        __android_log_print(ANDROID_LOG_ERROR, "VulkanApp", "Failed to get graphics queue");
        return false;
    }
    graphicsQueue = graphics_queue_ret.value();
    graphicsQueueFamily = vkb_dev.get_queue_index(vkb::QueueType::graphics).value();

    // 4. Surface + Swapchain - 修复格式和呈现模式选择
    vkb::SwapchainBuilder swapchain_builder{ vkb_dev };
    
    // 获取窗口尺寸，处理可能的 0 尺寸
    int32_t width = ANativeWindow_getWidth(window);
    int32_t height = ANativeWindow_getHeight(window);
    if (width <= 0) width = 1;
    if (height <= 0) height = 1;
    
    auto swap_ret = swapchain_builder.set_old_swapchain(VK_NULL_HANDLE)
                                     // 使用更通用的格式选择
                                     .add_fallback_format({ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
                                     .add_fallback_format({ VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
                                     // 使用更兼容的呈现模式
                                     .add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                                     .add_fallback_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
                                     .add_fallback_present_mode(VK_PRESENT_MODE_IMMEDIATE_KHR)
                                     .set_desired_extent(width, height)
                                     .set_pre_transform_flags(VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                                     .set_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
                                     .build();
    if (!swap_ret) {
        __android_log_print(ANDROID_LOG_ERROR, "VulkanApp", 
                          "Failed to create swapchain: %s", swap_ret.error().message().c_str());
        return false;
    }
    auto vkb_swap = swap_ret.value();

    swapchain = vkb_swap.swapchain;
    swapchainImages = vkb_swap.get_images().value();
    swapchainImageViews = vkb_swap.get_image_views().value();
    gSwapchainExtent = vkb_swap.extent;
    imageCount = (uint32_t)swapchainImages.size();
    imagesInFlight.resize(swapchainImages.size(), VK_NULL_HANDLE);
    
    // 5. RenderPass - 添加子通道依赖
    VkAttachmentDescription color_attachment{};
    color_attachment.format = vkb_swap.image_format;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    // 添加子通道依赖以确保正确的同步
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &color_attachment;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dependency;
    
    result = vkCreateRenderPass(device, &rp_info, nullptr, &renderPass);
    if (result != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, "VulkanApp", 
                          "Failed to create render pass: %d", result);
        return false;
    }
    imageAvailableSemaphores.resize(maxFramesInFlight);
    renderFinishedSemaphores.resize(maxFramesInFlight);
    inFlightFences.resize(maxFramesInFlight);
    
    for (size_t i = 0; i < maxFramesInFlight; i++) {
        VkSemaphoreCreateInfo sem_info{};
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        
        VkResult result = vkCreateSemaphore(device, &sem_info, nullptr, &imageAvailableSemaphores[i]);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "VulkanApp", 
                              "Failed to create imageAvailable semaphore %zu: %d", i, result);
            return false;
        }
        
        result = vkCreateSemaphore(device, &sem_info, nullptr, &renderFinishedSemaphores[i]);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "VulkanApp", 
                              "Failed to create renderFinished semaphore %zu: %d", i, result);
            return false;
        }
        
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT; // 关键：必须是 signaled 状态
        
        result = vkCreateFence(device, &fence_info, nullptr, &inFlightFences[i]);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "VulkanApp", 
                              "Failed to create inFlight fence %zu: %d", i, result);
            return false;
        }
    }
    // 6. Framebuffers + per-frame data - 修复帧数据结构
    frames.resize(imageCount);  

    frames.resize(maxFramesInFlight);
    for (uint32_t i = 0; i < maxFramesInFlight; i++) {
        // Command Pool + Buffer
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = graphicsQueueFamily;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        
        VkResult result = vkCreateCommandPool(device, &pool_info, nullptr, &frames[i].cmdPool);
        if (result != VK_SUCCESS) {
            return false;
        }

        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = frames[i].cmdPool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;
        
        result = vkAllocateCommandBuffers(device, &alloc_info, &frames[i].cmd);
        if (result != VK_SUCCESS) {
            return false;
        }
    }

    swapchainFramebuffers.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageView attachments[] = { swapchainImageViews[i] };
        
        VkFramebufferCreateInfo fb_info{};
        fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass = renderPass;
        fb_info.attachmentCount = 1;
        fb_info.pAttachments = attachments;
        fb_info.width = gSwapchainExtent.width;
        fb_info.height = gSwapchainExtent.height;
        fb_info.layers = 1;
        
        VkResult result = vkCreateFramebuffer(device, &fb_info, nullptr, &swapchainFramebuffers[i]);
        if (result != VK_SUCCESS) {
            return false;
        }
    }
    
    imagesInFlight.resize(imageCount, VK_NULL_HANDLE);
    currentFrame = 0;


    // 7. DescriptorPool - 为 ImGui 优化
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
    };
    
    VkDescriptorPoolCreateInfo pool_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1000 * (uint32_t)(sizeof(pool_sizes)/sizeof(pool_sizes[0]));
    pool_info.poolSizeCount = (uint32_t)(sizeof(pool_sizes)/sizeof(pool_sizes[0]));
    pool_info.pPoolSizes = pool_sizes;
    
    result = vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptorPool);
    if (result != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, "VulkanApp", 
                          "Failed to create descriptor pool: %d", result);
        return false;
    }

    __android_log_print(ANDROID_LOG_INFO, "VulkanApp", 
                        "Vulkan initialization successful. Swapchain: %dx%d, Images: %d", 
                        gSwapchainExtent.width, gSwapchainExtent.height, imageCount);
    
    return true;
}

void VulkanApp::cleanup() {
    vkDeviceWaitIdle(device);

    // 销毁 per-frame
    for (auto& f : frames) {
        vkDestroyFence(device, f.inFlight, nullptr);
        vkDestroySemaphore(device, f.imageAvailable, nullptr);
        vkDestroySemaphore(device, f.renderFinished, nullptr);
        vkDestroyCommandPool(device, f.cmdPool, nullptr);
        vkDestroyFramebuffer(device, f.framebuffer, nullptr);
    }

    // 销毁 swapchain 资源
    for (auto view : swapchainImageViews) {
        vkDestroyImageView(device, view, nullptr);
    }
    vkDestroySwapchainKHR(device, swapchain, nullptr);

    vkDestroyRenderPass(device, renderPass, nullptr);
    vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);

    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
}


bool VulkanApp::rebuildSwapchain(ANativeWindow* window) {
    // 等待设备空闲，确保没有正在使用的资源
    vkDeviceWaitIdle(device);
    
    __android_log_print(ANDROID_LOG_INFO, "VulkanApp", "Starting swapchain rebuild");
    
    // 1. 清理旧的 framebuffers 和相关资源
    cleanupSwapchainResources();
    
    // 2. 获取新的窗口尺寸
    int32_t width = ANativeWindow_getWidth(window);
    int32_t height = ANativeWindow_getHeight(window);
    
    // 处理窗口最小化或无效尺寸
    if (width <= 0 || height <= 0) {
        __android_log_print(ANDROID_LOG_WARN, "VulkanApp", 
                          "Invalid window size: %dx%d, waiting for valid size", width, height);
        return false; // 返回 false 但不是错误，调用者应该稍后重试
    }
    
    __android_log_print(ANDROID_LOG_INFO, "VulkanApp", "New window size: %dx%d", width, height);
    
    // 3. 创建新的 swapchain
    // 需要重新获取 vkb::Device 来创建 swapchain builder
    vkb::Device vkb_dev;
    vkb_dev.device = device;
    //vkb_dev.physical_device = physicalDevice;
    vkb_dev.surface = surface;
    //vkb_dev.instance = instance;
    
    vkb::SwapchainBuilder swapchain_builder{ vkb_dev };
    auto swap_ret = swapchain_builder.set_old_swapchain(swapchain) // 重用旧 swapchain
                                     .add_fallback_format({ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
                                     .add_fallback_format({ VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
                                     .add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                                     .add_fallback_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
                                     .add_fallback_present_mode(VK_PRESENT_MODE_IMMEDIATE_KHR)
                                     .set_desired_extent(width, height)
                                     .set_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
                                     .build();
    
    if (!swap_ret) {
        __android_log_print(ANDROID_LOG_ERROR, "VulkanApp", 
                          "Failed to rebuild swapchain: %s", swap_ret.error().message().c_str());
        return false;
    }
    
    // 4. 销毁旧的 swapchain
    if (swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
    }
    
    // 5. 更新 swapchain 相关数据
    auto vkb_swap = swap_ret.value();
    swapchain = vkb_swap.swapchain;
    swapchainImages = vkb_swap.get_images().value();
    swapchainImageViews = vkb_swap.get_image_views().value();
    gSwapchainExtent = vkb_swap.extent;
    
    uint32_t newImageCount = (uint32_t)swapchainImages.size();
    
    __android_log_print(ANDROID_LOG_INFO, "VulkanApp", 
                        "New swapchain created: %dx%d, %d images", 
                        gSwapchainExtent.width, gSwapchainExtent.height, newImageCount);
    
    // 6. 如果图像数量变化，需要调整帧数据
    if (newImageCount != imageCount) {
        __android_log_print(ANDROID_LOG_INFO, "VulkanApp", 
                          "Image count changed from %d to %d, recreating frame data", 
                          imageCount, newImageCount);
        
        // 清理旧的帧数据
        cleanupFrameData();
        
        // 重新创建帧数据
        imageCount = newImageCount;
        frames.resize(imageCount);
        imagesInFlight.resize(imageCount, VK_NULL_HANDLE);
        
        if (!createFrameData()) {
            return false;
        }
    }
    
    // 7. 重新创建 framebuffers
    if (!createFramebuffers()) {
        return false;
    }
    
    __android_log_print(ANDROID_LOG_INFO, "VulkanApp", "Swapchain rebuild completed successfully");
    return true;
}

void VulkanApp::cleanupSwapchainResources() {
    // 清理 framebuffers
    for (auto& frame : frames) {
        if (frame.framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, frame.framebuffer, nullptr);
            frame.framebuffer = VK_NULL_HANDLE;
        }
    }
    
    // 清理 image views
    for (auto imageView : swapchainImageViews) {
        if (imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(device, imageView, nullptr);
        }
    }
    swapchainImageViews.clear();
    swapchainImages.clear();
}

void VulkanApp::cleanupFrameData() {
    for (auto& frame : frames) {
        if (frame.framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, frame.framebuffer, nullptr);
            frame.framebuffer = VK_NULL_HANDLE;
        }
        if (frame.cmdPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, frame.cmdPool, nullptr);
            frame.cmdPool = VK_NULL_HANDLE;
        }
        if (frame.imageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, frame.imageAvailable, nullptr);
            frame.imageAvailable = VK_NULL_HANDLE;
        }
        if (frame.renderFinished != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, frame.renderFinished, nullptr);
            frame.renderFinished = VK_NULL_HANDLE;
        }
        if (frame.inFlight != VK_NULL_HANDLE) {
            vkDestroyFence(device, frame.inFlight, nullptr);
            frame.inFlight = VK_NULL_HANDLE;
        }
    }
    frames.clear();
}

bool VulkanApp::createFrameData() {
    for (uint32_t i = 0; i < imageCount; i++) {
        // Command Pool + Buffer
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = graphicsQueueFamily;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        
        VkResult result = vkCreateCommandPool(device, &pool_info, nullptr, &frames[i].cmdPool);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "VulkanApp", 
                              "Failed to create command pool %d: %d", i, result);
            return false;
        }

        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = frames[i].cmdPool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;
        
        result = vkAllocateCommandBuffers(device, &alloc_info, &frames[i].cmd);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "VulkanApp", 
                              "Failed to allocate command buffer %d: %d", i, result);
            return false;
        }

        // Semaphores + Fence
        VkSemaphoreCreateInfo sem_info{};
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        
        result = vkCreateSemaphore(device, &sem_info, nullptr, &frames[i].imageAvailable);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "VulkanApp", 
                              "Failed to create image available semaphore %d: %d", i, result);
            return false;
        }
        
        result = vkCreateSemaphore(device, &sem_info, nullptr, &frames[i].renderFinished);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "VulkanApp", 
                              "Failed to create render finished semaphore %d: %d", i, result);
            return false;
        }

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        
        result = vkCreateFence(device, &fence_info, nullptr, &frames[i].inFlight);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "VulkanApp", 
                              "Failed to create in flight fence %d: %d", i, result);
            return false;
        }
    }
    return true;
}

bool VulkanApp::createFramebuffers() {
    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageView attachments[] = { swapchainImageViews[i] };
        
        VkFramebufferCreateInfo fb_info{};
        fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass = renderPass;
        fb_info.attachmentCount = 1;
        fb_info.pAttachments = attachments;
        fb_info.width = gSwapchainExtent.width;
        fb_info.height = gSwapchainExtent.height;
        fb_info.layers = 1;
        
        VkResult result = vkCreateFramebuffer(device, &fb_info, nullptr, &frames[i].framebuffer);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "VulkanApp", 
                              "Failed to create framebuffer %d: %d", i, result);
            return false;
        }
    }
    return true;
}

// 处理窗口大小变化的便利函数
bool VulkanApp::handleWindowResize(ANativeWindow* window) {
    int32_t width = ANativeWindow_getWidth(window);
    int32_t height = ANativeWindow_getHeight(window);
    
    // 检查是否需要重建
    if (width == (int32_t)gSwapchainExtent.width && 
        height == (int32_t)gSwapchainExtent.height) {
        return true; // 尺寸没有变化，无需重建
    }
    
    return rebuildSwapchain(window);

}
