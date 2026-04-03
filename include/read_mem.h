#pragma once
#include "FrameSynchronizer.h"
#include "bullet_spread_monitor.h"
#include "mem_struct.h"
#include "driver_manager.h"
#include <atomic>
#include <string>
#include <cstdint>
#include <vector>
#include <memory>

// 共享状态（draw_menu.cpp 中定义）
extern std::atomic<int> driver_stat;
extern Offsets offset;
extern Addresses address;

// 读取线程 ↔ 渲染线程 双缓冲（仅用于 UI 信息显示）
extern FrameSynchronizer<ReadFrameData> gFrameSync;

// 骨骼名 → 自定义ID
enum BoneID : int {
    BONE_HEAD = 0,
    BONE_NECK,
    BONE_CHEST,
    BONE_PELVIS,
    BONE_L_SHOULDER,
    BONE_R_SHOULDER,
    BONE_L_ELBOW,
    BONE_R_ELBOW,
    BONE_L_HAND,
    BONE_R_HAND,
    BONE_L_KNEE,
    BONE_R_KNEE,
    BONE_L_FOOT,
    BONE_R_FOOT,
    BONE_ROOT,      // 根骨骼（用于 bottom label）
    BONE_COUNT
};

// Actor 类型分类
enum class ActorType : uint8_t {
    PLAYER,   // 玩家角色
    VEHICLE,  // 载具
    OTHER     // 其他（道具、NPC等）
};

// 缓存的 actor 地址（读取线程扫描，渲染线程使用）
struct CachedActor {
    uint64_t actorAddr;
    uint64_t rootCompAddr;
    std::string className;  // UE4 类名，扫描时读取
    std::string displayName; // 显示名称（从类名映射表获取）
    ActorType actorType;    // Actor 类型（读取线程分类）
    std::string playerName; // 玩家名称（UAECharacter::PlayerName）
    int teamID = -1;        // 队伍 ID（UAECharacter::TeamID），-1 表示未知
    int boneMap[BONE_COUNT]; // 骨骼ID → CST数组索引，-1表示未找到
    bool boneMapBuilt = false;  // 标记骨骼映射是否已构建
    uint64_t skelMeshCompAddr = 0;  // 缓存 SkeletalMeshComponent 地址（静态，避免每帧读取）
    uint64_t boneDataPtr = 0;       // 缓存 ComponentSpaceTransforms.Data（静态）
    int cachedBoneCount = 0;        // 缓存 ComponentSpaceTransforms.Num（静态）

    CachedActor() : actorAddr(0), rootCompAddr(0), actorType(ActorType::OTHER) {
        for (int i = 0; i < BONE_COUNT; i++) {
            boneMap[i] = -1;
        }
    }

    CachedActor(uint64_t addr, uint64_t root, std::string cls, std::string display, ActorType type)
        : actorAddr(addr), rootCompAddr(root), className(std::move(cls)),
          displayName(std::move(display)), actorType(type) {
        for (int i = 0; i < BONE_COUNT; i++) {
            boneMap[i] = -1;
        }
    }
};

// 分类后的 actor 列表（读取线程填充，渲染线程使用）
struct ClassifiedActors {
    std::vector<CachedActor> players;   // 玩家列表
    std::vector<CachedActor> vehicles;  // 载具列表
    std::vector<CachedActor> others;    // 其他列表
};

std::shared_ptr<ClassifiedActors> GetClassifiedActors();
std::shared_ptr<std::vector<CachedActor>> GetCachedActors();  // 兼容接口（合并三个列表）

// 内存读取工具函数
std::string GetNameByIndex(int32_t index, uint64_t libUE4);
std::string GetObjectName(uint64_t object, uint64_t libUE4);
Vec3 GetActorLocation(uint64_t Actor);
void hexdump(uint64_t addr, size_t size);
void DumpObjects(uint64_t libUE4, int count = 5);
void DumpTArray();
void DumpBones();
// 读取线程管理
void StartReadThread();
void StopReadThread();

// GUObjectArray 扫描
void ScanForClass(const char* className);
void FindUClass(const char* className);  // 查找 UClass 对象（类定义）
void FindSettingConfigViaGameInstance();  // 读取游戏 FPS Level
std::vector<std::pair<uint64_t, std::string>> GetScanResults();
void ClearScanResults();

// 初始化 driver（UI 线程调用）
void InitDriver(const char* packageName, uint64_t& libUE4Out);

// 获取游戏目标 FPS（从 FPS Level 读取）
int GetGameTargetFPS();
