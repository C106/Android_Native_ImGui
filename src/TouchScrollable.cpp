#include "TouchScrollable.h"

namespace TouchScrollable {

struct ScrollState {
    ImVec2 touch_start_pos;
    float  scroll_start_y    = 0.0f;
    float  prev_mouse_y      = 0.0f;    // 上一帧鼠标 Y，用于计算瞬时速度
    float  velocity          = 0.0f;
    bool   is_touching       = false;
    bool   is_dragging       = false;
    bool   widget_was_active = false;
};

static constexpr float DRAG_THRESHOLD  = 5.0f;   // 进入拖动的最小位移 (px)
static constexpr float VELOCITY_SMOOTH = 0.6f;    // 瞬时速度平滑系数 (0~1, 越大越平滑)
static constexpr float VELOCITY_CAP    = 50.0f;   // 惯性最大速度 (px/帧)
static constexpr float INERTIA_DECAY   = 0.90f;   // 每帧衰减系数
static constexpr float INERTIA_STOP    = 0.3f;    // 停止惯性的最小速度

static ScrollState g_state;
static bool        g_any_scrolling = false;

bool IsScrolling() { return g_any_scrolling; }

bool Begin(const char* id, const ImVec2& size, ImGuiWindowFlags flags) {
    flags |= ImGuiWindowFlags_NoScrollbar;
    return ImGui::BeginChild(id, size, false, flags);
}

void End() {
    ImGuiIO&     io     = ImGui::GetIO();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window) { ImGui::EndChild(); return; }

    ImGuiID wid = window->ID;

    ImRect window_rect(window->Pos,
                       ImVec2(window->Pos.x + window->Size.x,
                              window->Pos.y + window->Size.y));
    bool hovered    = window_rect.Contains(io.MousePos);
    bool mouse_down = io.MouseDown[0];

    ImGuiID active_id = ImGui::GetActiveID();
    bool widget_active = (active_id != 0 && active_id != wid);

    float max_scroll = ImGui::GetScrollMaxY();
    float cur_scroll = ImGui::GetScrollY();

    // --- 触摸开始 ---
    if (hovered && mouse_down && !g_state.is_touching) {
        g_state.is_touching       = true;
        g_state.is_dragging       = false;
        g_state.touch_start_pos   = io.MousePos;
        g_state.scroll_start_y    = cur_scroll;
        g_state.prev_mouse_y      = io.MousePos.y;
        g_state.velocity          = 0.0f;
        g_state.widget_was_active = widget_active;
    }

    // --- 触摸进行中 ---
    if (g_state.is_touching && mouse_down) {
        float dy = io.MousePos.y - g_state.touch_start_pos.y;

        if (!g_state.is_dragging && !g_state.widget_was_active && fabs(dy) > DRAG_THRESHOLD)
            g_state.is_dragging = true;

        if (g_state.is_dragging) {
            float new_scroll = g_state.scroll_start_y - dy;
            if (new_scroll < 0.0f)       new_scroll = 0.0f;
            if (new_scroll > max_scroll)  new_scroll = max_scroll;
            ImGui::SetScrollY(new_scroll);

            // 用帧间位移计算瞬时速度，EMA 平滑
            float frame_dy = -(io.MousePos.y - g_state.prev_mouse_y);
            g_state.velocity = VELOCITY_SMOOTH * g_state.velocity
                             + (1.0f - VELOCITY_SMOOTH) * frame_dy;

            g_state.prev_mouse_y = io.MousePos.y;
            io.MouseDown[0] = false;
        }
    }

    // --- 触摸结束 ---
    if (g_state.is_touching && !mouse_down) {
        // 限制最大惯性速度
        if (g_state.velocity >  VELOCITY_CAP) g_state.velocity =  VELOCITY_CAP;
        if (g_state.velocity < -VELOCITY_CAP) g_state.velocity = -VELOCITY_CAP;
        g_state.is_touching = false;
        g_state.is_dragging = false;
    }

    g_any_scrolling = g_state.is_dragging;

    // --- 惯性滚动 ---
    if (!g_state.is_touching && fabs(g_state.velocity) > INERTIA_STOP) {
        float s = cur_scroll + g_state.velocity;
        if (s < 0.0f)          { s = 0.0f;       g_state.velocity = 0.0f; }
        else if (s > max_scroll){ s = max_scroll;  g_state.velocity = 0.0f; }
        ImGui::SetScrollY(s);
        g_state.velocity *= INERTIA_DECAY;
    }

    ImGui::EndChild();
}

} // namespace TouchScrollable
