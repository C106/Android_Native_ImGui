#pragma once

#include "draw_objects.h"

// 读取游戏数据（在 fence wait 前调用）
GameFrameData ReadGameData();

// 仅读取 GFrameCounter（轻量，用于帧同步轮询）
uint64_t ReadFrameCounter();
