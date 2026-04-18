#include "hook_touch_event.h"
#include "VulkanApp.h"
#include "ImGuiLayer.h"
#include "ANativeWindowCreator.h"
#include "draw_menu.h"
#include "draw_objects.h"
#include "game_frame_reader.h"
#include "read_mem.h"
#include "auto_aim.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <atomic>
#include <thread>
#include "Gyro.h"
#include "volume_control.h"
#include "driver_manager.h"
#include "cpu_affinity.h"
#include "game_fps_monitor.h"
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
static int gEffectiveFPS = 0;  // 实际同步到的绘制 FPS
static auto gLastFPSUpdateTime = std::chrono::steady_clock::now();
static float gLastSyncedDeltaTime = 1.0f / 60.0f;
static auto gLastUiRenderTime = std::chrono::steady_clock::now();
static auto gLastFrameStartTime = std::chrono::steady_clock::now();
static GameFrameData gLastRenderedGameData{};
static bool gHasLastRenderedGameData = false;

static float GetFallbackGameDeltaTime();
static float GetRenderStepDeltaTime();

static void ResetSynchronizedTiming() {
    gLastSyncedDeltaTime = 1.0f / 60.0f;
}

static bool ShouldRenderUnsyncedUiFrame() {
    float fallbackDeltaTime = GetRenderStepDeltaTime();
    auto kUiFallbackInterval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<float>(fallbackDeltaTime));
    auto now = std::chrono::steady_clock::now();
    if (now - gLastUiRenderTime < kUiFallbackInterval) {
        return false;
    }
    gLastUiRenderTime = now;
    return true;
}

static void MarkUiFrameRendered() {
    gLastUiRenderTime = std::chrono::steady_clock::now();
}

static float GetFallbackGameDeltaTime() {
    int targetFPS = GetGameTargetFPS();
    if (targetFPS <= 0) {
        targetFPS = (gTargetFPS > 0) ? gTargetFPS : 60;
    }
    targetFPS = std::clamp(targetFPS, 1, 240);
    return 1.0f / static_cast<float>(targetFPS);
}

static float GetRenderStepDeltaTime() {
    int targetFPS = GetGameTargetFPS();
    if (targetFPS <= 0) {
        targetFPS = (gTargetFPS > 0) ? gTargetFPS : 60;
    }
    targetFPS = std::clamp(targetFPS, 20, 144);
    return 1.0f / static_cast<float>(targetFPS);
}

static float ResolveSynchronizedDeltaTime(const GameFrameData& gameData) {
    constexpr float kMinDeltaTime = 1.0f / 240.0f;
    constexpr float kMaxDeltaTime = 0.25f;

    float deltaTime = gameData.gameDeltaTime;
    if (deltaTime >= kMinDeltaTime && deltaTime <= kMaxDeltaTime) {
        gLastSyncedDeltaTime = deltaTime;
        return deltaTime;
    }

    if (gLastSyncedDeltaTime >= kMinDeltaTime && gLastSyncedDeltaTime <= kMaxDeltaTime) {
        return gLastSyncedDeltaTime;
    }

    gLastSyncedDeltaTime = GetFallbackGameDeltaTime();
    return gLastSyncedDeltaTime;
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

    LOGD("Reinit: Step 2.5 - Shutting down draw object caches...\n");
    ShutdownDrawObjects();
    LOGD("Reinit: Step 2.5 done\n");

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
static void DrawFPSCounter(const game_fps_monitor::GameFPSStats& gameStats) {
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

        // 显示同步后的 Overlay FPS 与游戏目标 FPS
        ImGui::SameLine();
        if (gEffectiveFPS > 0 && gEffectiveFPS < gTargetFPS) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "(Sync: %d/%d)", gEffectiveFPS, gTargetFPS);
        } else {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(Target: %d)", gTargetFPS);
        }

        // 显示游戏实际 FPS + 抖动诊断
        if (gameStats.fps > 0.5f) {
            // 使用从内存读取的游戏 FPS Level 判断（85% 作为阈值）
            int gameTargetFPS = GetGameTargetFPS();
            float threshold = gameTargetFPS * 0.9f;
            bool isLowFPS = gameStats.fps < threshold;
            ImVec4 fpsColor = isLowFPS ? ImVec4(1.0f, 0.0f, 0.0f, 1.0f) : ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
            ImGui::TextColored(fpsColor, "Game: %.0f", gameStats.fps);

            // 显示 Jank 计数（红色，仅在有掉帧时显示）
            if (gameStats.jank_count > 0) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Jank: %d", gameStats.jank_count);
            }

            // 显示最大帧时间（黄色）
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Max: %.1fms", gameStats.frametime_max_ms);
        }
    }
    ImGui::End();
}


int main() {
    // 初始化 CPU 拓扑并将渲染主线程绑定到大核
    InitCpuTopology();
    SetThreadAffinity(CoreTier::BIG);

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
    std::thread volume_monitor([]{ SetThreadAffinity(CoreTier::LITTLE); volume(); });
    volume_monitor.detach();
    IsToolActive.store(1);
    IsMenuOpen.store(1);
    int lastOrientation = displayInfo.orientation;
    int frameCounter = 0;
    bool firstFrame = true; // 首帧无上一帧需要等待
    uint64_t lastSyncedGameFrame = 0;
    bool previousMenuOpen = IsMenuOpen.load(std::memory_order_relaxed);
    bool previousTouchActive = false;

    //Paradise_hook = new c_driver();
    StartReadThread();
    InitAutoAim();
    ResetSynchronizedTiming();
    gLastUiRenderTime = std::chrono::steady_clock::now();
    gHasLastRenderedGameData = false;

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
            lastSyncedGameFrame = 0;
            ResetSynchronizedTiming();
            gLastUiRenderTime = std::chrono::steady_clock::now();
            gHasLastRenderedGameData = false;
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

        const bool menuOpen = IsMenuOpen.load(std::memory_order_relaxed);
        const bool menuStateChanged = (menuOpen != previousMenuOpen);
        const bool touchActive = has_active_touch_points();
        const bool touchStateChanged = (touchActive != previousTouchActive);
        uint64_t currentGameFrame = ReadFrameCounter();

        if (lastSyncedGameFrame != 0 && currentGameFrame < lastSyncedGameFrame) {
            lastSyncedGameFrame = 0;
            ResetSynchronizedTiming();
            firstFrame = true;
        }

        const bool hasNewGameFrame = currentGameFrame != 0 && currentGameFrame != lastSyncedGameFrame;

        // 帧率限制：无新游戏帧时，按目标 FPS 限制渲染
        if (!ShouldRenderUnsyncedUiFrame() && !hasNewGameFrame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        GameFrameData gameData = gLastRenderedGameData;
        bool haveRenderableGameFrame = false;
        float syncedDeltaTime = GetFallbackGameDeltaTime();
        const float renderStepDeltaTime = GetRenderStepDeltaTime();

        // 完全空闲且无缓存数据时跳过渲染（但保留悬浮球可交互）
        // if (!haveRenderableGameFrame && !gHasLastRenderedGameData && !menuOpen && !touchActive) {
        //     std::this_thread::sleep_for(std::chrono::milliseconds(1));
        //     continue;
        // }

        // 5. 等待 GPU pipeline 清空（首帧跳过，无上一帧需要等待）
        if (!firstFrame) {
            gImGui.waitForPreviousFrame(gApp);
        }
        firstFrame = false;

        if (hasNewGameFrame) {
            // GPU 可提交后再抓取游戏快照，减少数据在 fence wait 期间变旧。
            gameData = ReadGameData();
            if (gameData.valid) {
                if (gameData.frameCounter == 0) {
                    gameData.frameCounter = currentGameFrame;
                }

                if (lastSyncedGameFrame == 0 || gameData.frameCounter > lastSyncedGameFrame) {
                    syncedDeltaTime = ResolveSynchronizedDeltaTime(gameData);
                    haveRenderableGameFrame = true;
                }
            }
        }

        // 6. fence 释放后使用预读数据渲染
        ImGui_ProcessPendingTextureLoads();
        gImGui.beginFrame(gWindow, displayInfo.width, displayInfo.height, renderStepDeltaTime);

        if (gAutoAim) {
            gAutoAim->SetDisplaySize(static_cast<float>(displayInfo.width), static_cast<float>(displayInfo.height));
        }

        // 统计 FPS（每秒更新一次）— 只统计同步到游戏帧的渲染
        if (hasNewGameFrame) {
            gFrameCount++;
        }
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - gLastFPSUpdateTime).count();
        if (elapsed >= 1000) {  // 每 1000ms 更新一次
            gCurrentFPS = gFrameCount * 1000.0f / elapsed;
            gEffectiveFPS = static_cast<int>(std::lround(gCurrentFPS));
            gFrameCount = 0;
            gLastFPSUpdateTime = now;
        }

        if (haveRenderableGameFrame) {
            DrawObjectsWithData(gameData, renderStepDeltaTime);  // 绘制侧使用固定步长，避免受真实 DeltaTime 波动影响
        } else if (gHasLastRenderedGameData) {
            DrawObjectsWithData(gLastRenderedGameData, renderStepDeltaTime);
        }

        // 缓存游戏 FPS 统计（每帧一次，避免重复加锁）
        auto gameStats = GetGameFPSStats();

        // 自瞄更新由独立线程驱动，主线程不再调用

        Draw_Menu_Overlay();

        if (gAutoAim) gAutoAim->DrawDebug();

        if (menuOpen)
            Draw_Menu();

        DrawFPSCounter(gameStats);  // 显示 FPS 计数器（左下角）

        if (IsToolActive == 0) {
            gImGui.endFrame();
            gImGui.submitAndPresent(gApp);
            break;
        }

        gImGui.endFrame();
        gImGui.submitAndPresent(gApp);  // MAILBOX 模式非阻塞，降低延迟
        MarkUiFrameRendered();
        if (haveRenderableGameFrame) {
            lastSyncedGameFrame = gameData.frameCounter;
            gLastRenderedGameData = gameData;
            gHasLastRenderedGameData = true;
        }
        previousMenuOpen = menuOpen;
        previousTouchActive = touchActive;
    }

    // ── 清理 ──
    StopGameFPSMonitor();
    ShutdownAutoAim();
    StopReadThread();
    ShutdownDrawObjects();  // 清理绘制缓存
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
