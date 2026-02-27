#pragma once
#include "FrameSynchronizer.h"
#include "mem_struct.h"
#include "driver.h"
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

// 缓存的 actor 地址（读取线程扫描，渲染线程使用）
struct CachedActor {
    uint64_t actorAddr;
    uint64_t rootCompAddr;
    std::string className;  // UE4 类名，扫描时读取
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
