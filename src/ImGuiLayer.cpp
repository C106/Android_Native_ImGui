// ImGuiLayer.cpp
#include "ImGuiLayer.h"
#include "Utils.h"
VkExtent2D gSwapchainExtent;


void ImGuiLayer::init(ANativeWindow* window, VulkanApp& app) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplayFramebufferScale = ImVec2(10,10);
    ImGui_ImplAndroid_Init(window);
    printf("Android imgui init\n");
    app.imagesInFlight.resize(app.imageCount, VK_NULL_HANDLE);
    
    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.Instance = app.instance;
    init_info.PhysicalDevice = app.physicalDevice;
    init_info.Device = app.device;
    init_info.QueueFamily = app.graphicsQueueFamily;
    init_info.Queue = app.graphicsQueue;
    init_info.DescriptorPool = app.descriptorPool;
    init_info.MinImageCount = app.minImageCount;
    init_info.ImageCount = app.imageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    printf("Device: %p, RenderPass: %p, DescriptorPool: %p\n", app.device, app.renderPass, app.descriptorPool);
    
    ImGui_ImplVulkan_Init(&init_info, app.renderPass);
    printf("Vulkan imgui init\n");
    // 上传字体
    
    // 在 ImGui 1.90+ 里直接用 CreateFontsTexture2
#if IMGUI_VERSION_NUM >= 19000
    ImGuiIO& fio = ImGui::GetIO();
    io.Fonts->Clear();  // 可选：清除默认字体

    ImFont* font = fio.Fonts->AddFontFromFileTTF(
        "/system/fonts/SourceSansPro-Regular.ttf", // 字体路径
        32.0f,                            // 字体大小（像素）
        nullptr,
        fio.Fonts->GetGlyphRangesDefault()
    );
    fio.FontDefault = font;  // 使用该字体为默认字体

#else
    // ImGui_ImplVulkan_CreateFontsTexture(cmd);
    // ImGui_ImplVulkan_DestroyFontUploadObjects();
#endif
}

void ImGuiLayer::shutdown(VulkanApp& app) {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplAndroid_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiLayer::beginFrame(ANativeWindow* window, int width, int height) {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::frame_render(VulkanApp& app) {
    FrameData& f = app.frames[app.currentFrame];

    // -------------------------
    // 1) CPU 等待上一帧完成
    // -------------------------
    
    VkResult r = vkWaitForFences(app.device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);
    if (r != VK_SUCCESS) {
        printf("vkWaitForFences failed: %d\n", r);
    }
    vkResetFences(app.device, 1, &f.inFlight);

    // -------------------------
    // 2) 获取下一张 swapchain image
    // -------------------------
    uint32_t imageIndex = UINT32_MAX;
    r = vkAcquireNextImageKHR(app.device, app.swapchain, UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        printf("Swapchain out of date, rebuild needed\n");
        return;
    } else if (r != VK_SUCCESS) {
        printf("vkAcquireNextImageKHR failed: %d\n", r);
        return;
    }
    
    
    
    // -------------------------
    // 3) 等待 image 上一帧使用完成
    // -------------------------
    if (app.imagesInFlight[imageIndex] != VK_NULL_HANDLE) {

        vkWaitForFences(app.device, 1, &app.imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
    }
    // 更新 imagesInFlight
    
    app.imagesInFlight[imageIndex] = f.inFlight;

    // -------------------------
    // 4) 录制命令缓冲
    // -------------------------
    vkResetCommandPool(app.device, f.cmdPool, 0);

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(f.cmd, &bi), "vkBeginCommandBuffer");

    VkClearValue clear;
    clear.color = { {0.0f, 0.0f, 0.0f, 0.0f} }; // 透明 overlay

    VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpbi.renderPass = app.renderPass;
    rpbi.framebuffer = app.frames[imageIndex].framebuffer; // 对应 imageIndex
    rpbi.renderArea.offset = { 0, 0 };
    rpbi.renderArea.extent = gSwapchainExtent;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;

    vkCmdBeginRenderPass(f.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), f.cmd);
    vkCmdEndRenderPass(f.cmd);

    check_vk(vkEndCommandBuffer(f.cmd), "vkEndCommandBuffer");

    // -------------------------
    // 5) 提交 GPU 执行
    // -------------------------
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &f.imageAvailable;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &f.cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &f.renderFinished;

    check_vk(vkQueueSubmit(app.graphicsQueue, 1, &submitInfo, f.inFlight), "vkQueueSubmit");

    VkResult status = vkGetFenceStatus(app.device, f.inFlight);
    
    // -------------------------
    // 6) Present 到屏幕
    // -------------------------
    VkPresentInfoKHR presentInfo{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &f.renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &app.swapchain;
    presentInfo.pImageIndices = &imageIndex;

    VkResult pres = vkQueuePresentKHR(app.graphicsQueue, &presentInfo);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
        printf("vkQueuePresentKHR: OUT_OF_DATE/SUBOPTIMAL\n");
    } else if (pres != VK_SUCCESS) {
        printf("vkQueuePresentKHR failed: %d\n", pres);
    }

    // -------------------------
    // 7) Advance frame
    // -------------------------
    app.currentFrame = (app.currentFrame + 1) % app.frames.size();
}

void ImGuiLayer::endFrame(VulkanApp& app) {
    ImGui::Render();
    frame_render(app);  
}
