#pragma once
#include "imgui.h"
#include "imgui_internal.h"

// 触摸滚动区域：BeginChild 的触摸友好封装
// 在 End() 中检测触摸拖动并转换为滚动，自动跳过控件交互区域
namespace TouchScrollable {

bool Begin(const char* id, const ImVec2& size = ImVec2(0, 0), ImGuiWindowFlags flags = 0);
void End();

// 是否有滚动区域正在被拖动（用于阻止外部窗口拖动）
bool IsScrolling();

}
