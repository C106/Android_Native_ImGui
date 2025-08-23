// Utils.h
#pragma once
#include <vulkan/vulkan.h>
#include <android/log.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ImGuiVulkan", __VA_ARGS__)

#include <iostream>
#include <sstream>
#include <stdexcept>

inline void check_vk(VkResult result, const char* msg = "") {
    if (result != VK_SUCCESS) {
        std::ostringstream oss;
        oss << "Vulkan error " << result;
        if (msg && msg[0] != '\0') {
            oss << " at " << msg;
        }

        // 打印到标准错误
        std::cerr << oss.str() << std::endl;

        // 然后抛出异常
        throw std::runtime_error(oss.str());
    }
}
