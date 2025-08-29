// ImGuiLayer.h
#pragma once
#include "VulkanApp.h"
#include "imgui.h"
#include "backends/imgui_impl_android.h"
#include "backends/imgui_impl_vulkan.h"

class ImGuiLayer {
public:
    ANativeWindow* window;

    void init(ANativeWindow* window, VulkanApp& app);
    void shutdown(VulkanApp& app);
    void beginFrame(ANativeWindow* window, int width, int height);
    void frame_render(VulkanApp& app);
    void endFrame();
    void uploadFonts(VulkanApp& app);
    void testRender(VulkanApp& app);
};
