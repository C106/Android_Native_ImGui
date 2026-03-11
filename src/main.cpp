#include "hook_touch_event.h"
#include "VulkanApp.h"
#include "ImGuiLayer.h"
#include "ANativeWindowCreator.h"
#include "draw_menu.h"
#include "draw_objects.h"
#include "read_mem.h"
#include <chrono>
#include <atomic>
#include <thread>
#include "Gyro.h"
#include "volume_control.h"
#include "driver_manager.h"
#ifdef NDEBUG
#define LOGD(...) ((void)0)
#define LOGE(...) ((void)0)
#else
#define LOGD(...) do { printf(__VA_ARGS__); fflush(stdout); } while(0)
#define LOGE(...) do { printf(__VA_ARGS__); fflush(stdout); } while(0)
#endif
VulkanApp gApp; 
static ImGuiLayer gImGui;
static ANativeWindow* gWindow = nullptr;
static bool gRunning = false;
std::atomic<bool> IsToolActive,IsMenuOpen;
static bool gNeedReinitialize = false;  // 屏幕旋转后需要重新初始化
android::ANativeWindowCreator::DisplayInfo displayInfo;
int secure_flag = 0;

//c_driver *Paradise_hook = nullptr;

Gyro* Gyro_Controller;
// 帧率控制参数
int gTargetFPS = 60;

// 重新初始化所有组件（用于屏幕旋转后）
void reinitializeAll() {
    LOGD("=== Reinit Start ===\n");

    LOGD("Reinit: vkDeviceWaitIdle...\n");
    vkDeviceWaitIdle(gApp.device);
    LOGD("Reinit: vkDeviceWaitIdle done\n");

    // 1. 先清理 ImGui（在 Vulkan cleanup 之前，因为需要使用有效的 Vulkan 设备）
    LOGD("Reinit: Step 1 - Shutting down ImGui backend...\n");
    if (gImGui.initialized) {
        gImGui.shutdown(gApp);
    }
    LOGD("Reinit: Step 1 done\n");

    // 2. 清理纹理加载器
    LOGD("Reinit: Step 2 - Shutting down texture loader...\n");
    ImGui_TextureLoaderShutdown(gApp);
    Draw_Menu_ResetTextures();
    LOGD("Reinit: Step 2 done\n");

    // 3. 清理 Vulkan
    LOGD("Reinit: Step 3 - Cleaning up Vulkan...\n");
    gApp.cleanup();
    LOGD("Reinit: Step 3 done\n");

    // 4. 销毁旧的窗口
    LOGD("Reinit: Step 4 - Destroying old window, current gWindow=%p\n", (void*)gWindow);
    if (gWindow) {
        android::ANativeWindowCreator::Destroy(gWindow);
        ANativeWindow_release(gWindow);
        gWindow = nullptr;
    }
    LOGD("Reinit: Step 4 done\n");

    // 5. 重新获取显示信息
    LOGD("Reinit: Step 5 - Getting display info...\n");
    displayInfo = android::ANativeWindowCreator::GetDisplayInfo();
    LOGD("Reinit: DisplayInfo: width=%d, height=%d, orientation=%d\n",
         displayInfo.width, displayInfo.height, displayInfo.orientation);

    // 6. 创建新窗口
    LOGD("Reinit: Step 6 - Creating new window %dx%d...\n", displayInfo.width, displayInfo.height);
    gWindow = android::ANativeWindowCreator::Create("a", displayInfo.width, displayInfo.height, false);
    LOGD("Reinit: Step 6 done, gWindow=%p\n", (void*)gWindow);

    if (!gWindow) {
        LOGE("Reinit ERROR: Failed to create window!\n");
        return;
    }

    // 7. 重新初始化 Vulkan
    LOGD("Reinit: Step 7 - Initializing Vulkan...\n");
    if (!gApp.init(gWindow)) {
        LOGE("Reinit ERROR: Vulkan init failed!\n");
        return;
    }
    LOGD("Reinit: Step 7 done, currentFrame=%zu, maxFramesInFlight=%u\n", gApp.currentFrame, gApp.maxFramesInFlight);

    // 8. 重新初始化 ImGui
    LOGD("Reinit: Step 8 - Initializing ImGui...\n");
    gImGui.init(gWindow, gApp);
    ImGui_InitTextureLoader(&gApp);
    LOGD("Reinit: Step 8 done, initialized=%d\n", gImGui.initialized);

    // 9. 重置帧计数器
    LOGD("Reinit: Step 9 - Resetting currentFrame to 0 (was %zu)\n", gApp.currentFrame);
    gApp.currentFrame = 0;
    LOGD("Reinit: Step 9 done\n");

    LOGD("=== Reinit Complete ===\n");
}


int main() {
    displayInfo = android::ANativeWindowCreator::GetDisplayInfo();

    gWindow = android::ANativeWindowCreator::Create("ESK", displayInfo.width, displayInfo.height, false);
    LOGD("Native Surface: %dx%d\n", displayInfo.width, displayInfo.height);
    if(gApp.init(gWindow))
        LOGD("Vulkan init OK\n");
    else {
        LOGE("Vulkan Failed\n");
        return -1;
    }
    gImGui.init(gWindow, gApp);
    ImGui_InitTextureLoader(&gApp);
    LOGD("ImGui init OK\n");
    gRunning = true;

    Gyro_Controller = new Gyro;
    if(!Gyro_Controller->bGyroConnect())
        LOGE("Gyro Device Connect Failed\n");

    int touch_fd = find_touch_device();
    if (touch_fd < 0) {
        LOGE("No touch device found!\n");
        return -1;
    }
    std::thread volume_monitor(volume);
    volume_monitor.detach();
    IsToolActive.store(1);
    IsMenuOpen.store(1);
    int lastOrientation = displayInfo.orientation;
    int frameCounter = 0;
    auto last_time = std::chrono::high_resolution_clock::now();

    //Paradise_hook = new c_driver();
    StartReadThread();
    // ── 主循环 ──
    while (IsToolActive == 1) {

        // 1. 屏幕旋转重新初始化（在帧开始前处理，不破坏渲染流水线）
        if (gNeedReinitialize) {
            LOGD("Main loop: gNeedReinitialize=true, calling reinitializeAll()\n");
            gNeedReinitialize = false;
            reinitializeAll();

            LOGD("Main loop: reinitializeAll() returned, initialized=%d\n", gImGui.initialized);

            if (!gImGui.initialized) {
                LOGE("Reinit failed, exiting\n");
                break;
            }

            close(touch_fd);
            touch_fd = find_touch_device();
            if (touch_fd < 0)
                LOGE("No touch device found after reinit!\n");
            else
                LOGD("Touch device reconnected: fd=%d\n", touch_fd);

            LOGD("Main loop: skipping this frame after reinit\n");
            continue; // 跳过本帧渲染，下帧用新管线
        }

        // 2. VSync 自然控制帧率（FIFO 呈现模式），仅更新时间戳用于 FPS 测量
        last_time = std::chrono::high_resolution_clock::now();

        process_input_event(touch_fd);

        // 3. 每 30 帧检测一次屏幕旋转（约 0.5 秒），降低 Binder IPC 开销
        if (++frameCounter >= 30) {
            frameCounter = 0;
            update_info();
            refresh_touch_device_range();

            android::ANativeWindowCreator::DisplayInfo currentDisplayInfo =
                android::ANativeWindowCreator::GetDisplayInfo();
            if (currentDisplayInfo.orientation != lastOrientation) {
                LOGD("Orientation changed: %d -> %d, scheduling reinit\n",
                     lastOrientation, currentDisplayInfo.orientation);
                lastOrientation = currentDisplayInfo.orientation;
                displayInfo = currentDisplayInfo;
                gNeedReinitialize = true;
                continue;
            }
        }

        // 4. 渲染（先用上一帧读取的数据渲染）
        ImGui_ProcessPendingTextureLoads();
        gImGui.beginFrame(gWindow, displayInfo.width, displayInfo.height);
        DrawObjects();  // 使用缓存的数据绘制
        if (IsMenuOpen == 1)
            Draw_Menu();

        if (IsToolActive == 0) {
            gImGui.endFrame();
            gImGui.frame_render(gApp);
            break;
        }

        gImGui.endFrame();
        gImGui.frame_render(gApp);  // Present 在这里完成

        // 5. Present 完成后立即读取下一帧数据（最小化延迟）
        // 这样读取的数据会在下一帧渲染时使用，延迟最小
        // 注意：DrawObjects() 内部会读取数据并缓存到静态变量中
    }

    // ── 清理 ──
    StopReadThread();
    gImGui.shutdown(gApp);
    ImGui_TextureLoaderShutdown(gApp);
    gApp.cleanup();
    if (gWindow) {
        android::ANativeWindowCreator::Destroy(gWindow);
        ANativeWindow_release(gWindow);
        gWindow = nullptr;
    }
    return 0;
}
