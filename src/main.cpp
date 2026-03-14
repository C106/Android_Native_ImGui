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

// FPS 统计
static int gFrameCount = 0;
static float gCurrentFPS = 0.0f;
static auto gLastFPSUpdateTime = std::chrono::steady_clock::now();

// ═══════════════════════════════════════════
//  CPU 核心亲和性：将进程绑定到小核心
// ═══════════════════════════════════════════
static cpu_set_t gBigCoreMask;

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

    // 找到最小和最大频率
    long minFreq = LONG_MAX;
    long maxFreq = 0;
    for (const auto& p : cpuFreqs) {
        if (p.second > 0) {
            if (p.second < minFreq) minFreq = p.second;
            if (p.second > maxFreq) maxFreq = p.second;
        }
    }

    // 构建中大核心掩码：所有频率 > 最小频率的核心（排除小核心）
    CPU_ZERO(&gBigCoreMask);
    int bigCount = 0;
    for (const auto& p : cpuFreqs) {
        if (p.second > minFreq) {
            CPU_SET(p.first, &gBigCoreMask);
            bigCount++;
        }
    }

    if (bigCount == 0 || bigCount == numCpus) {
        // 无法区分大小核，不设置亲和性
        LOGD("CPU affinity: cannot distinguish big/LITTLE (%d/%d), skipping\n",
             bigCount, numCpus);
        return;
    }

    // 绑定当前进程到中大核心（高性能核心）
    if (sched_setaffinity(0, sizeof(cpu_set_t), &gBigCoreMask) == 0) {
        LOGD("CPU affinity: bound process to %d big/medium cores (max_freq=%ld KHz)\n",
             bigCount, maxFreq);
        for (int i = 0; i < numCpus; i++) {
            if (CPU_ISSET(i, &gBigCoreMask)) {
                long freq = cpuFreqs[i].second;
                LOGD("  Big/Medium core: CPU %d (freq=%ld KHz)\n", i, freq);
            }
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

// 在屏幕左下角显示 FPS
static void DrawFPSCounter() {
    ImGuiIO& io = ImGui::GetIO();

    // 左下角位置（留出边距）
    const float PADDING = 10.0f;
    ImVec2 pos(PADDING, io.DisplaySize.y - PADDING);

    // 设置窗口位置和大小
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);  // 半透明背景

    // 创建无边框、无标题栏的小窗口
    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove;

    if (ImGui::Begin("##FPSCounter", nullptr, window_flags)) {
        // 显示 FPS（绿色文字）
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "FPS: %.1f", gCurrentFPS);

        // 显示目标 FPS（灰色小字）
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(Target: %d)", gTargetFPS);
    }
    ImGui::End();
}


int main() {
    // 将所有线程绑定到中大核心（高性能核心），提升渲染性能
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
    bool firstFrame = true; // 首帧无上一帧需要等待

    // 帧率控制：固定时间步进，避免累积误差
    auto nextFrameTime = std::chrono::steady_clock::now();

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
            firstFrame = true; // reinit 后无上一帧需要等待
            continue; // 跳过本帧渲染，下帧用新管线
        }

        // 2. 处理输入事件
        process_input_event(touch_fd);

        // 3. 每 60 帧检测一次屏幕旋转（约 0.5 秒 @ 120 FPS），降低 Binder IPC 开销
        if (++frameCounter >= 60) {
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

        // 4. 在 fence wait 前读取游戏数据（减少 8-16ms 延迟）
        GameFrameData gameData = ReadGameData();

        // 5. 等待 GPU pipeline 清空（首帧跳过，无上一帧需要等待）
        if (!firstFrame) {
            gImGui.waitForPreviousFrame(gApp);
        }
        firstFrame = false;

        // 6. fence 释放后使用预读数据渲染
        ImGui_ProcessPendingTextureLoads();
        gImGui.beginFrame(gWindow, displayInfo.width, displayInfo.height);

        // 统计 FPS（每秒更新一次）
        gFrameCount++;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - gLastFPSUpdateTime).count();
        if (elapsed >= 1000) {  // 每 1000ms 更新一次
            gCurrentFPS = gFrameCount * 1000.0f / elapsed;
            gFrameCount = 0;
            gLastFPSUpdateTime = now;
        }

        DrawObjectsWithData(gameData);  // 使用预读数据（延迟更低）
        if (IsMenuOpen == 1)
            Draw_Menu();

        DrawFPSCounter();  // 显示 FPS 计数器（左下角）

        if (IsToolActive == 0) {
            gImGui.endFrame();
            gImGui.submitAndPresent(gApp);
            break;
        }

        gImGui.endFrame();
        gImGui.submitAndPresent(gApp);  // MAILBOX 模式非阻塞，降低延迟

        // 7. 帧率控制
        // MAILBOX present mode 非阻塞，需要主动限制帧率
        // 仅在 gTargetFPS > 60 时才主动限制（避免超过屏幕刷新率）
        if (gTargetFPS > 60) {
            int effectiveFPS = gTargetFPS;
            auto targetDuration = std::chrono::microseconds(1000000 / effectiveFPS);
            nextFrameTime += targetDuration;

            auto now = std::chrono::steady_clock::now();
            if (nextFrameTime <= now) {
                nextFrameTime = now + targetDuration;
            } else {
                std::this_thread::sleep_for(nextFrameTime - now);
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
