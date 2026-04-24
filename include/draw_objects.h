#pragma once

#include "mem_struct.h"
#include "read_mem.h"
#include <unordered_map>
#include <memory>

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
    bool visible[BONE_COUNT];
    float distance;
    int teamID;
    uint32_t playerKey;
    uint64_t actorAddr;
    uint64_t frameCounter;
    bool valid;
    // auto-aim 直读骨骼所需的静态指针（从 CachedActor 拷贝）
    uint64_t skelMeshCompAddr;
    uint64_t boneDataPtr;
    int boneMap[BONE_COUNT];
    int cachedBoneCount;
};

using BoneScreenCache = std::unordered_map<uint64_t, BoneScreenData>;
using BoneScreenCacheSnapshot = std::shared_ptr<const BoneScreenCache>;

extern bool gShowObjects;
extern bool gShowAllClassNames;
extern bool gUseBatchBoneRead;  // true=批量读取（优化），false=逐个读取（调试）
extern bool gEnableBoneSmoothing;  // true=骨骼插值平滑，false=低延迟直出
extern bool gUseCameraCacheVPMatrix;  // true=使用 CameraCache/MinimalViewInfo 构建 VP，false=使用现有矩阵地址
extern int gBoneCount;
extern float gMaxSkeletonDistance;  // 超过此距离不绘制骨骼（米）

// 分类显示开关
extern bool gShowPlayers;   // 显示玩家
extern bool gShowBots;      // 显示 Bot
extern bool gShowNPCs;      // 显示 NPC
extern bool gShowMonsters;  // 显示 Monster
extern bool gShowTombBoxes; // 显示战利品箱
extern bool gShowOtherBoxes;// 显示其他盒子
extern bool gShowEscapeBoxes;// 显示宝箱
extern bool gShowContainers;// 显示容器
extern bool gShowVehicles;  // 显示载具
extern bool gShowOthers;    // 显示其他

// 绘制模块开关（细粒度控制）
extern bool gDrawSkeleton;   // 绘制骨骼线条
extern bool gDrawPredictedAimPoint; // 绘制预判目标点
extern bool gDrawDistance;   // 绘制距离信息
extern bool gDrawName;       // 绘制名称标签
extern bool gDrawBox;        // 绘制包围盒（预留）
extern bool gDrawPhysXGeometry; // 绘制 PhysX 几何体
extern bool gPhysXDrawMeshes; // 绘制 TriangleMesh / ConvexMesh
extern bool gPhysXDrawPrimitives; // 绘制 Box / Capsule / Sphere
extern float gPhysXDrawRadiusMeters; // 最大读取半径
extern int gPhysXMaxActorsPerFrame; // 每帧最多遍历的 PhysX actor 数
extern int gPhysXMaxShapesPerActor; // 每个 actor 最多处理的 shape 数
extern int gPhysXMaxTrianglesPerMesh; // 每个 mesh 最多绘制的三角形数
extern float gPhysXCenterRegionFovDegrees; // 仅绘制准星附近时的屏幕区域参数
extern bool gPhysXManualSceneIndexEnabled; // 是否手动指定 PxScene index
extern int gPhysXManualSceneIndex; // 手动指定的 PxScene index
extern bool gPhysXUseLocalModelData; // 是否只读取本地导出的模型数据
extern bool gPhysXAutoExport;       // 自动导出未缓存的模型到磁盘
extern float gPhysXGeomRefreshInterval; // 几何缓存刷新间隔（秒），大间隔
extern int gPhysXMaxPrunerObjectsPerFrame; // 每帧最多读取的 Pruner 对象数
extern int gPhysXMaxPrunerObjectCount;     // Pruner 对象总数合法性上限

// 骨骼可视性判断
extern bool gUseDepthBufferVisibility;  // 使用 CPU raycaster + 本地 BVH 做骨骼可视性
extern float gDepthBufferBias;          // 光栅化深度偏移
extern float gDepthBufferTolerance;     // 查询深度容差
extern int gDepthBufferDownscale;       // 降采样倍率
extern bool gDrawDepthBuffer;           // 调试：直接绘制深度缓冲

// 小地图
extern bool gDrawMiniMap;               // 绘制旋转小地图
extern float gMiniMapPosX;              // 左上角 X（像素）
extern float gMiniMapPosY;              // 左上角 Y（像素）
extern float gMiniMapSizePx;            // 小地图边长（像素）
extern float gMiniMapZoomMeters;        // 半径对应的世界距离（米）

// 使用预读数据绘制（在 fence wait 后调用）
// gameStepDeltaTime: 与游戏帧同步的时间步（秒），用于骨骼插值
void DrawObjectsWithData(const GameFrameData& data, float gameStepDeltaTime);

// 旧接口（保留兼容性）
void DrawObjects();

void ShutdownDrawObjects();

// 供 auto-aim 访问骨骼缓存（返回拷贝，线程安全）
BoneScreenCacheSnapshot GetBoneScreenCacheSnapshot();
bool GetCachedBoneWorldPos(uint64_t actorAddr, int boneID, uint64_t frameCounter, Vec3& outWorldPos);
bool ExportStablePhysXMeshes();
const char* GetStablePhysXExportStatus();

// 测试：从内存导出带BVH的mesh
bool TestExportBVHMesh();
