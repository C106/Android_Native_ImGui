#ifndef __VULKANUTILS_G_H__
#define __VULKANUTILS_G_H__

//#include <cstring>
#include <android/native_window.h>
#include "imgui.h"
#include "vulkan_wrapper.h"

// A struct to manage data related to one image in vulkan
struct MyTextureData {
    VkDescriptorSet DS;         // Descriptor set: this is what you'll pass to Image()
    int Width;
    int Height;
    int Channels;

    // Need to keep track of these to properly cleanup
    VkImageView ImageView;
    VkImage Image;
    VkDeviceMemory ImageMemory;
    VkSampler Sampler;
    VkBuffer UploadBuffer;
    VkDeviceMemory UploadBufferMemory;

    MyTextureData() { memset(this, 0, sizeof(*this)); }
};

ImTextureID LoadTextureFromMemory(const unsigned char *filedata, int len);

bool LoadTextureFromFile(const char *filename, MyTextureData *tex_data);

void RemoveTexture(MyTextureData *tex_data);

void SetupVulkan();

void SetupVulkanWindow(ANativeWindow *window, int width, int height);

void UploadFonts();

void SwapChainRebuild(int w, int h);

void FrameRender(ImDrawData *draw_data);

void FramePresent();

void DeviceWait();

void CleanupVulkanWindow();

void CleanupVulkan();

#endif // __VULKAN_WRAPPER_G_H__
