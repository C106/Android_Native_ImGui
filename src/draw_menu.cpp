#include "draw_menu.h"
#include <logo.h>  // Logo
#include "ImGuiLayer.h"
#include <android/log.h>
#include "Gyro.h"
// 简单的日志宏
#ifdef NDEBUG
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#else
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "draw_menu", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "draw_menu", __VA_ARGS__)
#endif

extern std::atomic<bool> IsToolActive;
extern Gyro* Gyro_Controller;
// 当前激活的标签页
static int gCurrentTab = 0;

// Logo 纹理
static ImTextureID gLogoTexture = (ImTextureID)0;
static int gLogoWidth = 0;
static int gLogoHeight = 0;

//陀螺仪坐标
float gyro_x=0,gyro_y=0;


void Draw_Menu_ResetTextures() {
    gLogoTexture = (ImTextureID)0;
    gLogoWidth = 0;
    gLogoHeight = 0;
}

void Draw_Menu(){
    Gyro_Controller->update(gyro_x,gyro_y);
    ImGuiIO& io = ImGui::GetIO();

    float windowWidth = 700.0f ;
    float windowHeight =  500.0f ;

    ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_Once);
    ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - windowWidth) / 2, (io.DisplaySize.y - windowHeight) / 2), ImGuiCond_Once);

    // 自定义样式
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 15.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 8.0f)); // 增加按钮内边距

    if (!ImGui::Begin("###MainWindow", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PopStyleVar(4);
        ImGui::End();
        return;
    }

    // 左侧 Logo（使用延迟加载）
    ImGui::SetCursorPos(ImVec2(20, 30));
    if (!gLogoTexture && aimware_png_len > 0) {
        LOGI("Requesting logo load: %d bytes", aimware_png_len);
        ImGui_RequestTextureLoad(aimware_png, aimware_png_len, &gLogoTexture, &gLogoWidth, &gLogoHeight);
        LOGI("Logo load requested, waiting for completion...");
    }
    if (gLogoTexture) {
        ImGui::Image(gLogoTexture, ImVec2(90, 90));
        LOGI("Logo displayed: %dx%d", gLogoWidth, gLogoHeight);
    }

    // 根据屏幕方向调整布局
    bool isLandscape = (io.DisplaySize.x > io.DisplaySize.y);
    float tabX = isLandscape ? 130.0f : 133.0f;
    float tabY = isLandscape ? 65.0f : 68.0f;

    // TabBar
    ImGui::SetCursorPos(ImVec2(tabX, tabY));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f, 13.0f));
    if (ImGui::BeginTabBar("MainTabBar", ImGuiTabBarFlags_None)) {
        float _tabWidth = (ImGui::GetContentRegionAvail().x - (3 - 1) * ImGui::GetStyle().ItemInnerSpacing.x) / 3 - 1;
        // === Camera Tab ===
        if (gIconFont) ImGui::PushFont(gIconFont);
        ImGui::SetNextItemWidth(_tabWidth);
        bool cameraOpen = ImGui::BeginTabItem("    s   ", nullptr, 0);
        if (gIconFont) ImGui::PopFont();
        if (cameraOpen) {
            DrawCameraTab();
            ImGui::EndTabItem();
        }

        // === Obj View Tab ===
        if (gIconFont) ImGui::PushFont(gIconFont);
        ImGui::SetNextItemWidth(_tabWidth);
        bool objViewOpen = ImGui::BeginTabItem("    t   ", nullptr, 0);
        if (gIconFont) ImGui::PopFont();
        if (objViewOpen) {
            DrawObjViewTab();
            ImGui::EndTabItem();
        }

        // === Config Tab ===
        if (gIconFont) ImGui::PushFont(gIconFont);
        ImGui::SetNextItemWidth(_tabWidth);
        bool configOpen = ImGui::BeginTabItem("    v   ", nullptr, 0);
        if (gIconFont) ImGui::PopFont();
        if (configOpen) {
            DrawConfigTab();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();

    ImGui::PopStyleVar(4);
    ImGui::End();


}

// Camera Tab 内容
void DrawCameraTab() {
    ImGui::Text("Camera Settings");
    ImGui::Separator();

    

    // 显示触摸坐标（已经是屏幕坐标空间）
    ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("Touch: (%.1f, %.1f)", io.MousePos.x, io.MousePos.y);
    ImGui::SliderFloat("Gyro_x", &gyro_x, -10, 10, nullptr);
    ImGui::SliderFloat("Gyro_y", &gyro_y, -10, 10, nullptr);
}

// Obj View Tab 内容
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

    // 坐标显示（已经是屏幕坐标空间）
    ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("Screen: (%.1f, %.1f)", io.MousePos.x, io.MousePos.y);
}

// Config Tab 内容
void DrawConfigTab() {
    ImGui::Text("Configuration");
    ImGui::Separator();
    // 帧率滑动条
    ImGui::SliderInt("Target FPS", &gTargetFPS, 1, 144);
    ImGui::SameLine();
    ImGui::Text("%d FPS", gTargetFPS);
    if (ImGui::Button("退出", ImVec2(100, 50))) { 
        IsToolActive.store(false);
    }
}