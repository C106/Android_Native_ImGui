// ImGuiLayer.cpp
#include "ImGuiLayer.h"
#include "Utils.h"
#include "misc/freetype/imgui_freetype.h"
VkExtent2D gSwapchainExtent;



void ImGuiLayer::init(ANativeWindow* new_window, VulkanApp& app) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    window = new_window;
    
    // Android 平台初始化
    if (!ImGui_ImplAndroid_Init(window)) {
        __android_log_print(ANDROID_LOG_ERROR, "ImGuiLayer", "Failed to initialize ImGui Android backend");
        return;
    }
    
    __android_log_print(ANDROID_LOG_INFO, "ImGuiLayer", "Android ImGui backend initialized");
    
    // 初始化 Vulkan 后端
    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.Instance = app.instance;
    init_info.PhysicalDevice = app.physicalDevice;
    init_info.Device = app.device;
    init_info.QueueFamily = app.graphicsQueueFamily;
    init_info.Queue = app.graphicsQueue;
    init_info.DescriptorPool = app.descriptorPool;
    init_info.MinImageCount = app.imageCount; // 修复：MinImageCount 应该等于 ImageCount
    init_info.ImageCount = app.imageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = nullptr; // 可选：添加错误检查回调
    init_info.RenderPass = app.renderPass;
    __android_log_print(ANDROID_LOG_INFO, "ImGuiLayer", 
                        "Device: %p, RenderPass: %p, DescriptorPool: %p", 
                        app.device, app.renderPass, app.descriptorPool);
    
    if (!ImGui_ImplVulkan_Init(&init_info)) {
        __android_log_print(ANDROID_LOG_ERROR, "ImGuiLayer", "Failed to initialize ImGui Vulkan backend");
        return;
    }
    
    __android_log_print(ANDROID_LOG_INFO, "ImGuiLayer", "Vulkan ImGui backend initialized");
    
    // 修复：正确上传字体纹理
    uploadFonts(app);
    
    __android_log_print(ANDROID_LOG_INFO, "ImGuiLayer", "ImGui initialization complete");
}

void ImGuiLayer::uploadFonts(VulkanApp& app) {
    
    ImGuiIO& io = ImGui::GetIO();

    // 主字体
    ImFont* font = io.Fonts->AddFontFromFileTTF(
        "/system/fonts/NotoSansCJK-Regular.ttc",
        32.0f,
        nullptr,
        io.Fonts->GetGlyphRangesChineseFull()
    );
    
    // Emoji 字体（彩色）
    ImFontConfig config;
    config.MergeMode = true;
    config.FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_LoadColor;
    printf("FreeType flags: %d\n", config.FontLoaderFlags);
    config.GlyphMinAdvanceX = 32.0f; // 使用彩色加载
    static const ImWchar emoji_ranges[] = { 
    0x1F300, 0x1F5FF, // 杂项符号和象形文字
    0x1F600, 0x1F64F, // 表情符号
    0x1F680, 0x1F6FF, // 交通和地图符号
    0x1F700, 0x1F77F, // 炼金术符号
    0x1F780, 0x1F7FF, // 几何图形扩展
    0x1F800, 0x1F8FF, // 补充箭头-C
    0x1F900, 0x1F9FF, // 补充符号和象形文字
    0x1FA00, 0x1FA6F, // 扩展-A
    0x1FA70, 0x1FAFF, // 符号和象形文字扩展-A
    0x2600, 0x26FF,   // 杂项符号
    0x2700, 0x27BF,   // 装饰符号
    0 
    };
    ImFont* emoji_font = io.Fonts->AddFontFromFileTTF(
        "/system/fonts/NotoColorEmoji.ttf",
        32.0f,
        &config,
        emoji_ranges
    );

    io.FontDefault = font;
    io.Fonts->Build();
    __android_log_print(ANDROID_LOG_INFO, "ImGuiLayer", "Font upload completed");
}

void ImGuiLayer::shutdown(VulkanApp& app) {
    // 等待设备空闲
    vkDeviceWaitIdle(app.device);
    
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplAndroid_Shutdown();
    ImGui::DestroyContext();
    
    __android_log_print(ANDROID_LOG_INFO, "ImGuiLayer", "ImGui shutdown complete");
}

void ImGuiLayer::beginFrame(ANativeWindow* window, int width, int height) {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::endFrame() {
    ImGui::Render();
}

void ImGuiLayer::frame_render(VulkanApp& app) {
    uint32_t frameIndex = app.currentFrame % app.maxFramesInFlight;
    
    __android_log_print(ANDROID_LOG_DEBUG, "ImGuiLayer", "Frame %zu (index %d) start", app.currentFrame, frameIndex);
    
    // -------------------------
    // 1) 等待当前帧的 fence（新的同步方式）
    // -------------------------
    VkResult r = vkWaitForFences(app.device, 1, &app.inFlightFences[frameIndex], VK_TRUE, UINT64_MAX);
    if (r != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, "ImGuiLayer", "Frame fence wait failed: %d", r);
        return;
    }
    
    // -------------------------
    // 2) 获取下一张 swapchain image
    // -------------------------
    uint32_t imageIndex = UINT32_MAX;
    r = vkAcquireNextImageKHR(app.device, app.swapchain, UINT64_MAX, 
                              app.imageAvailableSemaphores[frameIndex], VK_NULL_HANDLE, &imageIndex);
    
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        app.swapchainRebuildRequired = true;
        return;
    } else if (r != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, "ImGuiLayer", "Acquire image failed: %d", r);
        return;
    }
    
    // -------------------------
    // 3) 新的同步方式：只在需要时等待图像
    // -------------------------
    if (app.imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(app.device, 1, &app.imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
    }
    app.imagesInFlight[imageIndex] = app.inFlightFences[frameIndex];
    
    // 重要：只有在获取到图像后才重置 fence
    vkResetFences(app.device, 1, &app.inFlightFences[frameIndex]);

    // -------------------------
    // 4) 录制命令缓冲
    // -------------------------
    VkCommandBuffer cmd = app.frames[frameIndex].cmd;
    VkCommandPool cmdPool = app.frames[frameIndex].cmdPool;
    
    vkResetCommandPool(app.device, cmdPool, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkClearValue clear{};
    clear.color = { {0.0f, 0.0f, 0.0f, 0.0f} };

    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = app.renderPass;
    rpbi.framebuffer = app.swapchainFramebuffers[imageIndex]; // 使用独立的 framebuffer 数组
    rpbi.renderArea.offset = { 0, 0 };
    rpbi.renderArea.extent = gSwapchainExtent;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;

    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    
    // 渲染 ImGui（添加更严格的检查）
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data != nullptr && draw_data->Valid && draw_data->CmdListsCount > 0) {
        // 在新版本中，确保 ImGui 已经正确初始化
        if (ImGui::GetCurrentContext() != nullptr) {
            ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
        }
    }
    
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    // -------------------------
    // 5) 提交（修复同步）
    // -------------------------
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &app.imageAvailableSemaphores[frameIndex];
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &app.renderFinishedSemaphores[frameIndex];

    r = vkQueueSubmit(app.graphicsQueue, 1, &submitInfo, app.inFlightFences[frameIndex]);
    if (r != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, "ImGuiLayer", "Queue submit failed: %d", r);
        return;
    }

    // -------------------------
    // 6) Present
    // -------------------------
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &app.renderFinishedSemaphores[frameIndex];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &app.swapchain;
    presentInfo.pImageIndices = &imageIndex;

    VkResult present_result = vkQueuePresentKHR(app.graphicsQueue, &presentInfo);
    if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
        app.swapchainRebuildRequired = true;
    } else if (present_result != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, "ImGuiLayer", "Present failed: %d", present_result);
    }

    // -------------------------
    // 7) 前进到下一帧
    // -------------------------
    app.currentFrame++;
    
    __android_log_print(ANDROID_LOG_DEBUG, "ImGuiLayer", "Frame %zu completed", app.currentFrame - 1);
}

void ImGuiLayer::testRender(VulkanApp& app) {
    __android_log_print(ANDROID_LOG_INFO, "ImGuiLayer", "=== Test Render ===");
    
    // 不使用 ImGui，只清屏测试
    FrameData& f = app.frames[app.currentFrame];
    
    // 1. 等待 fence
    VkResult r = vkWaitForFences(app.device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);
    if (r != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, "ImGuiLayer", "Test: Fence wait failed: %d", r);
        return;
    }
    vkResetFences(app.device, 1, &f.inFlight);
    
    // 2. 获取图像
    uint32_t imageIndex;
    r = vkAcquireNextImageKHR(app.device, app.swapchain, UINT64_MAX, 
                              f.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (r != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, "ImGuiLayer", "Test: Acquire failed: %d", r);
        return;
    }
    
    // 3. 录制最简单的命令
    vkResetCommandPool(app.device, f.cmdPool, 0);
    
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);
    
    VkClearValue clear{};
    clear.color = { {1.0f, 0.0f, 0.0f, 1.0f} }; // 红色测试
    
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = app.renderPass;
    rpbi.framebuffer = app.frames[imageIndex].framebuffer;
    rpbi.renderArea.offset = { 0, 0 };
    rpbi.renderArea.extent = gSwapchainExtent;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;
    
    vkCmdBeginRenderPass(f.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    // 不做任何绘制，只清屏
    vkCmdEndRenderPass(f.cmd);
    
    vkEndCommandBuffer(f.cmd);
    
    // 4. 提交
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &f.imageAvailable;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &f.cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &f.renderFinished;
    
    r = vkQueueSubmit(app.graphicsQueue, 1, &submitInfo, f.inFlight);
    if (r != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, "ImGuiLayer", "Test: Submit failed: %d", r);
        return;
    }
    
    // 5. Present
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &f.renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &app.swapchain;
    presentInfo.pImageIndices = &imageIndex;
    
    r = vkQueuePresentKHR(app.graphicsQueue, &presentInfo);
    __android_log_print(ANDROID_LOG_INFO, "ImGuiLayer", "Test: Present result: %d", r);
    
    app.currentFrame = (app.currentFrame + 1) % app.frames.size();
    
    __android_log_print(ANDROID_LOG_INFO, "ImGuiLayer", "=== Test Render Complete ===");
}