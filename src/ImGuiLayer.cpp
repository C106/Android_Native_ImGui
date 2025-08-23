// ImGuiLayer.cpp
#include "ImGuiLayer.h"
#include "Utils.h"
VkExtent2D gSwapchainExtent;
void ImGuiLayer::init(ANativeWindow* window, VulkanApp& app) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplAndroid_Init(window);
    printf("Android imgui init\n");

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
    // 需要你自己封装一个一次性 command buffer
    // ImGui_ImplVulkan_CreateFontsTexture2(cmd);
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

void ImGuiLayer::frame_render(VulkanApp& app){
    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(
        app.device, app.swapchain, UINT64_MAX,
        app.frames[imageIndex].imageAvailable, VK_NULL_HANDLE, &imageIndex);

    if (acq == VK_ERROR_OUT_OF_DATE_KHR || acq == VK_SUBOPTIMAL_KHR) {
        //rebuild_swapchain();
        return;
    }
    

    auto& f = app.frames[imageIndex];
    vkWaitForFences(app.device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);
    vkResetFences(app.device, 1, &f.inFlight);
    vkResetCommandPool(app.device, f.cmdPool, 0);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(f.cmd, &bi);

    VkClearValue clear{};
    clear.color = {{0.05f, 0.05f, 0.07f, 1.0f}};


    
    VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass = app.renderPass;
    rpbi.framebuffer = f.framebuffer;
    rpbi.renderArea.offset = {0,0};
    rpbi.renderArea.extent = gSwapchainExtent;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues    = &clear;

    vkCmdBeginRenderPass(f.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), f.cmd);
    vkCmdEndRenderPass(f.cmd);

    vkEndCommandBuffer(f.cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.imageAvailable; // ✅ per-frame
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &f.renderFinished;
    vkQueueSubmit(app.graphicsQueue, 1, &si, f.inFlight);

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    VkSwapchainKHR swapchains[] = { app.swapchain };
    pi.swapchainCount = 1;
    pi.pSwapchains = swapchains;
    pi.pImageIndices = &imageIndex;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &f.renderFinished;

    VkResult pres = vkQueuePresentKHR(app.graphicsQueue, &pi);
}

void ImGuiLayer::endFrame(VulkanApp& app) {
    ImGui::Render();
    frame_render(app);  // 需要自己实现
}
