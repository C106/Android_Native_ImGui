#include "draw_menu.h"
#include "draw_objects.h"
#include <logo.h>
#include "ImGuiLayer.h"
#include "Gyro.h"
#include "read_mem.h"
#include "driver_manager.h"
#include "ANativeWindowCreator.h"  // 用于 LayerStack 监控
#include "game_fps_monitor.h"
#include "auto_aim.h"
#include "TouchScrollable.h"


// From Main
extern std::atomic<bool> IsToolActive;
extern Gyro* Gyro_Controller;
extern VulkanApp gApp;
extern int gTargetFPS;

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

    // 在函数开始就保存原始鼠标状态（在任何控件修改之前）
    static bool was_mouse_down = false;
    bool original_mouse_down = io.MouseDown[0];
    ImVec2 original_mouse_pos = io.MousePos;

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
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
        ImGui::PopStyleVar(4);
        ImGui::End();
        was_mouse_down = original_mouse_down;
        return;
    }

    // 自定义窗口拖动：只在 Logo 和 Tab 区域可以拖动窗口
    ImVec2 window_pos = ImGui::GetWindowPos();
    ImVec2 drag_area_min = window_pos;
    ImVec2 drag_area_max;
    drag_area_max.x = window_pos.x + windowWidth;
    drag_area_max.y = window_pos.y + 120.0f;  // Logo + Tab 区域高度

    static bool is_dragging_window = false;
    static ImVec2 drag_offset;
    static ImVec2 drag_start_pos;
    static bool drag_start_in_area = false;

    bool in_drag_area = ImGui::IsMouseHoveringRect(drag_area_min, drag_area_max);

    // 触摸按下：记录起始位置和是否在拖动区域内
    if (original_mouse_down && !was_mouse_down) {
        drag_start_pos = original_mouse_pos;
        drag_start_in_area = in_drag_area;
        drag_offset.x = original_mouse_pos.x - window_pos.x;
        drag_offset.y = original_mouse_pos.y - window_pos.y;
    }

    // 拖动区域内开始 + 移动超阈值 + 滚动区域未活动 → 开始拖动窗口
    if (drag_start_in_area && original_mouse_down && !is_dragging_window) {
        float dx = original_mouse_pos.x - drag_start_pos.x;
        float dy = original_mouse_pos.y - drag_start_pos.y;
        if (sqrtf(dx * dx + dy * dy) > 5.0f && !TouchScrollable::IsScrolling())
            is_dragging_window = true;
    }

    if (is_dragging_window) {
        if (original_mouse_down) {
            ImVec2 new_pos;
            new_pos.x = original_mouse_pos.x - drag_offset.x;
            new_pos.y = original_mouse_pos.y - drag_offset.y;
            ImGui::SetWindowPos(new_pos);
        } else {
            is_dragging_window = false;
            drag_start_in_area = false;
        }
    }

    was_mouse_down = original_mouse_down;

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
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 18.0f));
    if (ImGui::BeginTabBar("MainTabBar", ImGuiTabBarFlags_None)) {
        float _tabWidth = (ImGui::GetContentRegionAvail().x - (3 - 1) * ImGui::GetStyle().ItemInnerSpacing.x) / 3 - 1;

        if (gIconFont) ImGui::PushFont(gIconFont);
        ImGui::SetNextItemWidth(_tabWidth);
        bool cameraOpen = ImGui::BeginTabItem("    s   ", nullptr, 0);
        if (gIconFont) ImGui::PopFont();
        if (cameraOpen) {
            TouchScrollable::Begin("CameraScrollRegion", ImVec2(0, 0));
            DrawCameraTab();
            TouchScrollable::End();
            ImGui::EndTabItem();
        }

        if (gIconFont) ImGui::PushFont(gIconFont);
        ImGui::SetNextItemWidth(_tabWidth);
        bool objViewOpen = ImGui::BeginTabItem("    t   ", nullptr, 0);
        if (gIconFont) ImGui::PopFont();
        if (objViewOpen) {
            TouchScrollable::Begin("ObjViewScrollRegion", ImVec2(0, 0));
            DrawObjViewTab();
            TouchScrollable::End();
            ImGui::EndTabItem();
        }

        if (gIconFont) ImGui::PushFont(gIconFont);
        ImGui::SetNextItemWidth(_tabWidth);
        bool configOpen = ImGui::BeginTabItem("    v   ", nullptr, 0);
        if (gIconFont) ImGui::PopFont();
        if (configOpen) {
            TouchScrollable::Begin("ConfigScrollRegion", ImVec2(0, 0));
            DrawConfigTab();
            TouchScrollable::End();
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

    ImGui::Separator();
    ImGui::Text("Auto-Aim:");

    if (!gAutoAim) {
        ImGui::TextDisabled("未初始化");
        return;
    }

    AutoAimConfig& cfg = gAutoAim->GetConfig();

    ImGui::Checkbox("启用", &cfg.enabled);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("使用 PD 控制器自动瞄准屏幕中心附近的目标");
        ImGui::Text("通过陀螺仪发送微调指令");
        ImGui::EndTooltip();
    }

    ImGui::Checkbox("仅开火时启用", &cfg.onlyWhenFiring);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("勾选后，只有在开火时才会自动瞄准");
        ImGui::Text("未开火时不会调整视角");
        ImGui::EndTooltip();
    }

    const char* boneNames[] = {"头部", "颈部", "胸部", "骨盆"};
    int boneIDs[] = {BONE_HEAD, BONE_NECK, BONE_CHEST, BONE_PELVIS};
    int currentBoneIdx = 0;
    for (int i = 0; i < 4; i++) {
        if (boneIDs[i] == cfg.targetBone) {
            currentBoneIdx = i;
            break;
        }
    }
    if (ImGui::Combo("目标骨骼", &currentBoneIdx, boneNames, 4)) {
        cfg.targetBone = boneIDs[currentBoneIdx];
    }

    ImGui::SliderFloat("最大距离 (米)", &cfg.maxDistance, 10.0f, 500.0f, "%.0f");
    ImGui::SliderFloat("FOV 限制 (度)", &cfg.fovLimit, 5.0f, 90.0f, "%.0f");

    ImGui::Text("PD 控制器参数:");
    ImGui::SliderFloat("X Kp", &cfg.KpX, 0.0f, 2.0f, "%.2f");
    ImGui::SliderFloat("X Kd", &cfg.KdX, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Y Kp", &cfg.KpY, 0.0f, 2.0f, "%.2f");
    ImGui::SliderFloat("Y Kd", &cfg.KdY, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("X 输出倍率", &cfg.outputScaleX, 0.5f, 1.8f, "%.2f");
    ImGui::SliderFloat("Y 输出倍率", &cfg.outputScaleY, 0.5f, 1.8f, "%.2f");

    ImGui::SliderFloat("目标切换阈值 (像素)", &cfg.hysteresisThreshold, 10.0f, 200.0f, "%.0f");
    ImGui::Checkbox("过滤队友", &cfg.filterTeammates);
    ImGui::Checkbox("显示调试信息", &cfg.drawDebug);

    const TargetState& state = gAutoAim->GetTargetState();
    if (cfg.enabled) {
        if (state.valid) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
                "状态: 锁定目标 (0x%llX)",
                (unsigned long long)state.actorAddr);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                "状态: 搜索目标...");
        }
    } else {
        ImGui::TextDisabled("状态: 已禁用");
    }
}

// Obj View Tab
void DrawObjViewTab() {
    ImGui::Text("Object View");
    ImGui::Separator();

    ImGui::Checkbox("Show Objects", &gShowObjects);

    // 分类显示开关
    ImGui::Separator();
    ImGui::Text("显示分类:");
    ImGui::Checkbox("玩家 (Players)", &gShowPlayers);
    ImGui::Checkbox("载具 (Vehicles)", &gShowVehicles);
    ImGui::Checkbox("其他 (Others)", &gShowOthers);

    // 绘制模块开关
    ImGui::Separator();
    ImGui::Text("绘制模块:");
    ImGui::Checkbox("骨骼 (Skeleton)", &gDrawSkeleton);
    ImGui::Checkbox("骨骼平滑", &gEnableBoneSmoothing);
    ImGui::Checkbox("名称 (Name)", &gDrawName);
    ImGui::Checkbox("距离 (Distance)", &gDrawDistance);
    ImGui::Checkbox("包围盒 (Box)", &gDrawBox);
    ImGui::SameLine();
    ImGui::TextDisabled("(预留)");
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
    ImGui::Text("Overlay 帧率:");
    ImGui::SliderInt("Target FPS", &gTargetFPS, 0, 144);
    ImGui::SameLine();
    if (gTargetFPS == 0) {
        ImGui::Text("无限制 (最低延迟)");
    } else {
        ImGui::Text("%d FPS", gTargetFPS);
    }
    ImGui::TextDisabled("提示: 会自动匹配游戏 FPS 设置，也可手动调整");
    ImGui::TextDisabled("设为 0 可获得最低延迟，但会增加 CPU 占用");

    // 骨骼绘制距离限制
    ImGui::Separator();
    ImGui::Text("骨骼绘制距离:");
    ImGui::SliderFloat("最大距离 (米)", &gMaxSkeletonDistance, 50.0f, 500.0f, "%.0f");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("超过此距离的角色不绘制骨骼，减少性能开销");
        ImGui::Text("人物名称会随距离变小、变透明（10-300米）");
        ImGui::EndTooltip();
    }

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

        // 扫描 CustomizeCanvasPanel_BP_C
        ImGui::Separator();
        if (ImGui::Button("扫描 Canvas", ImVec2(150, 50))) {
            ClearScanResults();
            ScanForClass("CustomizeCanvasPanel_BP_C");
        }
        ImGui::SameLine();
        if (ImGui::Button("查找 UClass", ImVec2(150, 50))) {
            ClearScanResults();
            FindUClass("SettingConfig");
        }
        if (ImGui::Button("读取 FPS Level", ImVec2(150, 50))) {
            FindSettingConfigViaGameInstance();
        }
        ImGui::SameLine();
        float gameFps = GetGameFPS();
        if (gameFps > 0.5f) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "游戏实际 FPS: %.0f", gameFps);
        } else {
            ImGui::TextDisabled("游戏实际 FPS: --");
        }

        // 显示扫描结果
        auto scanResults = GetScanResults();
        if (!scanResults.empty()) {
            ImGui::Text("找到 %zu 个实例:", scanResults.size());
            ImGui::BeginChild("ScanResults", ImVec2(0, 200), true, ImGuiWindowFlags_HorizontalScrollbar);
            for (size_t i = 0; i < scanResults.size(); i++) {
                ImGui::Text("[%zu] 0x%llX", i, (unsigned long long)scanResults[i].first);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", scanResults[i].second.c_str());
            }
            ImGui::EndChild();
        }

        ImGui::Separator();
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
