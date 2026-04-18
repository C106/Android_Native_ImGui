#include "draw_menu.h"
#include "draw_objects.h"
#include <banner.h>
#include <logo.h>
#include "ImGuiLayer.h"
#include "Gyro.h"
#include "read_mem.h"
#include "driver_manager.h"
#include "ANativeWindowCreator.h"  // 用于 LayerStack 监控
#include "game_fps_monitor.h"
#include "auto_aim.h"
#include "HwBreakpointMgr4.h"
#include "TouchScrollable.h"
#include "menu_framework.h"
#include "hook_touch_event.h"
#include <cstring>
#include <filesystem>
#include <thread>
#include <chrono>
#include <random>
#include <sstream>
#include <array>
#include <unordered_map>
#include <string>
#include <vector>


// From Main
extern std::atomic<bool> IsToolActive;
extern std::atomic<bool> IsMenuOpen;
extern Gyro* Gyro_Controller;
extern VulkanApp gApp;
extern int gTargetFPS;

// Logo 纹理
static ImTextureID gLogoTexture = (ImTextureID)0;
static int gLogoWidth = 0;
static int gLogoHeight = 0;
static ImTextureID gBannerTexture = (ImTextureID)0;
static int gBannerWidth = 0;
static int gBannerHeight = 0;

// 陀螺仪坐标
float gyro_x = 0, gyro_y = 0;

// driver / address 定义
std::atomic<int> driver_stat{0};
Offsets offset;
Addresses address;

// UI 线程持有的 libUE4（仅 mem 按钮初始化时写入）
static uint64_t libUE4 = 0;
static bool gShowPhysXDebugWindow = false;
static bool gShowBulletSpreadDebugWindow = false;
static int gBulletBreakpointTargetUi = 0;
static int gMainTabIndex = 0;
static bool gMenuFrameworkRegistered = false;
static std::string gConfigStatusMessage;
static std::string gTouchTestStatusMessage;
static int gDriverTypeUi = DRIVER_RT_HOOK;
static bool gTriggerBotFireButtonPickerActive = false;
static ImVec2 gLastMenuPos(-1.0f, -1.0f);  // 记录 menu 关闭时的位置

static bool IsAnyActiveTouchInsideRect(const ImVec2& min, const ImVec2& max);

static bool DrawIconActionButton(const char* id, const char* icon, const ImVec2& pos, const ImVec2& size,
                                 const ImVec4& fill_color, const ImVec4& border_color) {
    ImGui::PushID(id);
    ImGui::SetCursorPos(pos);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    const bool touch_inside = IsAnyActiveTouchInsideRect(min, max);

    ImGui::InvisibleButton("##icon_action", size);
    const bool hovered = touch_inside || ImGui::IsItemHovered();
    static std::unordered_map<ImGuiID, bool> s_touch_latched;
    const ImGuiID touch_id = ImGui::GetID("##icon_action_touch");
    const bool touch_clicked = touch_inside && !s_touch_latched[touch_id];
    s_touch_latched[touch_id] = touch_inside;
    const bool pressed = ImGui::IsItemClicked() || touch_clicked;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec4 fill = fill_color;
    if (hovered) {
        fill.x = std::min(fill.x + 0.05f, 1.0f);
        fill.y = std::min(fill.y + 0.05f, 1.0f);
        fill.z = std::min(fill.z + 0.05f, 1.0f);
        fill.w = std::min(fill.w + 0.10f, 1.0f);
    }
    draw_list->AddRectFilled(min, max, ImGui::GetColorU32(fill), size.y * 0.35f);
    draw_list->AddRect(min, max, ImGui::GetColorU32(border_color), size.y * 0.35f, 0, hovered ? 2.2f : 1.4f);

    const float icon_font_size = ImGui::GetFontSize() * 1.15f;
    const ImVec2 icon_size = ImGui::CalcTextSize(icon);
    draw_list->AddText(ImVec2(min.x + (size.x - icon_size.x) * 0.5f,
                              min.y + (size.y - icon_size.y) * 0.5f - 1.0f),
                       IM_COL32(248, 252, 255, 255), icon);

    ImGui::PopID();
    return pressed;
}

static void DrawFloatingMenuBall() {
    if (IsMenuOpen.load(std::memory_order_relaxed)) return;

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 ball_size(80.0f, 80.0f);
    static ImVec2 ball_pos(-1.0f, -1.0f);
    static bool pressing = false;
    static bool moved = false;
    static ImVec2 drag_offset(0.0f, 0.0f);
    static ImVec2 press_origin(0.0f, 0.0f);
    static int active_touch_id = -1;
    static bool was_touch_inside = false;
    if (ball_pos.x < 0.0f || ball_pos.y < 0.0f) {
        if (gLastMenuPos.x >= 0.0f && gLastMenuPos.y >= 0.0f) {
            ball_pos = gLastMenuPos;
        } else {
            ball_pos = ImVec2(io.DisplaySize.x - ball_size.x - 26.0f, io.DisplaySize.y * 0.36f);
        }
    }
    ball_pos.x = std::clamp(ball_pos.x, 8.0f, std::max(8.0f, io.DisplaySize.x - ball_size.x - 8.0f));
    ball_pos.y = std::clamp(ball_pos.y, 8.0f, std::max(8.0f, io.DisplaySize.y - ball_size.y - 8.0f));

    ImGui::SetNextWindowPos(ball_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ball_size, ImGuiCond_Always);
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (!ImGui::Begin("##FloatingMenuBall", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + ball_size.x, min.y + ball_size.y);
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);

    ImGui::InvisibleButton("##floating_menu_ball_surface", ball_size);
    TouchPoint touches[10];
    const int touch_count = has_active_touch_points() ? get_active_touch_points(touches, 10) : 0;
    bool touch_inside = false;
    bool tracked_touch_active = false;
    ImVec2 touch_pos(0.0f, 0.0f);
    for (int i = 0; i < touch_count; ++i) {
        if (!touches[i].active) continue;
        if (active_touch_id >= 0 && touches[i].id == active_touch_id) {
            tracked_touch_active = true;
            touch_pos = ImVec2(touches[i].x, touches[i].y);
        }
        if (touches[i].x >= min.x && touches[i].x <= max.x &&
            touches[i].y >= min.y && touches[i].y <= max.y) {
            touch_inside = true;
            if (active_touch_id < 0) {
                touch_pos = ImVec2(touches[i].x, touches[i].y);
            }
        }
    }
    const bool hovered = touch_inside || tracked_touch_active || ImGui::IsItemHovered();
    const bool mouse_clicked = ImGui::IsItemClicked();
    const bool mouse_held = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool use_touch_logic = touch_count > 0;
    const bool clicked = use_touch_logic ? (touch_inside && !was_touch_inside && !pressing) : mouse_clicked;
    const bool held = use_touch_logic ? (tracked_touch_active || touch_inside) : mouse_held;
    const ImVec2 pointer_pos = use_touch_logic ? touch_pos : io.MousePos;

    if (clicked) {
        active_touch_id = -1;
        ImVec2 new_touch_pos = pointer_pos;
        if (use_touch_logic) {
            for (int i = 0; i < touch_count; ++i) {
                if (!touches[i].active) continue;
                if (touches[i].x >= min.x && touches[i].x <= max.x &&
                    touches[i].y >= min.y && touches[i].y <= max.y) {
                    active_touch_id = touches[i].id;
                    new_touch_pos = ImVec2(touches[i].x, touches[i].y);
                    break;
                }
            }
        }
        pressing = true;
        moved = false;
        drag_offset = ImVec2(new_touch_pos.x - ball_pos.x, new_touch_pos.y - ball_pos.y);
        press_origin = new_touch_pos;
    }
    was_touch_inside = touch_inside;

    if (pressing && held) {
        const float dx = pointer_pos.x - press_origin.x;
        const float dy = pointer_pos.y - press_origin.y;
        if (!moved && (dx * dx + dy * dy) > 100.0f) {
            moved = true;
        }
        if (moved) {
            ball_pos.x = pointer_pos.x - drag_offset.x;
            ball_pos.y = pointer_pos.y - drag_offset.y;
        }
    } else if (pressing && !held) {
        pressing = false;
        active_touch_id = -1;
        if (!moved && hovered) {
            ball_pos = ImVec2(-1.0f, -1.0f);
            IsMenuOpen.store(true, std::memory_order_relaxed);
        }
    }

    const ImU32 outer = ImGui::GetColorU32(hovered ? ImVec4(0.18f, 0.46f, 0.84f, 0.94f)
                                                   : ImVec4(0.12f, 0.32f, 0.68f, 0.84f));
    const ImU32 inner = ImGui::GetColorU32(hovered ? ImVec4(0.34f, 0.76f, 1.00f, 0.98f)
                                                   : ImVec4(0.24f, 0.64f, 0.98f, 0.94f));
    draw_list->AddCircleFilled(center, 40.0f, outer, 48);
    draw_list->AddCircleFilled(center, 32.0f, inner, 48);
    draw_list->AddCircle(center, 40.0f, IM_COL32(180, 228, 255, 235), 48, 2.4f);
    draw_list->AddCircleFilled(ImVec2(center.x - 8.0f, center.y - 8.0f), 4.0f, IM_COL32(245, 250, 255, 255), 16);
    draw_list->AddCircleFilled(ImVec2(center.x + 8.0f, center.y - 8.0f), 4.0f, IM_COL32(245, 250, 255, 255), 16);
    draw_list->AddCircleFilled(ImVec2(center.x, center.y + 8.0f), 4.0f, IM_COL32(245, 250, 255, 255), 16);

    ImGui::End();
}

static bool IsAnyActiveTouchInsideRect(const ImVec2& min, const ImVec2& max) {
    if (!has_active_touch_points()) return false;
    TouchPoint touches[10];
    const int count = get_active_touch_points(touches, 10);
    for (int i = 0; i < count; ++i) {
        if (!touches[i].active) continue;
        if (touches[i].x >= min.x && touches[i].x <= max.x &&
            touches[i].y >= min.y && touches[i].y <= max.y) {
            return true;
        }
    }
    return false;
}

static bool DrawMainTabButton(const char* id, const char* icon, int tab_index, float width, float height) {
    ImGui::PushID(id);
    const ImVec2 button_size(width, height);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + button_size.x, min.y + button_size.y);
    const bool touch_inside = IsAnyActiveTouchInsideRect(min, max);

    ImGui::InvisibleButton("##tab_button", button_size);
    const bool hovered = touch_inside || ImGui::IsItemHovered();
    const bool selected = (gMainTabIndex == tab_index);

    static std::array<bool, 3> s_touch_latched = {false, false, false};
    const bool mouse_clicked = ImGui::IsItemClicked();
    const bool touch_clicked = touch_inside && !s_touch_latched[tab_index];
    if (mouse_clicked || touch_clicked) {
        gMainTabIndex = tab_index;
    }
    s_touch_latched[tab_index] = touch_inside;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImU32 fill = ImGui::GetColorU32(
        selected ? ImVec4(0.16f, 0.30f, 0.48f, 0.88f)
                 : (hovered ? ImVec4(0.18f, 0.32f, 0.50f, 0.78f)
                            : ImVec4(0.10f, 0.18f, 0.30f, 0.58f)));
    const ImU32 border = ImGui::GetColorU32(
        selected ? ImVec4(0.50f, 0.82f, 1.00f, 0.92f)
                 : ImVec4(0.16f, 0.36f, 0.58f, hovered ? 0.82f : 0.56f));
    draw_list->AddRectFilled(min, max, fill, 8.0f);
    draw_list->AddRect(min, max, border, 8.0f, 0, selected ? 2.4f : 1.6f);

    ImFont* text_font = gIconFont ? gIconFont : ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const ImVec2 text_size = text_font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, icon);
    const ImVec2 text_pos(min.x + (button_size.x - text_size.x) * 0.5f,
                          min.y + (button_size.y - text_size.y) * 0.5f - 1.0f);
    draw_list->AddText(text_font, font_size, text_pos,
                       ImGui::GetColorU32(selected ? ImVec4(0.96f, 0.99f, 1.00f, 1.0f)
                                                   : ImVec4(0.82f, 0.92f, 1.00f, 0.96f)),
                       icon);

    ImGui::PopID();
    return selected;
}

static std::string GetConfigPathString() {
    return MenuRegistry::Instance().GetDefaultConfigPath().string();
}

static void SetConfigStatus(const MenuConfigResult& result) {
    gConfigStatusMessage = result.message;
    if (!result.success) {
        gConfigStatusMessage += " [errors=" + std::to_string(result.errors) + "]";
        return;
    }
    if (result.applied > 0) {
        gConfigStatusMessage += " [applied=" + std::to_string(result.applied) + "]";
    }
}

static std::string GetAutoAimStatusText() {
    if (!gAutoAim) {
        return "未初始化";
    }

    const AutoAimConfig& cfg = gAutoAim->GetConfig();
    const TargetState& state = gAutoAim->GetTargetState();
    if (!cfg.enabled) {
        return "状态: 已禁用";
    }
    if (!state.valid) {
        return "状态: 搜索目标...";
    }

    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "状态: 锁定目标\nActor: 0x%llX",
                  static_cast<unsigned long long>(state.actorAddr));
    return buffer;
}

static std::string GetDisplaySyncStatusText() {
    std::ostringstream out;
    out << "跟随游戏帧率: " << gTargetFPS << " FPS";
    return out.str();
}

static std::string GetDriverMemoryStatusText() {
    if (driver_stat.load(std::memory_order_relaxed) <= 0) {
        return "未初始化";
    }

    std::ostringstream out;
    out << "驱动已初始化\n骨骼数: " << gBoneCount;
    return out.str();
}

static std::string GetScanDataStatusText() {
    if (driver_stat.load(std::memory_order_relaxed) <= 0) {
        return "驱动未初始化";
    }

    std::ostringstream out;
    const float gameFps = GetGameFPS();
    if (gameFps > 0.5f) {
        out << "游戏实际 FPS: " << static_cast<int>(gameFps + 0.5f) << "\n";
    } else {
        out << "游戏实际 FPS: --\n";
    }

    ReadFrameData info{};
    gFrameSync.peek(info);
    if (info.valid) {
        out << std::hex
            << "UWorld: 0x" << info.uworld << "\n"
            << "ULevel: 0x" << info.persistentLevel << "\n"
            << std::dec
            << "ActorCount: " << info.actorCount;
    } else {
        out << "等待数据...";
    }
    return out.str();
}

static std::string GetToolStatusText() {
    ImGuiIO& io = ImGui::GetIO();
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "Touch: (%.1f, %.1f)", io.MousePos.x, io.MousePos.y);
    return buffer;
}

static void ApplyDriverTypeSelection() {
    const DriverType current = GetDriverManager().getType();
    const DriverType desired = static_cast<DriverType>(gDriverTypeUi);
    if (current == desired) return;

    StopReadThread();
    driver_stat.store(0, std::memory_order_release);
    GetDriverManager().switchDriver(desired);
}

static bool HasGyroSocketConnection() {
    return Gyro_Controller && Gyro_Controller->bGyroConnect();
}

static bool CurrentDriverSupportsGyroUpdate() {
    return GetDriverManager().getType() == DRIVER_PARADISE;
}

static bool CurrentDriverSupportsTouch() {
    return GetDriverManager().supports_touch();
}

static bool IsCameraPageGyroUnsupported() {
    return !HasGyroSocketConnection() && !CurrentDriverSupportsGyroUpdate();
}

static std::string GetTouchTestStatusText() {
    return gTouchTestStatusMessage.empty() ? std::string("未执行") : gTouchTestStatusMessage;
}

static void RunTouchTest() {
    if (!GetDriverManager().supports_touch()) {
        gTouchTestStatusMessage = "当前驱动不支持 touch";
        return;
    }

    int max_x = 0;
    int max_y = 0;
    if (!GetDriverManager().touch_init(&max_x, &max_y) || max_x <= 0 || max_y <= 0) {
        gTouchTestStatusMessage = "touch_init 失败";
        return;
    }

    std::mt19937 rng(static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    auto rand_in_range = [&rng](int min_value, int max_value) -> int {
        std::uniform_int_distribution<int> dist(min_value, max_value);
        return dist(rng);
    };

    ImGuiIO& io = ImGui::GetIO();
    const float start_screen_x = rand_in_range(static_cast<int>(io.DisplaySize.x * 0.65f),
                                               static_cast<int>(io.DisplaySize.x * 0.88f));
    const float start_screen_y = rand_in_range(static_cast<int>(io.DisplaySize.y * 0.42f),
                                               static_cast<int>(io.DisplaySize.y * 0.72f));
    const float end_screen_x = std::clamp(start_screen_x + static_cast<float>(rand_in_range(
                                               -static_cast<int>(io.DisplaySize.x / 20.0f),
                                               static_cast<int>(io.DisplaySize.x / 20.0f))),
                                          0.0f, io.DisplaySize.x);
    const float end_screen_y = std::clamp(start_screen_y + static_cast<float>(rand_in_range(
                                               -static_cast<int>(io.DisplaySize.y / 18.0f),
                                               static_cast<int>(io.DisplaySize.y / 18.0f))),
                                          0.0f, io.DisplaySize.y);
    const int slot = 8;
    int start_x = 0;
    int start_y = 0;
    int end_x = 0;
    int end_y = 0;
    if (!MapScreenToTouch(start_screen_x, start_screen_y, start_x, start_y) ||
        !MapScreenToTouch(end_screen_x, end_screen_y, end_x, end_y)) {
        GetDriverManager().touch_destroy();
        gTouchTestStatusMessage = "screen/touch 坐标转换失败";
        return;
    }
    const int move_steps = rand_in_range(3, 6);

    if (!GetDriverManager().touch_down(slot, start_x, start_y)) {
        GetDriverManager().touch_destroy();
        gTouchTestStatusMessage = "touch_down 失败";
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(rand_in_range(25, 45)));

    for (int step = 1; step <= move_steps; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(move_steps);
        const int move_x = static_cast<int>(std::lround(start_x + (end_x - start_x) * t));
        const int move_y = static_cast<int>(std::lround(start_y + (end_y - start_y) * t));
        if (!GetDriverManager().touch_move(slot, move_x, move_y)) {
            GetDriverManager().touch_destroy();
            gTouchTestStatusMessage = "touch_move 失败";
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(rand_in_range(8, 18)));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(rand_in_range(20, 35)));

    if (!GetDriverManager().touch_up(slot)) {
        GetDriverManager().touch_destroy();
        gTouchTestStatusMessage = "touch_up 失败";
        return;
    }

    GetDriverManager().touch_destroy();

    char buffer[192];
    std::snprintf(buffer, sizeof(buffer),
                  "测试成功: slot=%d (%d,%d)->(%d,%d) steps=%d",
                  slot, start_x, start_y, end_x, end_y, move_steps);
    gTouchTestStatusMessage = buffer;
}

static std::string GetTriggerBotFireButtonStatusText() {
    if (!gAutoAim) {
        return "未初始化";
    }

    const AutoAimConfig& cfg = gAutoAim->GetConfig();
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "当前开火键: X %.1f%% / Y %.1f%%",
                  cfg.triggerBotFireButtonX * 100.0f,
                  cfg.triggerBotFireButtonY * 100.0f);
    return buffer;
}

static void BeginTriggerBotFireButtonPicker() {
    if (!CurrentDriverSupportsTouch() || !gAutoAim) {
        gTriggerBotFireButtonPickerActive = false;
        return;
    }
    gTriggerBotFireButtonPickerActive = true;
}

static void DrawTriggerBotFireButtonPickerOverlay() {
    if (!gTriggerBotFireButtonPickerActive || !gAutoAim) return;

    AutoAimConfig& cfg = gAutoAim->GetConfig();
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();

    const ImVec2 center(
        std::clamp(cfg.triggerBotFireButtonX, 0.0f, 1.0f) * io.DisplaySize.x,
        std::clamp(cfg.triggerBotFireButtonY, 0.0f, 1.0f) * io.DisplaySize.y);

    draw_list->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize,
                             IM_COL32(12, 16, 22, 180));
    draw_list->AddCircleFilled(center, 20.0f, IM_COL32(50, 170, 255, 180), 32);
    draw_list->AddCircle(center, 30.0f, IM_COL32(130, 210, 255, 255), 48, 3.0f);
    draw_list->AddLine(ImVec2(center.x - 42.0f, center.y), ImVec2(center.x + 42.0f, center.y),
                       IM_COL32(130, 210, 255, 255), 3.0f);
    draw_list->AddLine(ImVec2(center.x, center.y - 42.0f), ImVec2(center.x, center.y + 42.0f),
                       IM_COL32(130, 210, 255, 255), 3.0f);

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (ImGui::Begin("##TriggerBotFireButtonPicker", nullptr, flags)) {
        ImGui::SetCursorScreenPos(ImVec2(0.0f, 0.0f));
        ImGui::InvisibleButton("##trigger_bot_picker_surface", io.DisplaySize);

        const ImVec2 label_pos(28.0f, 28.0f);
        draw_list->AddText(label_pos, IM_COL32(255, 255, 255, 255), "Tap to set Trigger Bot fire button");
        draw_list->AddText(ImVec2(label_pos.x, label_pos.y + 26.0f),
                           IM_COL32(170, 200, 230, 255),
                           "Release outside the menu area to cancel");

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const ImVec2 pos = io.MousePos;
            cfg.triggerBotFireButtonX = std::clamp(pos.x / std::max(io.DisplaySize.x, 1.0f), 0.0f, 1.0f);
            cfg.triggerBotFireButtonY = std::clamp(pos.y / std::max(io.DisplaySize.y, 1.0f), 0.0f, 1.0f);
            gTriggerBotFireButtonPickerActive = false;
        }
    }
    ImGui::End();
}

static void RegisterCameraPage(MenuRegistry& registry) {
    if (!gAutoAim) return;

    AutoAimConfig& cfg = gAutoAim->GetConfig();
    MenuPageSpec& page = registry.AddPage("camera", "Camera");

    MenuSectionSpec& autoAim = page.AddSection("auto_aim", "自动瞄准", MenuColumn::Left);
    autoAim.AddBool("enabled", "启用", &cfg.enabled)
        .Tooltip("使用 PD 控制器自动瞄准屏幕中心附近的目标，通过陀螺仪发送微调指令")
        .OnChange([] {
            if (gAutoAim && !gAutoAim->GetConfig().enabled) {
                gAutoAim->Stop();
            }
        });
    autoAim.AddBool("only_when_firing", "仅开火时启用", &cfg.onlyWhenFiring)
        .Tooltip("勾选后，只有在开火时才会自动瞄准");
    autoAim.AddChoice("aim_mode", "模式", &cfg.aimMode,
                      {{"Assist", AUTO_AIM_MODE_ASSIST}, {"Magnet", AUTO_AIM_MODE_MAGNET}});
    autoAim.AddChoice("target_bone", "目标骨骼", &cfg.targetBone,
                      {{"头部", BONE_HEAD}, {"颈部", BONE_NECK}, {"胸部", BONE_CHEST}, {"骨盆", BONE_PELVIS}});
    autoAim.AddFloat("max_distance", "最大距离 (米)", &cfg.maxDistance, 10.0f, 500.0f, "%.0f");
    autoAim.AddFloat("fov_limit", "FOV 限制 (度)", &cfg.fovLimit, 5.0f, 90.0f, "%.0f");
    autoAim.AddFloat("update_rate", "更新频率 (Hz)", &cfg.updateRate, 30.0f, 500.0f, "%.0f");
    autoAim.AddFloat("switch_threshold", "目标切换阈值", &cfg.hysteresisThreshold, 10.0f, 200.0f, "%.0f");
    autoAim.AddBool("filter_teammates", "过滤队友", &cfg.filterTeammates);
    autoAim.AddBool("visibility_check", "可视性限制", &cfg.visibilityCheck);
    autoAim.AddBool("draw_debug", "显示调试信息", &cfg.drawDebug);

    MenuSectionSpec& status = page.AddSection("target_status", "目标状态", MenuColumn::Left);
    status.AddText("autoaim_status", "", [] { return GetAutoAimStatusText(); }).Persisted(false);

    MenuSectionSpec& triggerBot = page.AddSection("trigger_bot", "Trigger Bot", MenuColumn::Left);
    triggerBot.VisibleIf([] { return CurrentDriverSupportsTouch(); });
    triggerBot.AddBool("trigger_bot_enabled", "启用", &cfg.triggerBotEnabled)
        .Tooltip("当目标可视且位于准星中心附近时，自动按下你配置的开火键触点");
    triggerBot.AddBool("trigger_bot_hitscan_head", "Hit Scan 头部", &cfg.triggerBotHitScanHead)
        .Tooltip("勾选后，头部进入 Trigger Bot 判定范围时也会自动开火");
    triggerBot.AddBool("trigger_bot_hitscan_neck", "Hit Scan 颈部", &cfg.triggerBotHitScanNeck)
        .Tooltip("勾选后，颈部进入 Trigger Bot 判定范围时也会自动开火");
    triggerBot.AddBool("trigger_bot_hitscan_chest", "Hit Scan 胸部", &cfg.triggerBotHitScanChest)
        .Tooltip("勾选后，胸部进入 Trigger Bot 判定范围时也会自动开火");
    triggerBot.AddBool("trigger_bot_hitscan_pelvis", "Hit Scan 骨盆", &cfg.triggerBotHitScanPelvis)
        .Tooltip("勾选后，骨盆进入 Trigger Bot 判定范围时也会自动开火");
    triggerBot.AddFloat("trigger_bot_radius", "中心触发半径", &cfg.triggerBotCenterRadius, 4.0f, 80.0f, "%.0f");
    triggerBot.AddText("trigger_bot_fire_button_status", "", [] {
        return GetTriggerBotFireButtonStatusText();
    }).Persisted(false);
    triggerBot.AddButton("trigger_bot_pick_fire_button", "可视化设置开火键", [] {
        BeginTriggerBotFireButtonPicker();
    }).Tooltip("进入拾取模式后，直接在屏幕上点击开火键位置");
    triggerBot.AddFloat("trigger_bot_fire_x", "开火键 X 比例", &cfg.triggerBotFireButtonX, 0.0f, 1.0f, "%.3f")
        .Tooltip("可视化设置后的结果也会写回这里，便于微调");
    triggerBot.AddFloat("trigger_bot_fire_y", "开火键 Y 比例", &cfg.triggerBotFireButtonY, 0.0f, 1.0f, "%.3f")
        .Tooltip("可视化设置后的结果也会写回这里，便于微调");

    MenuSectionSpec& triggerBotUnsupported = page.AddSection("trigger_bot_unsupported", "Trigger Bot", MenuColumn::Left);
    triggerBotUnsupported.VisibleIf([] { return !CurrentDriverSupportsTouch(); });
    triggerBotUnsupported.AddText("trigger_bot_status", "", [] {
        return std::string("当前驱动不支持 touch 注入，Trigger Bot 已禁用。");
    }).Persisted(false);

    MenuSectionSpec& pd = page.AddSection("pd_controller", "PD 控制器", MenuColumn::Right);
    pd.AddFloat("kp_x", "X Kp", &cfg.KpX, 0.0f, 2.0f, "%.2f");
    pd.AddFloat("kd_x", "X Kd", &cfg.KdX, 0.0f, 1.0f, "%.2f");
    pd.AddFloat("kp_y", "Y Kp", &cfg.KpY, 0.0f, 2.0f, "%.2f");
    pd.AddFloat("kd_y", "Y Kd", &cfg.KdY, 0.0f, 1.0f, "%.2f");
    pd.AddFloat("output_scale_x", "X 输出倍率", &cfg.outputScaleX, 0.5f, 1.8f, "%.2f");
    pd.AddFloat("output_scale_y", "Y 输出倍率", &cfg.outputScaleY, 0.5f, 1.8f, "%.2f");

    MenuSectionSpec& humanize = page.AddSection("humanize", "人手噪声", MenuColumn::Right);
    humanize.AddBool("humanize_noise", "启用人手噪声", &cfg.humanizeNoise)
        .Tooltip("为最终陀螺仪输出增加平滑随机漂移和轻微颤动，模拟真实手搓微调");
    humanize.AddFloat("noise_strength_x", "X 漂移幅度", &cfg.noiseStrengthX, 0.0f, 0.80f, "%.2f");
    humanize.AddFloat("noise_strength_y", "Y 漂移幅度", &cfg.noiseStrengthY, 0.0f, 0.80f, "%.2f");
    humanize.AddFloat("noise_change_rate", "换向频率", &cfg.noiseChangeRate, 0.5f, 12.0f, "%.1f");
    humanize.AddFloat("noise_smoothing", "平滑速度", &cfg.noiseSmoothing, 1.0f, 16.0f, "%.1f");
    humanize.AddFloat("noise_micro_jitter", "微颤强度", &cfg.noiseMicroJitter, 0.0f, 0.20f, "%.2f");

    MenuSectionSpec& magnet = page.AddSection("magnet", "Magnet", MenuColumn::Right);
    magnet.VisibleIf([&cfg] { return cfg.aimMode == AUTO_AIM_MODE_MAGNET; });
    magnet.AddFloat("magnet_capture_radius", "吸附半径", &cfg.magnetCaptureRadius, 0.02f, 0.15f, "%.3f")
        .Tooltip("占屏幕短边比例。准星进入这个范围后才会启动 magnet");
    magnet.AddFloat("magnet_release_radius", "释放半径", &cfg.magnetReleaseRadius, 0.03f, 0.25f, "%.3f")
        .Tooltip("占屏幕短边比例。保持锁定时允许目标在更大范围内波动，避免一碰就断");
    magnet.AddFloat("magnet_strength", "保持强度", &cfg.magnetStrength, 0.05f, 1.00f, "%.2f")
        .Tooltip("只在 magnet 已吸附后生效，数值越大越不容易脱离目标");

    MenuSectionSpec& recoil = page.AddSection("recoil_control", "后座控制", MenuColumn::Right);
    recoil.AddFloat("recoil_base_offset_scale", "基础抬升倍率", &cfg.recoilBaseOffsetScale, 0.0f, 1.5f, "%.2f");
    recoil.AddFloat("recoil_kick_offset_scale", "枪口上跳幅度", &cfg.recoilKickOffsetScale, 0.0f, 400.0f, "%.1f");
    recoil.AddFloat("recoil_recovery_return_scale", "回正速度倍率", &cfg.recoilRecoveryReturnScale, 0.0f, 1.5f, "%.2f");
    recoil.AddFloat("max_recoil_offset_fraction", "最大镜心偏移", &cfg.maxRecoilOffsetFraction, 0.05f, 0.50f, "%.2f");
}

static void RegisterObjectsPage(MenuRegistry& registry) {
    MenuPageSpec& page = registry.AddPage("objects", "Objects");

    MenuSectionSpec& objects = page.AddSection("object_view", "对象显示", MenuColumn::Left);
    objects.AddBool("show_objects", "Show Objects", &gShowObjects);
    objects.AddBool("show_players", "玩家 (Players)", &gShowPlayers).ShortcutSupported(false);
    objects.AddBool("show_bots", "Bot", &gShowBots).ShortcutSupported(false);
    objects.AddBool("show_npcs", "NPC", &gShowNPCs).ShortcutSupported(false);
    objects.AddBool("show_monsters", "Monster", &gShowMonsters).ShortcutSupported(false);
    objects.AddBool("show_tomb_boxes", "战利品箱 (TombBox)", &gShowTombBoxes).ShortcutSupported(false);
    objects.AddBool("show_other_boxes", "其他盒子 (OtherBox)", &gShowOtherBoxes).ShortcutSupported(false);
    objects.AddBool("show_escape_boxes", "宝箱 (EscapeBox)", &gShowEscapeBoxes).ShortcutSupported(false);
    objects.AddBool("show_containers", "容器 (Container)", &gShowContainers).ShortcutSupported(false);
    objects.AddBool("show_vehicles", "载具 (Vehicles)", &gShowVehicles).ShortcutSupported(false);
    objects.AddBool("show_others", "其他 (Others)", &gShowOthers).ShortcutSupported(false);
    objects.AddBool("draw_name", "名称 (Name)", &gDrawName).ShortcutSupported(false);
    objects.AddBool("draw_distance", "距离 (Distance)", &gDrawDistance).ShortcutSupported(false);
    objects.AddBool("draw_box", "包围盒 (Box)", &gDrawBox).Tooltip("预留").ShortcutSupported(false);
    objects.AddBool("draw_predicted_aim", "预判点 (Prediction)", &gDrawPredictedAimPoint)
        .ShortcutSupported(false)
        .Tooltip("准星在目标附近停留一小段时间后，显示该目标的预判点");

    MenuSectionSpec& skeleton = page.AddSection("skeleton_matrix", "骨骼与矩阵", MenuColumn::Left);
    skeleton.AddBool("draw_skeleton", "骨骼 (Skeleton)", &gDrawSkeleton).ShortcutSupported(false);
    skeleton.AddBool("depth_visibility", "骨骼可视性射线检测", &gUseDepthBufferVisibility).ShortcutSupported(false);
    skeleton.AddBool("bone_smoothing", "骨骼平滑", &gEnableBoneSmoothing).ShortcutSupported(false);
    skeleton.AddBool("use_camera_cache_vp", "使用 CameraCache VP 矩阵", &gUseCameraCacheVPMatrix)
        .ShortcutSupported(false)
        .Tooltip("切换主矩阵来源: CanvasMap 或 CameraCache->MinimalViewInfo");

    MenuSectionSpec& physx = page.AddSection("physx_geometry", "PhysX 几何体", MenuColumn::Right);
    physx.AddBool("draw_physx_geometry", "显示 PhysX 几何", &gDrawPhysXGeometry).ShortcutSupported(false);
    physx.AddBool("show_physx_debug_window", "显示 PhysX 调试窗口", &gShowPhysXDebugWindow)
        .Persisted(false)
        .ShortcutSupported(false);
    physx.AddBool("show_bullet_spread_debug_window", "显示子弹扩散调试窗口", &gShowBulletSpreadDebugWindow)
        .Persisted(false)
        .ShortcutSupported(false)
        .OnChange([] { SetBulletSpreadMonitorEnabled(gShowBulletSpreadDebugWindow); });
    physx.AddChoice("bullet_breakpoint_target", "子弹断点目标", &gBulletBreakpointTargetUi,
                    {{"BulletSpreadFuncEntry", 0}, {"EngineLoopProbe", 1}})
        .Persisted(false)
        .OnChange([] { SetBulletBreakpointTarget(static_cast<BulletBreakpointTarget>(gBulletBreakpointTargetUi)); });
    physx.AddBool("draw_physx_meshes", "绘制 Mesh", &gPhysXDrawMeshes).ShortcutSupported(false);
    physx.AddBool("draw_physx_primitives", "绘制基础体", &gPhysXDrawPrimitives).ShortcutSupported(false);
    physx.AddBool("use_local_model_data", "读取本地模型数据", &gPhysXUseLocalModelData).ShortcutSupported(false);
    physx.AddBool("physx_auto_export", "自动导出模型", &gPhysXAutoExport)
        .ShortcutSupported(false)
        .Tooltip("内存读取到新模型时自动保存到磁盘，下次优先从磁盘加载");
    physx.AddBool("manual_scene_index_enabled", "手动 SceneIndex", &gPhysXManualSceneIndexEnabled)
        .ShortcutSupported(false);
    physx.AddInt("manual_scene_index", "PxScene Index", &gPhysXManualSceneIndex, 0, 15, "%d")
        .VisibleIf([] { return gPhysXManualSceneIndexEnabled; });
    physx.AddFloat("physx_radius_meters", "PhysX 半径 (米)", &gPhysXDrawRadiusMeters, 20.0f, 300.0f, "%.0f");
    physx.AddFloat("physx_refresh_interval", "几何缓存刷新间隔 (秒)", &gPhysXGeomRefreshInterval, 5.0f, 120.0f, "%.0f");
    physx.AddFloat("physx_center_region", "准星区域", &gPhysXCenterRegionFovDegrees, 5.0f, 60.0f, "%.0f");

    MenuSectionSpec& limits = page.AddSection("export_limits", "导出与限制", MenuColumn::Right);
    limits.AddInt("physx_max_actors", "最大 Actor", &gPhysXMaxActorsPerFrame, 32, 512, "%d");
    limits.AddInt("physx_max_shapes_per_actor", "每 Actor 最大 Shape", &gPhysXMaxShapesPerActor, 1, 64, "%d");
    limits.AddInt("physx_max_triangles_per_mesh", "每 Mesh 最大三角形", &gPhysXMaxTrianglesPerMesh, 64, 8000, "%d");
    limits.AddButton("export_stable_obj", "导出稳定 OBJ", [] {
        ExportStablePhysXMeshes();
    });
    limits.AddText("export_status", "", [] {
        return std::string(GetStablePhysXExportStatus());
    }).Persisted(false);
}

static void RegisterConfigPage(MenuRegistry& registry) {
    MenuPageSpec& page = registry.AddPage("config", "Config");

    MenuSectionSpec& display = page.AddSection("display_sync", "显示与同步", MenuColumn::Left);
    display.AddBool("show_all_class_names", "显示所有类名", &gShowAllClassNames);
    display.AddBool("use_batch_bone_read", "批量读取（优化）", &gUseBatchBoneRead)
        .Tooltip("批量读取: 1次ioctl读取整个骨骼数组（推荐）\n逐个读取: 每个骨骼1次ioctl（调试用）");
    display.AddFloat("max_skeleton_distance", "最大距离 (米)", &gMaxSkeletonDistance, 50.0f, 500.0f, "%.0f")
        .Tooltip("超过此距离的角色不绘制骨骼，减少性能开销");
    display.AddText("display_sync_status", "", [] { return GetDisplaySyncStatusText(); }).Persisted(false);

    MenuSectionSpec& driver = page.AddSection("driver_memory", "驱动与内存", MenuColumn::Left);
    driver.AddChoice("driver_type", "驱动类型", &gDriverTypeUi,
                     {{"RT Hook", DRIVER_RT_HOOK}, {"Paradise Hook", DRIVER_PARADISE}})
        .Persisted(false)
        .OnChange([] { ApplyDriverTypeSelection(); });
    driver.AddButton("init_mem", "mem", [] {
        InitDriver("com.tencent.tmgp.pubgmhd", libUE4);
    }).Persisted(false);
    driver.AddButton("dump_tarray", "Dump TArray", [] {
        DumpTArray();
    }).Persisted(false).VisibleIf([] {
        return driver_stat.load(std::memory_order_relaxed) > 0;
    });
    driver.AddButton("dump_bones", "Dump Bones", [] {
        DumpBones();
    }).Persisted(false).VisibleIf([] {
        return driver_stat.load(std::memory_order_relaxed) > 0;
    });
    driver.AddButton("test_touch", "测试 Touch", [] {
        RunTouchTest();
    }).Persisted(false);
    driver.AddText("touch_test_status", "", [] {
        return GetTouchTestStatusText();
    }).Persisted(false);
    driver.AddText("driver_status", "", [] { return GetDriverMemoryStatusText(); }).Persisted(false);

    MenuSectionSpec& scan = page.AddSection("scan_data", "扫描与数据", MenuColumn::Right);
    scan.AddButton("scan_canvas", "扫描 Canvas", [] {
        ClearScanResults();
        ScanForClass("CustomizeCanvasPanel_BP_C");
    }).Persisted(false).VisibleIf([] {
        return driver_stat.load(std::memory_order_relaxed) > 0;
    });
    scan.AddButton("find_uclass", "查找 UClass", [] {
        ClearScanResults();
        FindUClass("SettingConfig");
    }).Persisted(false).VisibleIf([] {
        return driver_stat.load(std::memory_order_relaxed) > 0;
    });
    scan.AddButton("read_fps_level", "读取 FPS Level", [] {
        FindSettingConfigViaGameInstance();
    }).Persisted(false).VisibleIf([] {
        return driver_stat.load(std::memory_order_relaxed) > 0;
    });
    scan.AddText("scan_status", "", [] { return GetScanDataStatusText(); }).Persisted(false);
    scan.FooterDraw([] {
        if (driver_stat.load(std::memory_order_relaxed) <= 0) return;
        auto scanResults = GetScanResults();
        if (scanResults.empty()) return;
        ImGui::Spacing();
        ImGui::Text("找到 %zu 个实例:", scanResults.size());
        for (size_t i = 0; i < scanResults.size(); ++i) {
            ImGui::Text("[%zu] 0x%llX", i, static_cast<unsigned long long>(scanResults[i].first));
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", scanResults[i].second.c_str());
        }
    });

    MenuSectionSpec& tools = page.AddSection("tools_recording", "工具与录屏", MenuColumn::Right);
    tools.AddButton("exit_tool", "退出", [] {
        IsToolActive.store(false);
    }).Persisted(false);
    tools.AddButton("detect_virtual_display", "检测虚拟显示", [] {
        android::ANativeWindowCreator::DetectAndCreateVirtualDisplayMirrors();
    }).Persisted(false).Tooltip("开始录屏后点击此按钮，会检测录屏创建的虚拟显示并自动创建镜像层");
    tools.AddText("tool_status", "", [] { return GetToolStatusText(); }).Persisted(false);

    MenuSectionSpec& configOps = page.AddSection("config_management", "配置管理", MenuColumn::Left);
    configOps.AddText("config_path", "配置文件", [] { return GetConfigPathString(); }).Persisted(false);
    configOps.AddButton("export_config", "导出配置", [] {
        SetConfigStatus(MenuRegistry::Instance().ExportToFile(MenuRegistry::Instance().GetDefaultConfigPath()));
    }).Persisted(false);
    configOps.AddButton("import_config", "导入配置", [] {
        SetConfigStatus(MenuRegistry::Instance().ImportFromFile(MenuRegistry::Instance().GetDefaultConfigPath()));
    }).Persisted(false);
    configOps.AddButton("reset_defaults", "恢复默认", [] {
        SetConfigStatus(MenuRegistry::Instance().ResetToDefaults());
    }).Persisted(false);
    configOps.AddText("config_status", "结果", [] { return gConfigStatusMessage.empty() ? std::string("未执行") : gConfigStatusMessage; })
        .Persisted(false);
}

static void EnsureMenuFrameworkRegistered() {
    if (gMenuFrameworkRegistered) return;

    MenuRegistry& registry = MenuRegistry::Instance();
    registry.Reset();
    registry.SetDefaultConfigPath("debugger_config.json");
    gDriverTypeUi = static_cast<int>(GetDriverManager().getType());
    RegisterCameraPage(registry);
    RegisterObjectsPage(registry);
    RegisterConfigPage(registry);
    gMenuFrameworkRegistered = true;
}

struct PhysXSceneMapEntryDebug {
    uint16_t key;
    uint16_t pad0;
    uint32_t pad1;
    uint64_t scenePtr;
    int32_t next;
    int32_t pad2;
};

static uint64_t LookupPhysXSceneDebug(uint64_t libBase, uint16_t sceneIndex) {
    if (libBase == 0) return 0;

    const uint32_t hashSize = GetDriverManager().read<uint32_t>(libBase + offset.GPhysXSceneMapHashSize);
    if (hashSize == 0) return 0;

    const uint64_t entryArray = GetDriverManager().read<uint64_t>(libBase + offset.GPhysXSceneMap);
    if (entryArray == 0) return 0;

    uint64_t bucketBase = GetDriverManager().read<uint64_t>(libBase + offset.GPhysXSceneMapBucketPtr);
    if (bucketBase == 0) {
        bucketBase = libBase + offset.GPhysXSceneMapBuckets;
    }

    int32_t bucket = GetDriverManager().read<int32_t>(
        bucketBase + 4ULL * ((hashSize - 1u) & static_cast<uint32_t>(sceneIndex)));
    while (bucket != -1) {
        PhysXSceneMapEntryDebug entry{};
        if (!GetDriverManager().read(entryArray + static_cast<uint64_t>(bucket) * sizeof(entry), &entry, sizeof(entry))) {
            return 0;
        }
        if (entry.key == sceneIndex) {
            return entry.scenePtr;
        }
        bucket = entry.next;
    }
    return 0;
}

static uint16_t ResolvePreferredPhysXSceneIndexDebug(uint64_t libBase, uint64_t physScene, uint32_t sceneCount) {
    if (gPhysXManualSceneIndexEnabled) {
        return static_cast<uint16_t>(std::max(gPhysXManualSceneIndex, 0) & 0xFFFF);
    }
    if (physScene == 0 || sceneCount == 0 || sceneCount >= 16) {
        return 0;
    }

    uint16_t fallbackIndex = 0;
    bool hasFallback = false;
    for (uint32_t i = 0; i < sceneCount; ++i) {
        const uint16_t sceneIndex = GetDriverManager().read<uint16_t>(
            physScene + offset.PhysSceneSceneIndexArray + static_cast<uint64_t>(i) * sizeof(uint16_t));
        if (!hasFallback) {
            fallbackIndex = sceneIndex;
            hasFallback = true;
        }
        const uint64_t pxScene = LookupPhysXSceneDebug(libBase, sceneIndex);
        if (pxScene == 0) continue;
        const uint64_t pxActorsAddr = GetDriverManager().read<uint64_t>(pxScene + offset.PxSceneActors);
        const uint32_t pxActorCount = GetDriverManager().read<uint32_t>(pxScene + offset.PxSceneActorCount);
        if (pxActorsAddr != 0 && pxActorCount > 0 && pxActorCount <= 100000) {
            return sceneIndex;
        }
    }
    return hasFallback ? fallbackIndex : 0;
}

static void DrawPhysXDebugWindow() {
    if (!gShowPhysXDebugWindow) return;

    ImGui::SetNextWindowSize(ImVec2(860.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("PhysX Debug", &gShowPhysXDebugWindow)) {
        ImGui::End();
        return;
    }

    ImGui::Text("PhysX offset/runtime debug");
    ImGui::Separator();

    const uint64_t libBase = address.libUE4;
    const uint64_t uworldPtrAddr = libBase ? libBase + offset.Gworld : 0;
    const uint64_t uworld = uworldPtrAddr ? GetDriverManager().read<uint64_t>(uworldPtrAddr) : 0;
    const uint64_t physSceneAddr = uworld ? uworld + offset.PhysicsScene : 0;
    const uint64_t physScene = physSceneAddr ? GetDriverManager().read<uint64_t>(physSceneAddr) : 0;
    const uint32_t sceneCount = physScene ? GetDriverManager().read<uint32_t>(physScene + offset.PhysSceneSceneCount) : 0;
    const uint16_t syncSceneIndex = physScene ? GetDriverManager().read<uint16_t>(physScene + offset.PhysSceneSceneIndexArray) : 0;
    const uint16_t activeSceneIndex = ResolvePreferredPhysXSceneIndexDebug(libBase, physScene, sceneCount);
    const uint64_t pxScene = LookupPhysXSceneDebug(libBase, activeSceneIndex);
    const uint64_t pxActorsAddr = pxScene ? GetDriverManager().read<uint64_t>(pxScene + offset.PxSceneActors) : 0;
    const uint32_t pxActorCount = pxScene ? GetDriverManager().read<uint32_t>(pxScene + offset.PxSceneActorCount) : 0;
    const uint64_t bucketPtrAddr = libBase ? libBase + offset.GPhysXSceneMapBucketPtr : 0;
    const uint64_t bucketPtrValue = bucketPtrAddr ? GetDriverManager().read<uint64_t>(bucketPtrAddr) : 0;
    const uint64_t bucketBase = bucketPtrValue ? bucketPtrValue : (libBase ? libBase + offset.GPhysXSceneMapBuckets : 0);
    const uint32_t hashSize = libBase ? GetDriverManager().read<uint32_t>(libBase + offset.GPhysXSceneMapHashSize) : 0;
    const uint64_t entryArrayPtr = libBase ? GetDriverManager().read<uint64_t>(libBase + offset.GPhysXSceneMap) : 0;
    const int32_t bucketValue = (bucketBase && hashSize)
        ? GetDriverManager().read<int32_t>(bucketBase + 4ULL * ((hashSize - 1u) & static_cast<uint32_t>(activeSceneIndex)))
        : -1;

    auto draw_row = [](const char* label, uint64_t addr, uint64_t value) {
        ImGui::SeparatorText(label);
        ImGui::TextWrapped("addr : 0x%llX", static_cast<unsigned long long>(addr));
        ImGui::TextWrapped("value: 0x%llX", static_cast<unsigned long long>(value));
    };

    draw_row("libUE4", 0, libBase);
    draw_row("GWorld ptr", uworldPtrAddr, uworld);
    draw_row("UWorld->PhysicsScene", physSceneAddr, physScene);
    draw_row("PhysXSceneMap ptr", libBase ? libBase + offset.GPhysXSceneMap : 0, entryArrayPtr);
    draw_row("PhysX bucket ptr", bucketPtrAddr, bucketPtrValue);
    draw_row("PhysX bucket base", 0, bucketBase);
    draw_row("PxScene", 0, pxScene);
    draw_row("PxScene->Actors", pxScene ? pxScene + offset.PxSceneActors : 0, pxActorsAddr);

    ImGui::Separator();
    ImGui::Text("sceneCount: %u", sceneCount);
    ImGui::Text("syncSceneIndex: %u (0x%X)", static_cast<unsigned>(syncSceneIndex), static_cast<unsigned>(syncSceneIndex));
    ImGui::Text("activeSceneIndex: %u (0x%X)%s",
                static_cast<unsigned>(activeSceneIndex),
                static_cast<unsigned>(activeSceneIndex),
                gPhysXManualSceneIndexEnabled ? " [manual]" : " [auto]");
    ImGui::Text("hashSize: %u", hashSize);
    ImGui::Text("bucketValue: %d", bucketValue);
    ImGui::Text("pxActorCount: %u", pxActorCount);

    if (pxActorsAddr != 0 && pxActorCount > 0) {
        uint64_t firstActor = GetDriverManager().read<uint64_t>(pxActorsAddr);
        uint16_t firstActorType = firstActor ? GetDriverManager().read<uint16_t>(firstActor + offset.PxActorType) : 0;
        uint16_t firstShapeCount = firstActor ? GetDriverManager().read<uint16_t>(firstActor + offset.PxActorShapeCount) : 0;
        uint64_t firstShapePtr = 0;
        if (firstActor != 0 && firstShapeCount > 0) {
            if (firstShapeCount == 1) {
                firstShapePtr = GetDriverManager().read<uint64_t>(firstActor + offset.PxActorShapes);
            } else {
                uint64_t arr = GetDriverManager().read<uint64_t>(firstActor + offset.PxActorShapes);
                if (arr != 0) {
                    firstShapePtr = GetDriverManager().read<uint64_t>(arr);
                }
            }
        }
        uint32_t npShapeFlags = firstShapePtr ? GetDriverManager().read<uint32_t>(firstShapePtr + offset.PxShapeFlags) : 0;
        uint64_t shapeCore = firstShapePtr ? GetDriverManager().read<uint64_t>(firstShapePtr + offset.PxShapeCorePtr) : 0;
        uint64_t geomAddr = 0;
        if (firstShapePtr != 0) {
            geomAddr = ((npShapeFlags & 1u) != 0 && shapeCore != 0)
                ? shapeCore + offset.PxShapeCoreGeometry
                : firstShapePtr + offset.PxShapeGeometryInline;
        }
        uint32_t geomType = geomAddr ? GetDriverManager().read<uint32_t>(geomAddr) : 0;

        ImGui::Separator();
        ImGui::Text("firstActor: 0x%llX", static_cast<unsigned long long>(firstActor));
        ImGui::Text("firstActorType: %u", static_cast<unsigned>(firstActorType));
        ImGui::Text("firstShapeCount: %u", static_cast<unsigned>(firstShapeCount));
        ImGui::Text("firstShape: 0x%llX", static_cast<unsigned long long>(firstShapePtr));
        ImGui::Text("firstShapeFlags(raw): 0x%X", npShapeFlags);
        ImGui::Text("firstShapeCore: 0x%llX", static_cast<unsigned long long>(shapeCore));
        ImGui::Text("firstGeomAddr: 0x%llX", static_cast<unsigned long long>(geomAddr));
        ImGui::Text("firstGeomType: %u", geomType);
    }

    ImGui::End();
}

static void DrawBulletSpreadDebugWindow() {
    if (!gShowBulletSpreadDebugWindow) return;

    const BulletSpreadDebugState state = GetBulletSpreadDebugState();

    ImGui::SetNextWindowSize(ImVec2(520.0f, 280.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Bullet Spread Debug", &gShowBulletSpreadDebugWindow)) {
        ImGui::End();
        return;
    }

    ImGui::Text("CHwBreakpointMgr proc-node probe");
    ImGui::Separator();
    ImGui::Text("Target: %s",
                state.target == BulletBreakpointTarget::EngineLoopProbe
                    ? "EngineLoopProbe"
                    : "BulletSpreadFuncEntry");
    ImGui::Text("Requested: %s", state.requestedEnabled ? "true" : "false");
    ImGui::Text("Thread: %s", state.threadRunning ? "running" : "stopped");
    ImGui::Text("Driver: %s", state.driverConnected ? "connected" : "disconnected");
    ImGui::Text("Session: %s", state.sessionOpen ? "open" : "closed");
    ImGui::Text("Probe: %s", state.breakpointArmed ? "armed" : "not armed");
    ImGui::Text("Valid Hit: %s", state.valid ? "true" : "false");
    ImGui::Text("PID/TID: %d / %d", state.pid, state.tid);
    ImGui::Text("Hits: %llu (total: %llu)",
                static_cast<unsigned long long>(state.hitCount),
                static_cast<unsigned long long>(state.hitTotalCount));
    ImGui::Text("BP Addr: 0x%llX", static_cast<unsigned long long>(state.breakpointAddr));
    ImGui::Text("PC: 0x%llX", static_cast<unsigned long long>(state.pc));
    ImGui::Text("this: 0x%llX", static_cast<unsigned long long>(state.thisPtr));
    ImGui::Separator();
    ImGui::Text("Spread: %.5f, %.5f, %.5f", state.spread.X, state.spread.Y, state.spread.Z);
    ImGui::Text("Deviation: %.5f", state.deviation);
    ImGui::Text("Deviation Yaw/Pitch: %.5f / %.5f", state.deviationYaw, state.deviationPitch);
    ImGui::Text("Last Hit (monotonic ms): %llu",
                static_cast<unsigned long long>(state.lastHitMonotonicMs));

    if (state.lastError != 0) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "Error: %d (%s)", state.lastError, strerror(state.lastError));
    } else if (state.requestedEnabled && state.hitCount == 0) {
        ImGui::Separator();
        ImGui::TextDisabled("Waiting for breakpoint hit...");
    }

    ImGui::End();
}

void Draw_Menu_ResetTextures() {
    gLogoTexture = (ImTextureID)0;
    gLogoWidth = 0;
    gLogoHeight = 0;
    gBannerTexture = (ImTextureID)0;
    gBannerWidth = 0;
    gBannerHeight = 0;
}

void Draw_Menu_Overlay() {
    EnsureMenuFrameworkRegistered();
    MenuRegistry::Instance().RenderShortcutWidgets();
    DrawFloatingMenuBall();
}

void Draw_Menu() {
    //Gyro_Controller->update(gyro_x, gyro_y);
    ImGuiIO& io = ImGui::GetIO();
    EnsureMenuFrameworkRegistered();
    SetBulletBreakpointTarget(static_cast<BulletBreakpointTarget>(gBulletBreakpointTargetUi));
    SetBulletSpreadMonitorEnabled(gShowBulletSpreadDebugWindow);

    // 在函数开始就保存原始鼠标状态（在任何控件修改之前）
    static bool was_mouse_down = false;
    bool original_mouse_down = io.MouseDown[0];
    ImVec2 original_mouse_pos = io.MousePos;

    float windowWidth = 900.0f;
    float windowHeight = 860.0f;

    ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_Once);
    ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - windowWidth) / 2, (io.DisplaySize.y - windowHeight) / 2), ImGuiCond_Once);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 15.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.09f, 0.15f, 0.72f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.12f, 0.20f, 0.38f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.10f, 0.17f, 0.82f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.16f, 0.26f, 0.56f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.14f, 0.22f, 0.34f, 0.68f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.16f, 0.26f, 0.40f, 0.78f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.24f, 0.40f, 0.58f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.34f, 0.56f, 0.76f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.40f, 0.66f, 0.86f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.12f, 0.22f, 0.38f, 0.52f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.18f, 0.32f, 0.54f, 0.70f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.22f, 0.38f, 0.62f, 0.82f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.58f, 0.84f, 1.00f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.42f, 0.78f, 1.00f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.56f, 0.86f, 1.00f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.10f, 0.18f, 0.30f, 0.58f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.18f, 0.32f, 0.50f, 0.78f));
    ImGui::PushStyleColor(ImGuiCol_TabActive, ImVec4(0.16f, 0.30f, 0.48f, 0.88f));

    if (!ImGui::Begin("###MainWindow", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(18);
        ImGui::End();
        was_mouse_down = original_mouse_down;
        return;
    }

    // 自定义窗口拖动：只在 Logo 和 Tab 区域可以拖动窗口
    ImVec2 window_pos = ImGui::GetWindowPos();
    ImVec2 drag_area_min = window_pos;
    ImVec2 drag_area_max;
    drag_area_max.x = window_pos.x + windowWidth;
    drag_area_max.y = window_pos.y + 230.0f;  // Logo + Tab 区域高度

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
    const ImVec2 logoPos(28.0f, 12.0f);
    const ImVec2 logoSize(176.0f, 176.0f);

    ImGui::SetCursorPos(logoPos);
    if (!gLogoTexture && aimware_png_len > 0) {
        ImGui_RequestTextureLoad(aimware_png, aimware_png_len, &gLogoTexture, &gLogoWidth, &gLogoHeight);
    }
    if (gLogoTexture) {
        ImGui::Image(gLogoTexture, logoSize);
    }

    const ImVec2 bannerPos(-100.0f, 0.0f);
    const ImVec2 bannerSize(1145.0f, 192.0f);
    ImGui::SetCursorPos(bannerPos);
    if (!gBannerTexture && banner_png_len > 0) {
        ImGui_RequestTextureLoad(banner_png, banner_png_len, &gBannerTexture, &gBannerWidth, &gBannerHeight);
    }
    if (gBannerTexture) {
        ImGui::Image(gBannerTexture, bannerSize);
    }

    const ImVec2 actionButtonSize(42.0f, 42.0f);
    const float actionSpacing = 10.0f;
    const float actionStartX = windowWidth - (actionButtonSize.x * 4.0f + actionSpacing * 3.0f) - 28.0f;
    const float actionY = 18.0f;
    const ImVec4 actionFill(0.08f, 0.18f, 0.30f, 0.56f);
    const ImVec4 actionBorder(0.34f, 0.72f, 1.00f, 0.84f);

    if (DrawIconActionButton("banner_save_config", "💾", ImVec2(actionStartX, actionY), actionButtonSize, actionFill, actionBorder)) {
        SetConfigStatus(MenuRegistry::Instance().ExportToFile(MenuRegistry::Instance().GetDefaultConfigPath()));
    }
    if (DrawIconActionButton("banner_load_config", "📂", ImVec2(actionStartX + (actionButtonSize.x + actionSpacing), actionY),
                             actionButtonSize, actionFill, actionBorder)) {
        SetConfigStatus(MenuRegistry::Instance().ImportFromFile(MenuRegistry::Instance().GetDefaultConfigPath()));
    }
    if (DrawIconActionButton("banner_close_menu", "❌", ImVec2(actionStartX + (actionButtonSize.x + actionSpacing) * 2.0f, actionY),
                             actionButtonSize, actionFill, actionBorder)) {
        gLastMenuPos = ImGui::GetWindowPos();
        IsMenuOpen.store(false, std::memory_order_relaxed);
    }
    if (DrawIconActionButton("banner_exit_tool", "🚪", ImVec2(actionStartX + (actionButtonSize.x + actionSpacing) * 3.0f, actionY),
                             actionButtonSize, actionFill, actionBorder)) {
        IsToolActive.store(false);
    }

    bool isLandscape = (io.DisplaySize.x > io.DisplaySize.y);
    float tabX = isLandscape ? 236.0f : 242.0f;
    float tabY = isLandscape ? 130.0f : 136.0f;
    const char* tabLabels[] = {"Camera", "Objects", "Config"};
    const float tabLabelY = tabY - 96.0f;
    ImGui::SetWindowFontScale(1.45f);
    ImVec2 currentTabTextSize = ImGui::CalcTextSize(tabLabels[gMainTabIndex]);
    float currentTabTextX = tabX + ((windowWidth - tabX - 20.0f) - currentTabTextSize.x) * 0.5f - 56.0f;
    ImVec2 titlePos(currentTabTextX, tabLabelY);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 titleScreenPos(window_pos.x + titlePos.x, window_pos.y + titlePos.y);
    drawList->AddText(
        ImGui::GetFont(),
        ImGui::GetFontSize() * 1.45f,
        ImVec2(titleScreenPos.x + 2.0f, titleScreenPos.y + 3.0f),
        ImGui::GetColorU32(ImVec4(0.02f, 0.08f, 0.16f, 0.75f)),
        tabLabels[gMainTabIndex]);
    drawList->AddText(
        ImGui::GetFont(),
        ImGui::GetFontSize() * 1.45f,
        titleScreenPos,
        ImGui::GetColorU32(ImVec4(0.90f, 0.97f, 1.00f, 1.00f)),
        tabLabels[gMainTabIndex]);
    drawList->AddRectFilledMultiColor(
        ImVec2(titleScreenPos.x, titleScreenPos.y + currentTabTextSize.y + 8.0f),
        ImVec2(titleScreenPos.x + currentTabTextSize.x + 18.0f, titleScreenPos.y + currentTabTextSize.y + 12.0f),
        ImGui::GetColorU32(ImVec4(0.10f, 0.45f, 0.95f, 0.20f)),
        ImGui::GetColorU32(ImVec4(0.22f, 0.68f, 1.00f, 0.95f)),
        ImGui::GetColorU32(ImVec4(0.22f, 0.68f, 1.00f, 0.95f)),
        ImGui::GetColorU32(ImVec4(0.10f, 0.45f, 0.95f, 0.20f)));
    ImGui::SetWindowFontScale(1.0f);

    ImGui::SetCursorPos(ImVec2(tabX, tabY));
    const float tabSpacing = 8.0f;
    const float tabWidth = ((windowWidth - tabX - 20.0f) - tabSpacing * 2.0f) / 3.0f;
    const float tabHeight = 58.0f;
    if (gIconFont) ImGui::PushFont(gIconFont);
    DrawMainTabButton("camera", "   s   ", 0, tabWidth, tabHeight);
    ImGui::SameLine(0.0f, tabSpacing);
    DrawMainTabButton("objects", "   t   ", 1, tabWidth, tabHeight);
    ImGui::SameLine(0.0f, tabSpacing);
    DrawMainTabButton("config", "   v   ", 2, tabWidth, tabHeight);
    if (gIconFont) ImGui::PopFont();

    const float dividerY = 198.0f;
    ImVec2 dividerMin(window_pos.x + 18.0f, window_pos.y + dividerY);
    ImVec2 dividerMax(window_pos.x + windowWidth - 18.0f, window_pos.y + dividerY + 6.0f);
    drawList->AddRectFilledMultiColor(
        dividerMin,
        dividerMax,
        ImGui::GetColorU32(ImVec4(0.10f, 0.45f, 0.95f, 0.25f)),
        ImGui::GetColorU32(ImVec4(0.22f, 0.68f, 1.00f, 0.92f)),
        ImGui::GetColorU32(ImVec4(0.22f, 0.68f, 1.00f, 0.92f)),
        ImGui::GetColorU32(ImVec4(0.10f, 0.45f, 0.95f, 0.25f)));

    const float contentStartY = dividerY + 18.0f;
    ImGui::SetCursorPos(ImVec2(16.0f, contentStartY));
    if (ImGui::BeginChild(
            "MainContentRegion",
            ImVec2(windowWidth - 32.0f, windowHeight - contentStartY - 16.0f),
            false,
            ImGuiWindowFlags_NoBackground)) {
        if (gMainTabIndex == 0) {
            TouchScrollable::Begin("CameraScrollRegion", ImVec2(0, 0));
            DrawCameraTab();
            TouchScrollable::End();
        } else if (gMainTabIndex == 1) {
            TouchScrollable::Begin("ObjViewScrollRegion", ImVec2(0, 0));
            DrawObjViewTab();
            TouchScrollable::End();
        } else {
            TouchScrollable::Begin("ConfigScrollRegion", ImVec2(0, 0));
            DrawConfigTab();
            TouchScrollable::End();
        }
    }
    ImGui::EndChild();

    ImGui::PopStyleColor(18);
    ImGui::PopStyleVar(4);
    ImGui::End();
    DrawPhysXDebugWindow();
    DrawBulletSpreadDebugWindow();
}

void DrawCameraTab() {
    EnsureMenuFrameworkRegistered();
    const bool gyroUnsupported = IsCameraPageGyroUnsupported();
    if (gyroUnsupported) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.30f, 1.0f));
        ImGui::TextWrapped("当前不支持陀螺仪：gyro socket 连接失败，且当前驱动不支持 gyro_update。");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::BeginDisabled(true);
    }
    MenuRegistry::Instance().RenderPage("camera");
    if (gyroUnsupported) {
        ImGui::EndDisabled();
    }
    DrawTriggerBotFireButtonPickerOverlay();
}

void DrawObjViewTab() {
    EnsureMenuFrameworkRegistered();
    MenuRegistry::Instance().RenderPage("objects");
}

void DrawConfigTab() {
    EnsureMenuFrameworkRegistered();
    gDriverTypeUi = static_cast<int>(GetDriverManager().getType());
    MenuRegistry::Instance().RenderPage("config");
}
