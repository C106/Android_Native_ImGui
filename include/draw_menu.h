#pragma once
#include "ImGuiLayer.h"

// 帧率控制（通过 UI 滑动条调节）
extern int gTargetFPS;

void Draw_Menu();
void Draw_Menu_Overlay();
void Draw_Menu_ResetTextures();

// Tab 绘制函数
void DrawCameraTab();
void DrawObjViewTab();
void DrawConfigTab();
