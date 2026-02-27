#include "read_mem.h"
#include <android/log.h>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <vector>
#include <memory>

#ifdef NDEBUG
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#else
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "read_mem", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "read_mem", __VA_ARGS__)
#endif

extern std::atomic<bool> IsToolActive;
extern Paradise_hook_driver* Paradise_hook;

// 双缓冲实例
FrameSynchronizer<ReadFrameData> gFrameSync;

// 读取线程内部状态
static std::thread gReadThread;
static std::atomic<bool> gReadThreadRunning{false};
static uint64_t sLibUE4 = 0; // 读取线程用的 libUE4 副本

// ═══════════════════════════════════════════
//  名称缓存
// ═══════════════════════════════════════════
static std::unordered_map<int32_t, std::string> nameCache;

std::string GetNameByIndex(int32_t index, uint64_t libUE4) {
    auto it = nameCache.find(index);
    if (it != nameCache.end()) return it->second;

    constexpr int ElementsPerChunk = 16384;
    int chunkIdx  = index / ElementsPerChunk;
    int withinIdx = index % ElementsPerChunk;

    uint64_t chunksArr = Paradise_hook->read<uint64_t>(libUE4 + offset.Gname);
    uint64_t chunk     = Paradise_hook->read<uint64_t>(chunksArr + chunkIdx * 8);
    uint64_t entryAddr = Paradise_hook->read<uint64_t>(chunk + withinIdx * 8);

    if (entryAddr == 0) return "";

    char buf[65] = {};
    Paradise_hook->read(entryAddr + 0x0C, buf, 64);

    std::string name(buf);
    nameCache[index] = name;
    return name;
}

std::string GetObjectName(uint64_t object, uint64_t libUE4) {
    int32_t nameIndex = Paradise_hook->read<int32_t>(object + 0x18);
    return GetNameByIndex(nameIndex, libUE4);
}

Vec3 GetActorLocation(uint64_t Actor) {
    uint64_t RootComp = Paradise_hook->read<uint64_t>(Actor + offset.RootComponent);
    if (RootComp == 0) return {0, 0, 0};
    return Paradise_hook->read<Vec3>(RootComp + offset.ComponentToWorld + 0x10);
}

void hexdump(uint64_t addr, size_t size) {
    std::vector<uint8_t> buf(size);
    Paradise_hook->read(addr, buf.data(), size);

    for (size_t i = 0; i < size; i += 16) {
        printf("%012lx: ", addr + i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < size)
                printf("%02x ", buf[i + j]);
            else
                printf("   ");
            if (j == 7) printf(" ");
        }
        printf(" |");
        for (size_t j = 0; j < 16 && (i + j) < size; j++) {
            uint8_t c = buf[i + j];
            printf("%c", (c >= 0x20 && c <= 0x7e) ? c : '.');
        }
        printf("|\n");
    }
}

void DumpObjects(uint64_t libUE4, int count) {
    uint64_t chunk0 = Paradise_hook->read<uint64_t>(libUE4 + offset.GUObject);
    int numElements = Paradise_hook->read<int32_t>(libUE4 + offset.GUObject + 0x10);

    if (count > numElements) count = numElements;

    for (int i = 0; i < count; i++) {
        uint64_t object = Paradise_hook->read<uint64_t>(chunk0 + i * 0x18);
        if (object == 0) continue;

        int32_t nameIndex = Paradise_hook->read<int32_t>(object + 0x18);
        int32_t nameNumber = Paradise_hook->read<int32_t>(object + 0x1C);

        std::string name = GetNameByIndex(nameIndex, libUE4);
        if (nameNumber > 0) {
            name += "_" + std::to_string(nameNumber - 1);
        }

        uint64_t classPtr = Paradise_hook->read<uint64_t>(object + 0x10);
        std::string className;
        if (classPtr != 0) {
            int32_t classNameIdx = Paradise_hook->read<int32_t>(classPtr + 0x18);
            className = GetNameByIndex(classNameIdx, libUE4);
        }

        uint64_t outerPtr = Paradise_hook->read<uint64_t>(object + 0x28);
        std::string outerName;
        if (outerPtr != 0) {
            int32_t outerNameIdx = Paradise_hook->read<int32_t>(outerPtr + 0x18);
            outerName = GetNameByIndex(outerNameIdx, libUE4);
        }

        printf("[%d] %s (%s) Outer:%s\n", i, name.c_str(), className.c_str(), outerName.c_str());
    }
}

// ═══════════════════════════════════════════
//  Driver 初始化（UI 线程调用一次）
// ═══════════════════════════════════════════
void InitDriver(const char* packageName, uint64_t& libUE4Out) {
    int pid = Paradise_hook->get_pid(packageName);
    Paradise_hook->initialize(pid);
    if (pid > 0) {
        libUE4Out = Paradise_hook->get_module_base("libUE4.so");
        sLibUE4 = libUE4Out;
        address.Matrix = Paradise_hook->read<uint64_t>(
            Paradise_hook->read<uint64_t>(libUE4Out + offset.CanvasMap) + 0x20) + 0x270;

        uint64_t level = Paradise_hook->read<uint64_t>(
            Paradise_hook->read<uint64_t>(libUE4Out + offset.Gworld) + 0xB0);
        LOGI("PersistentLevel = 0x%lX", level);
    }
    // release: 保证 sLibUE4/address 写入对读取线程可见
    driver_stat.store(pid, std::memory_order_release);
}

void DumpTArray(){
    for (uint64_t off = 0x0; off <= 0x250; off += 0x8) {
        uint64_t level = Paradise_hook->read<uint64_t>(
            Paradise_hook->read<uint64_t>(sLibUE4 + offset.Gworld) + 0xB0);
        uint64_t ptr = Paradise_hook->read<uint64_t>(level + off);
        int32_t count = Paradise_hook->read<int32_t>(level + off + 0x8);
        int32_t max   = Paradise_hook->read<int32_t>(level + off + 0xC);

        if (ptr > 0x10000ULL && ptr < 0x800000000000ULL
                    && count > 5 && count <= max && max < 100000) {
            printf("ULevel+0x%03llX -> Data=0x%llX  Count=%d  Max=%d\n", off, ptr, count, max);
            // 验证前3个元素
            for (int i = 0; i < 150 && i < count; i++) {
                uint64_t actor = Paradise_hook->read<uint64_t>(ptr + i * 8);
                if (actor) {
                    std::string name = GetObjectName(actor,sLibUE4);
                    if(name.length()<9){
                        continue;
                    }
                    printf("    [%d] 0x%llX -> %s\n", i, actor, name.c_str());
                }
            }
        }
    }
}

// ═══════════════════════════════════════════
//  Dump Bones
// ═══════════════════════════════════════════
void DumpBones() {
    auto actors = GetCachedActors();
    if (!actors) return;

    for (const auto& ca : *actors) {
        if (strncmp(ca.className.c_str(), "BP_TrainPlayerPawn_C", 5) != 0) continue;

        uint64_t skelMeshComp = Paradise_hook->read<uint64_t>(ca.actorAddr + offset.SkeletalMeshComponent);
        if (skelMeshComp == 0) { printf("SkeletalMeshComponent is null\n"); continue; }

        int boneCount = Paradise_hook->read<int>(skelMeshComp + offset.ComponentSpaceTransforms + 0x8);

        uint64_t skelMesh = Paradise_hook->read<uint64_t>(skelMeshComp + offset.SkeletalMesh);
        if (skelMesh == 0) { printf("SkeletalMesh is null\n"); continue; }

        // FReferenceSkeleton.RawRefBoneInfo at SkeletalMesh+0x238
        // FMeshBoneInfo: FName(8) + ParentIndex(4) + padding(4) = 0x10 stride
        uint64_t boneInfoPtr = Paradise_hook->read<uint64_t>(skelMesh + offset.RefBoneInfo);
        int boneInfoCount = Paradise_hook->read<int>(skelMesh + offset.RefBoneInfo + 0x8);

        printf("=== %s (0x%llX) BoneCount=%d ===\n",
               ca.className.c_str(), (unsigned long long)ca.actorAddr, boneInfoCount);

        constexpr int kBoneInfoStride = 0x10;
        int count = (boneInfoCount > 0 && boneInfoCount < 300) ? boneInfoCount : boneCount;
        for (int i = 0; i < count; i++) {
            int32_t nameIndex = Paradise_hook->read<int32_t>(boneInfoPtr + i * kBoneInfoStride);
            int32_t parentIdx = Paradise_hook->read<int32_t>(boneInfoPtr + i * kBoneInfoStride + 0x08);
            std::string boneName = GetNameByIndex(nameIndex, sLibUE4);
            printf("  [CST:%d] %s (parent=%d)\n", i, boneName.c_str(), parentIdx);
        }
    }
}

// ═══════════════════════════════════════════
//  读取线程
// ═══════════════════════════════════════════
static std::mutex gActorListMtx;
static std::shared_ptr<std::vector<CachedActor>> gSharedActors =
    std::make_shared<std::vector<CachedActor>>();

std::shared_ptr<std::vector<CachedActor>> GetCachedActors() {
    std::lock_guard<std::mutex> lock(gActorListMtx);
    return gSharedActors;
}

static void readThreadFunc() {
    while (gReadThreadRunning.load(std::memory_order_relaxed)) {
        if (driver_stat.load(std::memory_order_acquire) <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        ReadFrameData frame;

        frame.uworld = Paradise_hook->read<uint64_t>(sLibUE4 + offset.Gworld);
        if (frame.uworld == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        frame.persistentLevel = Paradise_hook->read<uint64_t>(frame.uworld + offset.PersistentLevel);
        uint64_t TArray = Paradise_hook->read<uint64_t>(frame.persistentLevel + offset.TArray);
        frame.actorCount = Paradise_hook->read<int>(frame.persistentLevel + offset.TArray + 0x8);
        if (frame.actorCount <= 0) frame.actorCount = 0;

        // 扫描 actor 列表，缓存地址 + RootComponent 指针
        std::vector<CachedActor> newActors;
        newActors.reserve(frame.actorCount);

        std::vector<uint64_t> ptrBuf(frame.actorCount);
        Paradise_hook->read(TArray, ptrBuf.data(), frame.actorCount * sizeof(uint64_t));

        for (int i = 0; i < frame.actorCount; i++) {
            uint64_t addr = ptrBuf[i];
            if (addr <= 0x10000000 || addr == 0 || addr % 4 != 0 || addr >= 0x10000000000)
                continue;
            uint64_t rootComp = Paradise_hook->read<uint64_t>(addr + offset.RootComponent);

            // 读取 actor 类名（2 次 ioctl，nameCache 自动缓存重复类名）
            std::string className;
            uint64_t classPtr = Paradise_hook->read<uint64_t>(addr + 0x10);
            if (classPtr != 0) {
                int32_t classNameIdx = Paradise_hook->read<int32_t>(classPtr + 0x18);
                className = GetNameByIndex(classNameIdx, sLibUE4);
            }

            newActors.push_back({addr, rootComp, std::move(className)});
        }

        {
            auto newList = std::make_shared<std::vector<CachedActor>>(std::move(newActors));
            std::lock_guard<std::mutex> lock(gActorListMtx);
            gSharedActors = newList;
        }

        // 提交基本信息供 UI 显示
        frame.valid = true;
        gFrameSync.submit(frame);

        // 扫描间隔 500ms
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void StartReadThread() {
    if (gReadThreadRunning.load()) return;
    gReadThreadRunning.store(true);
    gReadThread = std::thread(readThreadFunc);
}

void StopReadThread() {
    gReadThreadRunning.store(false);
    if (gReadThread.joinable())
        gReadThread.join();
}
