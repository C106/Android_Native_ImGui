#include "VulkanApp.h"
#include "Utils.h"
#include "vk-bootstrap/VkBootstrap.h"

#include <vector>

extern VkExtent2D gSwapchainExtent; // 方便给 frame_render 用

bool VulkanApp::init(ANativeWindow* window) {
    // 1. 创建 Instance
    vkb::InstanceBuilder builder;
    builder.enable_extension(VK_KHR_SURFACE_EXTENSION_NAME)
            .enable_extension(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    auto inst_ret = builder.set_app_name("ImGuiVulkanAndroid")
                            .require_api_version(1, 1, 0)
                            .use_default_debug_messenger()
                            .request_validation_layers()
                            .build();
    if (!inst_ret) return false;
    auto vkb_inst = inst_ret.value();
    
    instance = vkb_inst.instance;   
        
    // 创建 Android 表面
    VkAndroidSurfaceCreateInfoKHR surface_info = {};
    surface_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    surface_info.window = window;
    check_vk(vkCreateAndroidSurfaceKHR(instance, &surface_info, nullptr, &surface),"vkCreateAndroidSurfaceKHR");
    // 2. 选择 PhysicalDevice
    vkb::PhysicalDeviceSelector selector{ vkb_inst };
    selector.add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    auto phys_ret = selector.set_surface(surface) 
                            .select();
    if (!phys_ret) return false;
    auto phys = phys_ret.value();
    
    physicalDevice = phys.physical_device;

    // 3. 创建 Device
    vkb::DeviceBuilder dev_builder{ phys };
    auto dev_ret = dev_builder.build();
    if (!dev_ret) return false;
    auto vkb_dev = dev_ret.value();
    device = vkb_dev.device;
    graphicsQueue = vkb_dev.get_queue(vkb::QueueType::graphics).value();
    graphicsQueueFamily = vkb_dev.get_queue_index(vkb::QueueType::graphics).value();

    // 4. Surface + Swapchain
    

    vkb::SwapchainBuilder swapchain_builder{ vkb_dev };
    auto swap_ret = swapchain_builder.set_old_swapchain(VK_NULL_HANDLE)
                                     .set_desired_format({ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
                                     .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                                     .set_desired_extent(ANativeWindow_getWidth(window), ANativeWindow_getHeight(window))
                                     .build();
    if (!swap_ret) return false;
    auto vkb_swap = swap_ret.value();

    swapchain = vkb_swap.swapchain;
    swapchainImages = vkb_swap.get_images().value();
    swapchainImageViews = vkb_swap.get_image_views().value();
    gSwapchainExtent = vkb_swap.extent;
    imageCount = (uint32_t)swapchainImages.size();
    imagesInFlight.resize(swapchainImages.size(), VK_NULL_HANDLE);
    
    // 5. RenderPass
    VkAttachmentDescription color_attachment{};
    color_attachment.format = vkb_swap.image_format;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkRenderPassCreateInfo rp_info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &color_attachment;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    check_vk(vkCreateRenderPass(device, &rp_info, nullptr, &renderPass), "vkCreateRenderPass");

    // 6. Framebuffers + per-frame data
    frames.resize(2);

    for (uint32_t i = 0; i < imageCount; i++) {
        // Framebuffer
        VkImageView attachments[] = { swapchainImageViews[i] };
        VkFramebufferCreateInfo fb_info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fb_info.renderPass = renderPass;
        fb_info.attachmentCount = 1;
        fb_info.pAttachments = attachments;
        fb_info.width = gSwapchainExtent.width;
        fb_info.height = gSwapchainExtent.height;
        fb_info.layers = 1;
        check_vk(vkCreateFramebuffer(device, &fb_info, nullptr, &frames[i].framebuffer), "vkCreateFramebuffer");

        // Command Pool + Buffer
        VkCommandPoolCreateInfo pool_info{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pool_info.queueFamilyIndex = graphicsQueueFamily;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check_vk(vkCreateCommandPool(device, &pool_info, nullptr, &frames[i].cmdPool), "vkCreateCommandPool");

        VkCommandBufferAllocateInfo alloc_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        alloc_info.commandPool = frames[i].cmdPool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;
        check_vk(vkAllocateCommandBuffers(device, &alloc_info, &frames[i].cmd), "vkAllocateCommandBuffers");

        // Semaphores + Fence
        VkSemaphoreCreateInfo sem_info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        check_vk(vkCreateSemaphore(device, &sem_info, nullptr, &frames[i].imageAvailable), "vkCreateSemaphore");
        check_vk(vkCreateSemaphore(device, &sem_info, nullptr, &frames[i].renderFinished), "vkCreateSemaphore");

        VkFenceCreateInfo fence_info{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check_vk(vkCreateFence(device, &fence_info, nullptr, &frames[i].inFlight), "vkCreateFence");
    }

    // 7. DescriptorPool
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
    };
    VkDescriptorPoolCreateInfo pool_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1000 * (uint32_t)(sizeof(pool_sizes)/sizeof(pool_sizes[0]));
    pool_info.poolSizeCount = (uint32_t)(sizeof(pool_sizes)/sizeof(pool_sizes[0]));
    pool_info.pPoolSizes = pool_sizes;
    check_vk(vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptorPool), "vkCreateDescriptorPool");

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
