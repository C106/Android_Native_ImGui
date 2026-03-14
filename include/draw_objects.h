#pragma once

#include "mem_struct.h"

// 单帧游戏数据快照（在 fence wait 前读取）
struct GameFrameData {
    FMatrix VPMat;
    Vec3 localPlayerPos;
    bool valid;
};

extern bool gShowObjects;
extern bool gShowAllClassNames;
extern bool gUseBatchBoneRead;  // true=批量读取（优化），false=逐个读取（调试）
extern int gBoneCount;
extern float gMaxSkeletonDistance;  // 超过此距离不绘制骨骼（米）

// 读取游戏数据（在 fence wait 前调用）
GameFrameData ReadGameData();

// 使用预读数据绘制（在 fence wait 后调用）
void DrawObjectsWithData(const GameFrameData& data);

// 旧接口（保留兼容性）
void DrawObjects();
