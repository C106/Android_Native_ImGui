#include "draw_menu.h"
#include "draw_objects.h"
#include <logo.h>
#include "ImGuiLayer.h"
#include <android/log.h>
#include "Gyro.h"
#include "read_mem.h"
#include "driver_manager.h"
#include "ANativeWindowCreator.h"  // 用于 LayerStack 监控

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

void Draw_Menu_ResetTextures() {
    gLogoTexture = (ImTextureID)0;
    gLogoWidth = 0;
    gLogoHeight = 0;
}

void Draw_Menu() {
    //Gyro_Controller->update(gyro_x, gyro_y);
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

    // Logo claude --resume e3e0af5f-8444-48d7-b85e-60527b933e42
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
            ImGui::BeginChild("ObjViewScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
            DrawObjViewTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (gIconFont) ImGui::PushFont(gIconFont);
        ImGui::SetNextItemWidth(_tabWidth);
        bool configOpen = ImGui::BeginTabItem("    v   ", nullptr, 0);
        if (gIconFont) ImGui::PopFont();
        if (configOpen) {
            ImGui::BeginChild("ConfigScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
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

    ImGui::Checkbox("Show Objects", &gShowObjects);
}

// Config Tab — 纯绘制
void DrawConfigTab() {
    ImGui::Text("Configuration");
    ImGui::Separator();

    ImGui::Checkbox("显示所有类名", &gShowAllClassNames);

    // 骨骼读取模式切换
    ImGui::Separator();
    ImGui::Text("骨骼读取模式:");
    if (ImGui::Checkbox("批量读取（优化）", &gUseBatchBoneRead)) {
        // 切换时自动更新
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("批量读取: 1次ioctl读取整个骨骼数组（推荐）");
        ImGui::Text("逐个读取: 每个骨骼1次ioctl（调试用）");
        ImGui::EndTooltip();
    }

    ImGui::Separator();
    ImGui::SliderInt("Target FPS", &gTargetFPS, 0, 144);
    ImGui::SameLine();
    if (gTargetFPS == 0) {
        ImGui::Text("无限制 (最低延迟)");
    } else {
        ImGui::Text("%d FPS", gTargetFPS);
    }
    ImGui::TextDisabled("提示: 设为 0 可获得最低延迟，但会增加 CPU 占用");

    ImGuiIO& io = ImGui::GetIO();

    // 驱动选择
    ImGui::Separator();
    ImGui::Text("驱动选择:");
    int driverType = (int)Paradise_hook->getType();
    if (ImGui::RadioButton("RT Hook", &driverType, DRIVER_RT_HOOK)) {
        StopReadThread();
        driver_stat.store(0, std::memory_order_release);
        Paradise_hook->switchDriver(DRIVER_RT_HOOK);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Paradise Hook", &driverType, DRIVER_PARADISE)) {
        StopReadThread();
        driver_stat.store(0, std::memory_order_release);
        Paradise_hook->switchDriver(DRIVER_PARADISE);
    }

    ImGui::Separator();

    // "mem" 按钮：初始化 driver
    if (ImGui::Button("mem", ImVec2(100, 50))) {
        InitDriver("com.tencent.tmgp.pubgmhd", libUE4);

    }

    if (driver_stat.load(std::memory_order_relaxed) <= 0) {
        ImGui::Text("未初始化");
    } else {
        if (ImGui::Button("Dump TArray", ImVec2(100, 50))) {
            DumpTArray();
        }
        ImGui::SameLine();
        if (ImGui::Button("Dump Bones", ImVec2(100, 50))) {
            DumpBones();
        }
        ImGui::Text("骨骼数: %d", gBoneCount);
        ReadFrameData info;
        gFrameSync.peek(info);
        if (info.valid) {
            ImGui::Text("%lX Uworld", info.uworld);
            ImGui::Text("%lX Ulevel", info.persistentLevel);
            ImGui::Text("%d count", info.actorCount);
        } else {
            ImGui::Text("等待数据...");
        }
    }

    if (ImGui::Button("退出", ImVec2(100, 50))) {
        IsToolActive.store(false);
    }

    // 手动触发 LayerStack 检测和 mirror 创建（一次性扫描）
    ImGui::Separator();
    ImGui::Text("录屏支持:");
    if (ImGui::Button("检测虚拟显示", ImVec2(150, 50))) {
        android::ANativeWindowCreator::DetectAndCreateVirtualDisplayMirrors();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("开始录屏后点击此按钮");
        ImGui::Text("会检测录屏创建的虚拟显示");
        ImGui::Text("并自动创建镜像层以支持录屏捕获");
        ImGui::Text("(一次性扫描，不会持续占用CPU)");
        ImGui::EndTooltip();
    }
}
