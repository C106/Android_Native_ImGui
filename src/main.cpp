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
#include <sched.h>
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
int gTargetFPS = 120;

// ═══════════════════════════════════════════
//  CPU 核心亲和性：将进程绑定到小核心
// ═══════════════════════════════════════════
static cpu_set_t gLittleCoreMask;

static void SetupCpuAffinity() {
    // 读取每个 CPU 核心的最大频率，区分大小核
    int numCpus = sysconf(_SC_NPROCESSORS_CONF);
    if (numCpus <= 0) numCpus = 8;

    std::vector<std::pair<int, long>> cpuFreqs;  // {cpu_id, max_freq}
    for (int i = 0; i < numCpus; i++) {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        FILE* f = fopen(path, "r");
        long freq = 0;
        if (f) {
            fscanf(f, "%ld", &freq);
            fclose(f);
        }
        cpuFreqs.push_back({i, freq});
    }

    // 找到最小的最大频率（即小核心的频率）
    long minFreq = LONG_MAX;
    for (const auto& p : cpuFreqs) {
        if (p.second > 0 && p.second < minFreq)
            minFreq = p.second;
    }

    // 构建小核心掩码：所有频率 == 最小频率的核心
    CPU_ZERO(&gLittleCoreMask);
    int littleCount = 0;
    for (const auto& p : cpuFreqs) {
        if (p.second == minFreq) {
            CPU_SET(p.first, &gLittleCoreMask);
            littleCount++;
        }
    }

    if (littleCount == 0 || littleCount == numCpus) {
        // 无法区分大小核，不设置亲和性
        LOGD("CPU affinity: cannot distinguish big/LITTLE (%d/%d), skipping\n",
             littleCount, numCpus);
        return;
    }

    // 绑定当前进程到小核心（影响所有线程）
    if (sched_setaffinity(0, sizeof(cpu_set_t), &gLittleCoreMask) == 0) {
        LOGD("CPU affinity: bound process to %d LITTLE cores (freq=%ld KHz)\n",
             littleCount, minFreq);
        for (int i = 0; i < numCpus; i++) {
            if (CPU_ISSET(i, &gLittleCoreMask))
                LOGD("  LITTLE core: CPU %d\n", i);
        }
    } else {
        LOGD("CPU affinity: sched_setaffinity failed: %s\n", strerror(errno));
    }
}

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
    // 将所有线程绑定到小核心，让大核心给游戏引擎用
    SetupCpuAffinity();

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

    //Paradise_hook = new c_driver();
    StartReadThread();
    // ── 主循环 ──
    while (IsToolActive == 1) {
        // 0. 记录帧开始时间（用于帧率控制）
        auto frameStartTime = std::chrono::steady_clock::now();

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

        // 2. 处理输入事件
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

        // 5. 帧率控制（基于帧开始时间，精确控制）
        if (gTargetFPS > 0) {
            auto targetFrameTime = std::chrono::microseconds(1000000 / gTargetFPS);
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - frameStartTime);

            if (elapsed < targetFrameTime) {
                auto sleepTime = targetFrameTime - elapsed;
                std::this_thread::sleep_for(sleepTime);
            }
        }
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
