// ImGuiLayer.h
#pragma once
#include "VulkanApp.h"
#include "imgui.h"
#include "backends/imgui_impl_android.h"
#include "backends/imgui_impl_vulkan.h"

// 全局纹理加载函数（可在任意位置使用）
extern ImFont* gIconFont;
void ImGui_InitTextureLoader(VulkanApp* app);
void ImGui_TextureLoaderShutdown(VulkanApp& app);
void ImGui_RequestTextureLoad(const unsigned char* data, int data_size,
                               ImTextureID* out_texture, int* out_width, int* out_height);
void ImGui_ProcessPendingTextureLoads();
void ImGui_FreeTexture(ImTextureID texture);
static uint32_t findMemoryType(
    VkPhysicalDevice physicalDevice,
    uint32_t typeFilter,
    VkMemoryPropertyFlags properties);
class ImGuiLayer {
public:
    ANativeWindow* window;
    bool initialized = false;

    void init(ANativeWindow* window, VulkanApp& app);
    void shutdown(VulkanApp& app);
    void beginFrame(ANativeWindow* window, int width, int height, float deltaTime);
    void frame_render(VulkanApp& app);
    void endFrame();
    void uploadFonts(VulkanApp& app);
    void testRender(VulkanApp& app);

    // 拆分 frame_render 为两阶段，降低数据读取到显示的延迟
    void waitForPreviousFrame(VulkanApp& app);
    void submitAndPresent(VulkanApp& app);

private:
    uint32_t currentFrameIndex = 0; // waitForPreviousFrame 计算，submitAndPresent 使用
};
