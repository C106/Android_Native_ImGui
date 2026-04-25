// ImGuiLayer.cpp
#include "ImGuiLayer.h"
#include "Utils.h"
#include "misc/freetype/imgui_freetype.h"
#include "IconsFontAwesome7.h"
#include "fa_regular_400_otf.h"
#include "font1.h"
#include "font2.h"
#include "imgui_spectrum.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <vector>

// 发布模式：禁用调试日志
#ifdef NDEBUG
#define LOGI(...) ((void)0)
#else
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ImGuiLayer", __VA_ARGS__)
#endif

VkExtent2D gSwapchainExtent;
ImFont* gIconFont = nullptr;
ImFont* gBannerIconFont = nullptr;



void ImGuiLayer::init(ANativeWindow* new_window, VulkanApp& app) {
    IMGUI_CHECKVERSION();
    if (ImGui::GetCurrentContext() == nullptr)
    {
        ImGui::CreateContext();
    }

    ImGui::Spectrum::StyleColorsSpectrum();
    window = new_window;

    // 触摸优化：增大控件尺寸和间距
    ImGuiStyle& style = ImGui::GetStyle();
    style.ItemSpacing = ImVec2(12.0f, 10.0f);        // 默认 8x4 -> 12x10
    style.ItemInnerSpacing = ImVec2(8.0f, 8.0f);     // 默认 4x4 -> 8x8
    style.FramePadding = ImVec2(8.0f, 10.0f);        // 默认 4x8 -> 8x10
    style.GrabMinSize = 20.0f;                       // 默认 12 -> 20
    style.GrabRounding = 6.0f;                       // 默认 4 -> 6
    style.TouchExtraPadding = ImVec2(4.0f, 4.0f);    // 默认 0x0 -> 4x4
    style.ScrollbarSize = 18.0f;                     // 默认 14 -> 18

    // Android 平台初始化
    if (!ImGui_ImplAndroid_Init(window)) {
        LOGE( "Failed to initialize ImGui Android backend");
        return;
    }

    LOGI( "Android ImGui backend initialized");

    // 先加载字体
    uploadFonts(app);

    // 初始化 Vulkan 后端
    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.Instance = app.instance;
    init_info.PhysicalDevice = app.physicalDevice;
    init_info.Device = app.device;
    init_info.QueueFamily = app.graphicsQueueFamily;
    init_info.Queue = app.graphicsQueue;
    init_info.DescriptorPool = app.descriptorPool;
    init_info.MinImageCount = app.imageCount;
    init_info.ImageCount = app.imageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = nullptr;
    init_info.RenderPass = app.renderPass;

    if (!ImGui_ImplVulkan_Init(&init_info)) {
        LOGE( "Failed to initialize ImGui Vulkan backend");
        return;
    }

    LOGI( "Vulkan ImGui backend initialized");

    initialized = true;
    LOGI( "ImGui initialization complete");
}

void ImGuiLayer::uploadFonts(VulkanApp& app) {

    ImGuiIO& io = ImGui::GetIO();

    // 主字体（使用系统字体）
    ImFont* font = io.Fonts->AddFontFromFileTTF(
        "/system/fonts/NotoSansCJK-Regular.ttc",
        32.0f,
        nullptr,
        io.Fonts->GetGlyphRangesChineseFull()
    );

    // Emoji 字体（彩色，使用内存字体）
    ImFontConfig config;
    config.MergeMode = true;
    config.FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_LoadColor;
    config.GlyphMinAdvanceX = 32.0f;
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
    // 关键修复：AddFontFromMemoryTTF 默认接管传入指针的所有权，
    // 会在 DestroyContext 时调用 free() 释放它。
    // 但 NotoColorEmoji_ttf 是静态数组（非堆内存），free() 静态地址
    // 会触发 Android tagged-pointer 检测，导致 Abort。
    // 解决方案：先将静态数据拷贝到堆内存，再传给 ImGui 接管。
    void* emoji_font_data = malloc(NotoColorEmoji_ttf_len);
    memcpy(emoji_font_data, NotoColorEmoji_ttf, NotoColorEmoji_ttf_len);
    ImFont* emoji_font = io.Fonts->AddFontFromMemoryTTF(
        emoji_font_data, (int)NotoColorEmoji_ttf_len,
        32.0f,
        &config,
        emoji_ranges
    );
    // ImGui 已接管 emoji_font_data，DestroyContext 时会正确 free() 堆内存。

    void* icon_font_data = malloc(font1_ttf_len);
    memcpy(icon_font_data, font1_ttf, font1_ttf_len);
    gIconFont = io.Fonts->AddFontFromMemoryTTF(
        icon_font_data, (int)font1_ttf_len,
        32.0f
    );

    static const ImWchar fa_icon_ranges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
    void* banner_icon_font_data = malloc(Font_Awesome_7_Free_Regular_400_otf_len);
    memcpy(banner_icon_font_data,
           Font_Awesome_7_Free_Regular_400_otf,
           Font_Awesome_7_Free_Regular_400_otf_len);
    ImFontConfig banner_icon_config;
    banner_icon_config.GlyphMinAdvanceX = 32.0f;
    gBannerIconFont = io.Fonts->AddFontFromMemoryTTF(
        banner_icon_font_data,
        (int)Font_Awesome_7_Free_Regular_400_otf_len,
        30.0f,
        &banner_icon_config,
        fa_icon_ranges
    );
    io.FontDefault = font;

    LOGI( "Font upload completed");
}

void ImGuiLayer::shutdown(VulkanApp& app)
{
    if (!initialized)
    {
        LOGI("ImGui not initialized, skipping shutdown");
        return;
    }

    if (ImGui::GetCurrentContext() == nullptr)
    {
        LOGI("ImGui context already null");
        initialized = false;
        return;
    }

    // 等待 GPU 完成所有工作
    if (app.device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(app.device);
    }

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplAndroid_Shutdown();
    ImGui::DestroyContext();

    initialized = false;
}


void ImGuiLayer::beginFrame(ANativeWindow* window, int width, int height, float deltaTime) {
    if (!initialized) {
        LOGI( "ImGui not initialized, skipping beginFrame");
        return;
    }

    // 更新 ImGui 显示大小（支持屏幕旋转）
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)width, (float)height);

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    if (deltaTime > 0.0f) {
        io.DeltaTime = deltaTime;
    }
    ImGui::NewFrame();
}

void ImGuiLayer::endFrame() {
    ImGui::Render();
}

void ImGuiLayer::waitForPreviousFrame(VulkanApp& app) {
    if (!initialized) return;

    currentFrameIndex = app.currentFrame % app.maxFramesInFlight;

    VkResult r = vkWaitForFences(app.device, 1, &app.inFlightFences[currentFrameIndex], VK_TRUE, UINT64_MAX);
    if (r != VK_SUCCESS) {
        LOGE("Frame fence wait failed: %d", r);
    }
}

void ImGuiLayer::submitAndPresent(VulkanApp& app) {
    if (!initialized) {
        LOGI("ImGui not initialized, skipping submitAndPresent");
        return;
    }

    uint32_t frameIndex = currentFrameIndex;

    // -------------------------
    // 1) 获取下一张 swapchain image（阻塞等待，fence 已确保 pipeline 有空间）
    // -------------------------
    uint32_t imageIndex = UINT32_MAX;
    VkResult r = vkAcquireNextImageKHR(app.device, app.swapchain, UINT64_MAX,
                              app.imageAvailableSemaphores[frameIndex], VK_NULL_HANDLE, &imageIndex);

    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        app.swapchainRebuildRequired = true;
        return;
    } else if (r != VK_SUCCESS) {
        LOGE("Acquire image failed: %d", r);
        return;
    }

    // -------------------------
    // 2) 同步：只在需要时等待图像
    // -------------------------
    if (app.imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(app.device, 1, &app.imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
    }
    app.imagesInFlight[imageIndex] = app.inFlightFences[frameIndex];

    // 重要：只有在获取到图像后才重置 fence
    vkResetFences(app.device, 1, &app.inFlightFences[frameIndex]);

    // -------------------------
    // 3) 录制命令缓冲
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
    rpbi.framebuffer = app.swapchainFramebuffers[imageIndex];
    rpbi.renderArea.offset = { 0, 0 };
    rpbi.renderArea.extent = gSwapchainExtent;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;

    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    // 渲染 ImGui
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data != nullptr && draw_data->Valid && draw_data->CmdListsCount > 0) {
        ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
    }

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    // -------------------------
    // 4) 提交
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
        LOGE("Queue submit failed: %d", r);
        return;
    }

    // -------------------------
    // 5) Present
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
        LOGE("Present failed: %d", present_result);
    }

    // -------------------------
    // 6) 前进到下一帧
    // -------------------------
    app.currentFrame++;
}

void ImGuiLayer::frame_render(VulkanApp& app) {
    waitForPreviousFrame(app);
    submitAndPresent(app);
}

void ImGuiLayer::testRender(VulkanApp& app) {
    // 不使用 ImGui，只清屏测试
    FrameData& f = app.frames[app.currentFrame];

    // 1. 等待 fence
    VkResult r = vkWaitForFences(app.device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);
    if (r != VK_SUCCESS) {
        return;
    }
    vkResetFences(app.device, 1, &f.inFlight);

    // 2. 获取图像
    uint32_t imageIndex;
    r = vkAcquireNextImageKHR(app.device, app.swapchain, UINT64_MAX,
                              f.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (r != VK_SUCCESS) {
        return;
    }

    // 3. 录制命令
    vkResetCommandPool(app.device, f.cmdPool, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);

    VkClearValue clear{};
    clear.color = { {1.0f, 0.0f, 0.0f, 1.0f} };

    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = app.renderPass;
    rpbi.framebuffer = app.frames[imageIndex].framebuffer;
    rpbi.renderArea.offset = { 0, 0 };
    rpbi.renderArea.extent = gSwapchainExtent;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;

    vkCmdBeginRenderPass(f.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
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

    vkQueueSubmit(app.graphicsQueue, 1, &submitInfo, f.inFlight);

    // 5. Present
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &f.renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &app.swapchain;
    presentInfo.pImageIndices = &imageIndex;

    vkQueuePresentKHR(app.graphicsQueue, &presentInfo);

    app.currentFrame = (app.currentFrame + 1) % app.frames.size();
}

static VulkanApp* gAppForTexture = nullptr;
static VkSampler gTextureSampler = VK_NULL_HANDLE;

// 挂起的纹理加载请求
struct PendingTextureLoad {
    const unsigned char* data;
    int data_size;
    ImTextureID* out_texture;
    int* out_width;
    int* out_height;
    bool completed;
    bool success;
};
static std::vector<PendingTextureLoad> gPendingTextureLoads;

// 纹理数据存储
struct TextureData {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
};

// 使用 vector 存储所有加载的纹理，避免 map 的 key 问题
static std::vector<TextureData*> gTextureList;

void ImGui_InitTextureLoader(VulkanApp* app) {
    // 如果已经存在采样器，先销毁
    if (gTextureSampler != VK_NULL_HANDLE && app->device != VK_NULL_HANDLE) {
        vkDestroySampler(app->device, gTextureSampler, nullptr);
        gTextureSampler = VK_NULL_HANDLE;
    }

    gAppForTexture = app;

    // 创建纹理采样器
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.minLod = -1000;
    samplerInfo.maxLod = 1000;
    samplerInfo.maxAnisotropy = 1.0f;

    VkResult err = vkCreateSampler(app->device, &samplerInfo, nullptr, &gTextureSampler);
    if (err != VK_SUCCESS) {
        LOGE("ImGui_InitTextureLoader: vkCreateSampler failed: %d", err);
        gTextureSampler = VK_NULL_HANDLE;
    } else {
        LOGI("ImGui_InitTextureLoader: sampler created successfully");
    }

    LOGI("ImGui_InitTextureLoader: initialized");
}

// 请求延迟加载纹理（在渲染循环中完成）
void ImGui_RequestTextureLoad(const unsigned char* data, int data_size,
                               ImTextureID* out_texture, int* out_width, int* out_height) {
    if (!gAppForTexture || !data || data_size <= 0) {
        LOGE("ImGui_RequestTextureLoad: invalid parameters");
        *out_texture = (ImTextureID)0;
        return;
    }

    PendingTextureLoad request;
    request.data = data;
    request.data_size = data_size;
    request.out_texture = out_texture;
    request.out_width = out_width;
    request.out_height = out_height;
    request.completed = false;
    request.success = false;

    gPendingTextureLoads.push_back(request);
    LOGI("ImGui_RequestTextureLoad: queued texture load, %zu pending", gPendingTextureLoads.size());
}

// 处理挂起的纹理加载（应在渲染线程中调用）
void ImGui_ProcessPendingTextureLoads() {
    if (gPendingTextureLoads.empty() || !gAppForTexture) return;

    VulkanApp& app = *gAppForTexture;
    VkResult err;

    // 创建临时命令池和命令缓冲区（避免与渲染流水线冲突）
    VkCommandPool cmdPool;
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = app.graphicsQueueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

    if (vkCreateCommandPool(app.device, &poolInfo, nullptr, &cmdPool) != VK_SUCCESS) {
        LOGE("Failed to create temp command pool");
        return;
    }

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = cmdPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(app.device, &allocInfo, &cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(app.device, cmdPool, nullptr);
        LOGE("Failed to allocate temp command buffer");
        return;
    }

    LOGI("ImGui_ProcessPendingTextureLoads: processing %zu pending loads", gPendingTextureLoads.size());

    // 开始命令缓冲区录制
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        vkDestroyCommandPool(app.device, cmdPool, nullptr);
        LOGE("Failed to begin temp command buffer");
        return;
    }

    for (auto& request : gPendingTextureLoads) {
        if (request.completed) continue;

        LOGI("ImGui_ProcessPendingTextureLoads: loading texture %p, size=%d",
             request.data, request.data_size);

        // 验证 PNG 头
        if (!request.data || request.data_size < 8 ||
            request.data[0] != 0x89 || request.data[1] != 0x50 ||
            request.data[2] != 0x4E || request.data[3] != 0x47) {
            LOGE("ImGui_ProcessPendingTextureLoads: invalid PNG");
            *request.out_texture = (ImTextureID)0;
            request.completed = true;
            request.success = false;
            continue;
        }

        // 解码图像
        int width, height, channels;
        stbi_uc* pixels = stbi_load_from_memory(request.data, request.data_size,
                                                  &width, &height, &channels, 4);
        if (!pixels) {
            LOGE("ImGui_ProcessPendingTextureLoads: stbi_load failed");
            *request.out_texture = (ImTextureID)0;
            request.completed = true;
            request.success = false;
            continue;
        }

        LOGI("ImGui_ProcessPendingTextureLoads: decoded %dx%d", width, height);

        // 创建纹理
        LOGI("ImGui_ProcessPendingTextureLoads: creating TextureData...");
        TextureData* texData = new TextureData();
        LOGI("ImGui_ProcessPendingTextureLoads: TextureData created, device=%p", (void*)app.device);

        // 创建 Image
        LOGI("ImGui_ProcessPendingTextureLoads: creating VkImageCreateInfo...");
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {(uint32_t)width, (uint32_t)height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        LOGI("ImGui_ProcessPendingTextureLoads: calling vkCreateImage...");
        VkResult err = vkCreateImage(app.device, &imageInfo, nullptr, &texData->image);
        LOGI("ImGui_ProcessPendingTextureLoads: vkCreateImage returned %d, image=%p",
             err, (void*)texData->image);
        if (err != VK_SUCCESS) {
            LOGE("ImGui_ProcessPendingTextureLoads: vkCreateImage failed: %d", err);
            delete texData;
            *request.out_texture = (ImTextureID)0;
            request.completed = true;
            request.success = false;
            continue;
        }

        // 分配内存（优先 DEVICE_LOCAL，失败则回退到任意可用内存）
        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(app.device, texData->image, &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = findMemoryType(app.physicalDevice, memReq.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        err = vkAllocateMemory(app.device, &allocInfo, nullptr, &texData->memory);
        if (err != VK_SUCCESS) {
            // 回退：不要求 DEVICE_LOCAL，接受任意兼容内存类型
            LOGI("ImGui_ProcessPendingTextureLoads: DEVICE_LOCAL alloc failed (%d), trying fallback", err);
            VkPhysicalDeviceMemoryProperties memProps;
            vkGetPhysicalDeviceMemoryProperties(app.physicalDevice, &memProps);
            bool fallbackOk = false;
            for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
                if (memReq.memoryTypeBits & (1 << i)) {
                    allocInfo.memoryTypeIndex = i;
                    err = vkAllocateMemory(app.device, &allocInfo, nullptr, &texData->memory);
                    if (err == VK_SUCCESS) { fallbackOk = true; break; }
                }
            }
            if (!fallbackOk) {
                LOGE("ImGui_ProcessPendingTextureLoads: vkAllocateMemory failed (all types): %d", err);
                vkDestroyImage(app.device, texData->image, nullptr);
                delete texData;
                *request.out_texture = (ImTextureID)0;
                request.completed = true;
                request.success = false;
                continue;
            }
        }

        vkBindImageMemory(app.device, texData->image, texData->memory, 0);

        // 创建 ImageView
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = texData->image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        err = vkCreateImageView(app.device, &viewInfo, nullptr, &texData->view);
        if (err != VK_SUCCESS) {
            LOGE("ImGui_ProcessPendingTextureLoads: vkCreateImageView failed: %d", err);
            vkFreeMemory(app.device, texData->memory, nullptr);
            vkDestroyImage(app.device, texData->image, nullptr);
            delete texData;
            *request.out_texture = (ImTextureID)0;
            request.completed = true;
            request.success = false;
            continue;
        }

        // 转换布局
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texData->image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        // 复制数据（使用 staging buffer）
        VkBuffer stagingBuffer;
        VkBufferCreateInfo stagingInfo{};
        stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingInfo.size = (VkDeviceSize)width * height * 4;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        err = vkCreateBuffer(app.device, &stagingInfo, nullptr, &stagingBuffer);
        if (err != VK_SUCCESS) {
            LOGE("ImGui_ProcessPendingTextureLoads: vkCreateBuffer failed: %d", err);
            vkDestroyImageView(app.device, texData->view, nullptr);
            vkFreeMemory(app.device, texData->memory, nullptr);
            vkDestroyImage(app.device, texData->image, nullptr);
            delete texData;
            *request.out_texture = (ImTextureID)0;
            request.completed = true;
            request.success = false;
            continue;
        }

        VkMemoryRequirements stagingMemReq;
        vkGetBufferMemoryRequirements(app.device, stagingBuffer, &stagingMemReq);

        VkDeviceMemory stagingMemory;
        VkMemoryAllocateInfo stagingAllocInfo{};
        stagingAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        stagingAllocInfo.allocationSize = stagingMemReq.size;
        stagingAllocInfo.memoryTypeIndex = findMemoryType(app.physicalDevice, stagingMemReq.memoryTypeBits,
                                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        err = vkAllocateMemory(app.device, &stagingAllocInfo, nullptr, &stagingMemory);
        if (err != VK_SUCCESS) {
            // 回退：只要求 HOST_VISIBLE
            stagingAllocInfo.memoryTypeIndex = findMemoryType(app.physicalDevice, stagingMemReq.memoryTypeBits,
                                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            err = vkAllocateMemory(app.device, &stagingAllocInfo, nullptr, &stagingMemory);
        }
        if (err != VK_SUCCESS) {
            LOGE("ImGui_ProcessPendingTextureLoads: staging vkAllocateMemory failed: %d", err);
            vkDestroyBuffer(app.device, stagingBuffer, nullptr);
            vkDestroyImageView(app.device, texData->view, nullptr);
            vkFreeMemory(app.device, texData->memory, nullptr);
            vkDestroyImage(app.device, texData->image, nullptr);
            delete texData;
            *request.out_texture = (ImTextureID)0;
            request.completed = true;
            request.success = false;
            continue;
        }

        // 复制像素数据
        void* mappedData;
        err = vkMapMemory(app.device, stagingMemory, 0, stagingMemReq.size, 0, &mappedData);
        if (err != VK_SUCCESS) {
            LOGE("ImGui_ProcessPendingTextureLoads: vkMapMemory failed: %d", err);
        }
        memcpy(mappedData, pixels, (size_t)width * height * 4);
        vkUnmapMemory(app.device, stagingMemory);
        stbi_image_free(pixels);

        err = vkBindBufferMemory(app.device, stagingBuffer, stagingMemory, 0);

        // 保存 staging 资源引用，以便后续清理
        texData->stagingBuffer = stagingBuffer;
        texData->stagingMemory = stagingMemory;

        // 复制到 image
        VkBufferImageCopy copyRegion{};
        copyRegion.bufferOffset = 0;
        copyRegion.bufferRowLength = width;
        copyRegion.bufferImageHeight = height;
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = 0;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageExtent = {(uint32_t)width, (uint32_t)height, 1};

        vkCmdCopyBufferToImage(cmd, stagingBuffer, texData->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

        // 转换到 shader read
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        // 注册纹理
        if (!gTextureSampler) {
            LOGE("ImGui_ProcessPendingTextureLoads: gTextureSampler is invalid");
            *request.out_texture = (ImTextureID)0;
            request.completed = true;
            request.success = false;
            continue;
        }

        texData->descriptorSet = ImGui_ImplVulkan_AddTexture(gTextureSampler, texData->view,
                                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (texData->descriptorSet == VK_NULL_HANDLE) {
            LOGE("ImGui_ProcessPendingTextureLoads: ImGui_ImplVulkan_AddTexture failed");
            vkDestroyImageView(app.device, texData->view, nullptr);
            vkFreeMemory(app.device, texData->memory, nullptr);
            vkDestroyImage(app.device, texData->image, nullptr);
            delete texData;
            *request.out_texture = (ImTextureID)0;
            request.completed = true;
            request.success = false;
            continue;
        }

        gTextureList.push_back(texData);
        *request.out_texture = (ImTextureID)texData->descriptorSet;
        *request.out_width = width;
        *request.out_height = height;

        LOGI("ImGui_ProcessPendingTextureLoads: texture loaded successfully: %dx%d", width, height);
        request.completed = true;
        request.success = true;
    }

    // 结束命令缓冲区录制并提交
    err = vkEndCommandBuffer(cmd);
    if (err != VK_SUCCESS) {
        LOGE("ImGui_ProcessPendingTextureLoads: vkEndCommandBuffer failed: %d", err);
        vkDestroyCommandPool(app.device, cmdPool, nullptr);
        gPendingTextureLoads.clear();
        return;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    VkFence uploadFence;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    err = vkCreateFence(app.device, &fenceInfo, nullptr, &uploadFence);
    if (err == VK_SUCCESS) {
        err = vkQueueSubmit(app.graphicsQueue, 1, &submitInfo, uploadFence);
        if (err == VK_SUCCESS) {
            vkWaitForFences(app.device, 1, &uploadFence, VK_TRUE, UINT64_MAX);
        }
        vkDestroyFence(app.device, uploadFence, nullptr);
    }

    // 清理 staging buffer
    for (auto texData : gTextureList) {
        if (texData->stagingBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(app.device, texData->stagingBuffer, nullptr);
            texData->stagingBuffer = VK_NULL_HANDLE;
        }
        if (texData->stagingMemory != VK_NULL_HANDLE) {
            vkFreeMemory(app.device, texData->stagingMemory, nullptr);
            texData->stagingMemory = VK_NULL_HANDLE;
        }
    }

    // 清理临时命令池
    vkDestroyCommandPool(app.device, cmdPool, nullptr);

    // 清理已完成的请求
    gPendingTextureLoads.clear();
}

void ImGui_TextureLoaderShutdown(VulkanApp& app) {
    gPendingTextureLoads.clear();

    // 检查 Vulkan 设备是否有效
    if (app.device == VK_NULL_HANDLE) {
        LOGI( "Vulkan device is invalid, clearing texture list only");
        for (auto texData : gTextureList) {
            delete texData;
        }
        gTextureList.clear();
        gTextureSampler = VK_NULL_HANDLE;
        gAppForTexture = nullptr;
        return;
    }

    // 清理所有纹理
    for (auto texData : gTextureList) {
        if (texData->descriptorSet != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(app.device, app.descriptorPool, 1, &texData->descriptorSet);
        }
        if (texData->view != VK_NULL_HANDLE) {
            vkDestroyImageView(app.device, texData->view, nullptr);
        }
        if (texData->image != VK_NULL_HANDLE) {
            vkDestroyImage(app.device, texData->image, nullptr);
        }
        if (texData->memory != VK_NULL_HANDLE) {
            vkFreeMemory(app.device, texData->memory, nullptr);
        }
        delete texData;
    }
    gTextureList.clear();

    // 清理采样器
    if (gTextureSampler != VK_NULL_HANDLE) {
        vkDestroySampler(app.device, gTextureSampler, nullptr);
        gTextureSampler = VK_NULL_HANDLE;
    }

    gAppForTexture = nullptr;
}

// 查找内存类型的辅助函数
static uint32_t findMemoryType(
    VkPhysicalDevice physicalDevice,
    uint32_t typeFilter,
    VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
    {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }

    throw std::runtime_error("failed to find suitable memory type!");
}

// 清理函数：释放单个纹理的所有资源
static void cleanupTexture(TextureData* texData, VkDevice device, bool freeDescriptorSet, VkDescriptorPool descriptorPool) {
    if (!texData) return;

    if (texData->descriptorSet != VK_NULL_HANDLE && freeDescriptorSet) {
        vkFreeDescriptorSets(device, descriptorPool, 1, &texData->descriptorSet);
    }
    if (texData->view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, texData->view, nullptr);
    }
    if (texData->image != VK_NULL_HANDLE) {
        vkDestroyImage(device, texData->image, nullptr);
    }
    if (texData->memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, texData->memory, nullptr);
    }

    delete texData;
}

void ImGui_FreeTexture(ImTextureID texture) {
    if (!texture || !gAppForTexture) return;

    VkDescriptorSet descriptorSet = (VkDescriptorSet)texture;

    // 查找纹理
    for (auto it = gTextureList.begin(); it != gTextureList.end(); ++it) {
        if ((*it)->descriptorSet == descriptorSet) {
            VulkanApp& app = *gAppForTexture;
            cleanupTexture(*it, app.device, true, app.descriptorPool);
            gTextureList.erase(it);
            return;
        }
    }

    LOGE("Texture not found for freeing");
}
