#include "draw_menu.h"
#include <logo.h>
#include "ImGuiLayer.h"
#include <android/log.h>
#include "Gyro.h"
#include "read_mem.h"

#ifdef NDEBUG
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#else
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "draw_menu", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "draw_menu", __VA_ARGS__)
#endif

// From Main
extern std::atomic<bool> IsToolActive;
extern Gyro* Gyro_Controller;
extern VulkanApp gApp;
extern Paradise_hook_driver* Paradise_hook;

// Logo 纹理
static ImTextureID gLogoTexture = (ImTextureID)0;
static int gLogoWidth = 0;
static int gLogoHeight = 0;

// 陀螺仪坐标
float gyro_x = 0, gyro_y = 0;

// driver / address 定义
std::atomic<int> driver_stat{0};
Offsets offset;
Addresses address;

// UI 线程持有的 libUE4（仅 mem 按钮初始化时写入）
static uint64_t libUE4 = 0;

// 渲染线程持有的当前帧
static ReadFrameData gRenderFrame;

void Draw_Menu_ResetTextures() {
    gLogoTexture = (ImTextureID)0;
    gLogoWidth = 0;
    gLogoHeight = 0;
}

void Draw_Menu() {
    Gyro_Controller->update(gyro_x, gyro_y);
    ImGuiIO& io = ImGui::GetIO();

    float windowWidth = 700.0f;
    float windowHeight = 800.0f;

    ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_Once);
    ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - windowWidth) / 2, (io.DisplaySize.y - windowHeight) / 2), ImGuiCond_Once);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 15.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 8.0f));

    if (!ImGui::Begin("###MainWindow", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PopStyleVar(4);
        ImGui::End();
        return;
    }

    // Logo
    ImGui::SetCursorPos(ImVec2(20, 30));
    if (!gLogoTexture && aimware_png_len > 0) {
        ImGui_RequestTextureLoad(aimware_png, aimware_png_len, &gLogoTexture, &gLogoWidth, &gLogoHeight);
    }
    if (gLogoTexture) {
        ImGui::Image(gLogoTexture, ImVec2(90, 90));
    }

    bool isLandscape = (io.DisplaySize.x > io.DisplaySize.y);
    float tabX = isLandscape ? 130.0f : 133.0f;
    float tabY = isLandscape ? 65.0f : 68.0f;

    ImGui::SetCursorPos(ImVec2(tabX, tabY));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f, 13.0f));
    if (ImGui::BeginTabBar("MainTabBar", ImGuiTabBarFlags_None)) {
        float _tabWidth = (ImGui::GetContentRegionAvail().x - (3 - 1) * ImGui::GetStyle().ItemInnerSpacing.x) / 3 - 1;

        if (gIconFont) ImGui::PushFont(gIconFont);
        ImGui::SetNextItemWidth(_tabWidth);
        bool cameraOpen = ImGui::BeginTabItem("    s   ", nullptr, 0);
        if (gIconFont) ImGui::PopFont();
        if (cameraOpen) {
            DrawCameraTab();
            ImGui::EndTabItem();
        }

        if (gIconFont) ImGui::PushFont(gIconFont);
        ImGui::SetNextItemWidth(_tabWidth);
        bool objViewOpen = ImGui::BeginTabItem("    t   ", nullptr, 0);
        if (gIconFont) ImGui::PopFont();
        if (objViewOpen) {
            ImGui::BeginChild("CameraScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
            DrawObjViewTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (gIconFont) ImGui::PushFont(gIconFont);
        ImGui::SetNextItemWidth(_tabWidth);
        bool configOpen = ImGui::BeginTabItem("    v   ", nullptr, 0);
        if (gIconFont) ImGui::PopFont();
        if (configOpen) {
            ImGui::BeginChild("CameraScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
            DrawConfigTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleVar(4);
    ImGui::End();
}

// Camera Tab
void DrawCameraTab() {
    ImGui::Text("Camera Settings");
    ImGui::Separator();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("Touch: (%.1f, %.1f)", io.MousePos.x, io.MousePos.y);
    ImGui::SliderFloat("Gyro_x", &gyro_x, -10, 10, nullptr);
    ImGui::SliderFloat("Gyro_y", &gyro_y, -10, 10, nullptr);
}

// Obj View Tab
void DrawObjViewTab() {
    ImGui::Text("Object View");
    ImGui::Separator();

    static float samples[100];
    float time = ImGui::GetTime();
    for (int n = 0; n < 100; n++) {
        samples[n] = sinf(n * 0.2f + time * 1.5f);
    }
    ImGui::PlotLines("Wave", samples, 100);

    static float colors[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ImGui::ColorEdit4("Color", colors);

    ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("Screen: (%.1f, %.1f)", io.MousePos.x, io.MousePos.y);
}

// Config Tab — 纯绘制
void DrawConfigTab() {
    ImGui::Text("Configuration");
    ImGui::Separator();
    ImGui::SliderInt("Target FPS", &gTargetFPS, 1, 144);
    ImGui::SameLine();
    ImGui::Text("%d FPS", gTargetFPS);
    ImGuiIO& io = ImGui::GetIO();

    // "mem" 按钮：初始化 driver
    if (ImGui::Button("mem", ImVec2(100, 50))) {
        InitDriver("com.mycompany.EngineTest", libUE4);
    }

    if (driver_stat.load(std::memory_order_relaxed) <= 0) {
        ImGui::Text("未初始化");
    } else {
        gFrameSync.fetch(gRenderFrame);

        if (gRenderFrame.valid) {
            ImGui::Text("%lX Uworld", gRenderFrame.uworld);
            ImGui::Text("%lX Ulevel", gRenderFrame.persistentLevel);
            ImGui::Text("%d count", gRenderFrame.actorCount);

            // VP 矩阵渲染帧实时读取
            FMatrix VPMat;
            Paradise_hook->read(address.Matrix, &VPMat, sizeof(FMatrix));

            float AspectRatio = io.DisplaySize.x / io.DisplaySize.y;
            ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
            Vec2 screenPos;
            for (const auto& actor : gRenderFrame.actors) {
                if (WorldToScreen(actor.worldPos, VPMat, io.DisplaySize.x, io.DisplaySize.y, screenPos)) {
                    draw_list->AddCircleFilled(
                        ImVec2(screenPos.x, screenPos.y), 3.0f, IM_COL32(255, 0, 0, 255));
                }
            }
        } else {
            ImGui::Text("等待数据...");
        }
    }

    if (ImGui::Button("退出", ImVec2(100, 50))) {
        IsToolActive.store(false);
    }
}
