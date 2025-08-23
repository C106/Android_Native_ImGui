// Utils.h
#pragma once
#include <vulkan/vulkan.h>
#include <android/log.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ImGuiVulkan", __VA_ARGS__)

inline void check_vk(VkResult err) {
    if (err != VK_SUCCESS) {
        LOGE("Vulkan error: %d", err);
        abort();
    }
}
