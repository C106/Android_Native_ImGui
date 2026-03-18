#pragma once

#include "mem_struct.h"
#include "read_mem.h"
#include <unordered_map>

// 单帧游戏数据快照（在 fence wait 前读取）
struct GameFrameData {
    FMatrix VPMat;
    Vec3 localPlayerPos;
    float gameDeltaTime;    // 引擎 DeltaTime（秒）
    uint64_t frameCounter;  // 引擎 GFrameCounter，用于检测帧是否更新
    bool valid;
};

// 骨骼屏幕数据（供 auto-aim 使用）
struct BoneScreenData {
    Vec2 screenPos[BONE_COUNT];
    bool onScreen[BONE_COUNT];
    float distance;
    int teamID;
    uint64_t actorAddr;
    uint64_t frameCounter;
    bool valid;
};

extern bool gShowObjects;
extern bool gShowAllClassNames;
extern bool gUseBatchBoneRead;  // true=批量读取（优化），false=逐个读取（调试）
extern bool gEnableBoneSmoothing;  // true=骨骼插值平滑，false=低延迟直出
extern int gBoneCount;
extern float gMaxSkeletonDistance;  // 超过此距离不绘制骨骼（米）

// 分类显示开关
extern bool gShowPlayers;   // 显示玩家
extern bool gShowVehicles;  // 显示载具
extern bool gShowOthers;    // 显示其他

// 绘制模块开关（细粒度控制）
extern bool gDrawSkeleton;   // 绘制骨骼线条
extern bool gDrawDistance;   // 绘制距离信息
extern bool gDrawName;       // 绘制名称标签
extern bool gDrawBox;        // 绘制包围盒（预留）

// 读取游戏数据（在 fence wait 前调用）
GameFrameData ReadGameData();

// 仅读取 GFrameCounter（轻量，用于帧同步轮询）
uint64_t ReadFrameCounter();

// 使用预读数据绘制（在 fence wait 后调用）
// renderDeltaTime: 渲染帧间隔（秒），用于骨骼插值
void DrawObjectsWithData(const GameFrameData& data, float renderDeltaTime);

// 旧接口（保留兼容性）
void DrawObjects();

void ShutdownDrawObjects();

// 供 auto-aim 访问骨骼缓存（同线程调用，返回 const 引用）
const std::unordered_map<uint64_t, BoneScreenData>& GetBoneScreenCache();
