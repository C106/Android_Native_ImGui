#pragma once
#include "FrameSynchronizer.h"
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
    BONE_COUNT
};

// 缓存的 actor 地址（读取线程扫描，渲染线程使用）
struct CachedActor {
    uint64_t actorAddr;
    uint64_t rootCompAddr;
    std::string className;  // UE4 类名，扫描时读取
    int boneMap[BONE_COUNT]; // 骨骼ID → CST数组索引，-1表示未找到
    bool boneMapBuilt = false;  // 标记骨骼映射是否已构建

    CachedActor() : actorAddr(0), rootCompAddr(0) {
        for (int i = 0; i < BONE_COUNT; i++) boneMap[i] = -1;
    }

    CachedActor(uint64_t addr, uint64_t root, std::string cls)
        : actorAddr(addr), rootCompAddr(root), className(std::move(cls)) {
        for (int i = 0; i < BONE_COUNT; i++) boneMap[i] = -1;
    }
};
std::shared_ptr<std::vector<CachedActor>> GetCachedActors();

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

// 初始化 driver（UI 线程调用）
void InitDriver(const char* packageName, uint64_t& libUE4Out);
