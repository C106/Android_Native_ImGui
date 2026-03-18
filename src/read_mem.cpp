#include "read_mem.h"
#include "cpu_affinity.h"
#include "game_fps_monitor.h"
#include <android/log.h>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <string>

#ifdef NDEBUG
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#else
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "read_mem", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "read_mem", __VA_ARGS__)
#endif

extern std::atomic<bool> IsToolActive;
extern int gTargetFPS;  // 从 main.cpp 导入
extern bool gShowAllClassNames;  // 从 draw_objects.cpp 导入

// 双缓冲实例
FrameSynchronizer<ReadFrameData> gFrameSync;

// 读取线程内部状态
static std::thread gReadThread;
static std::atomic<bool> gReadThreadRunning{false};
static uint64_t sLibUE4 = 0; // 读取线程用的 libUE4 副本
static std::atomic<int> gGameFPSLevel{6}; // 游戏 FPS Level (默认 6=60fps)

// ═══════════════════════════════════════════
//  名称缓存
// ═══════════════════════════════════════════
static std::unordered_map<int32_t, std::string> nameCache;

// 类名分类映射表（类名 → {显示名称, 类型}）
struct ClassInfo {
    std::string displayName;
    ActorType type;
};

static const std::unordered_map<std::string, ClassInfo> kClassNameMap = {
    // 玩家角色
    {"BP_TrainPlayerPawn_C",    {"骨骼测试", ActorType::PLAYER}},
    {"BP_PlayerPawn_CG35_C",    {"骨骼测试", ActorType::PLAYER}},
    {"BP_PlayerPawn_C",         {"玩家", ActorType::PLAYER}},

    // 载具
    {"VH_4SportCar_C",            {"敞篷跑车", ActorType::VEHICLE}},
    {"BP_VH_Buggy_C",                {"双人赛车", ActorType::VEHICLE}},
    {"VH_Dacia_New_C",         {"轿车", ActorType::VEHICLE}},
    {"VH_UZA01_New_C",         {"吉普", ActorType::VEHICLE}},
    {"VH_CoupeRB_1_C",         {"轿跑", ActorType::VEHICLE}},
    {"VH_Drift_001_New_C",         {"拉力赛车", ActorType::VEHICLE}},
    {"VH_Horse_1_C",         {"马", ActorType::VEHICLE}},
    {"VH_PG117_C",         {"快艇", ActorType::VEHICLE}},
    {"VH_BROM_C",         {"装甲车", ActorType::VEHICLE}},
    {"PickUp_02_C",         {"小货车", ActorType::VEHICLE}},
    {"VH_StationWagon_New_C",         {"旅行车", ActorType::VEHICLE}},
    // 其他（道具、NPC等）
    // 用户按需填充
};

// 分类 actor（根据类名映射表）
// 返回 {displayName, actorType}，如果类名不在映射表中返回 {"", ActorType::OTHER}
static std::pair<std::string, ActorType> ClassifyActor(const std::string& className) {
    auto it = kClassNameMap.find(className);
    if (it != kClassNameMap.end()) {
        return {it->second.displayName, it->second.type};
    }
    return {"", ActorType::OTHER};  // 不在映射表中，归类为 OTHER 但不显示
}

// 骨骼名 → 自定义ID 映射表
static const std::unordered_map<std::string, int> gBoneNameToID = {
    {"head",              BONE_HEAD},
    {"neck_01",           BONE_NECK},
    {"spine_03",          BONE_CHEST},
    {"pelvis",            BONE_PELVIS},
    {"upperarm_l",        BONE_L_SHOULDER},
    {"upperarm_r",        BONE_R_SHOULDER},
    {"lowerarm_l",        BONE_L_ELBOW},
    {"lowerarm_r",        BONE_R_ELBOW},
    {"hand_l",            BONE_L_HAND},
    {"hand_r",            BONE_R_HAND},
    {"calf_l",            BONE_L_KNEE},
    {"calf_r",            BONE_R_KNEE},
    {"foot_l",            BONE_L_FOOT},
    {"foot_r",            BONE_R_FOOT},
    {"Root",              BONE_ROOT},      // 根骨骼（用于 bottom label）
};

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
    if (pid > 0) {
        printf("[+] pid = %d\n", pid);
        libUE4Out = Paradise_hook->get_module_base("libUE4.so");
        sLibUE4 = libUE4Out;
        address.libUE4 = libUE4Out;  // 保存到 address 结构体
        if(libUE4Out){
            printf("libUE4: %lx\n",libUE4Out);
        }

        StartGameFPSMonitor(packageName);
        EnableMemoryMode(true);  // 启用内存读取模式

        address.Matrix = Paradise_hook->read<uint64_t>(
            Paradise_hook->read<uint64_t>(libUE4Out + offset.CanvasMap) + 0x20) + 0x270;

        // LocalPlayerActor 由读取线程定期刷新（每 ~10 次扫描）

        // 启动读取线程
        StartReadThread();
    }
    // release: 保证 sLibUE4/address 写入对读取线程可见
    driver_stat.store(pid, std::memory_order_release);
}

void DumpTArray(){
    uint64_t Gworld = Paradise_hook->read<uint64_t>(sLibUE4 + offset.Gworld);
    uint64_t level = Paradise_hook->read<uint64_t>(Gworld + 0xB0);

    for (uint64_t off = 0x0; off <= 0x250; off += 0x8) {
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
        if (ca.className != "BP_TrainPlayerPawn_C") continue;

        uint64_t skelMeshComp = Paradise_hook->read<uint64_t>(ca.actorAddr + offset.SkeletalMeshComponent);
        if (skelMeshComp == 0) { printf("SkeletalMeshComponent is null\n"); continue; }

        int boneCount = Paradise_hook->read<int>(skelMeshComp + offset.ComponentSpaceTransforms + 0x8);
        if (boneCount != 66) continue;

        uint64_t skelMesh = Paradise_hook->read<uint64_t>(skelMeshComp + offset.SkeletalMesh);
        if (skelMesh == 0) { printf("SkeletalMesh is null\n"); continue; }

        // FReferenceSkeleton.RawRefBoneInfo at SkeletalMesh+0x238
        // FMeshBoneInfo: FName(8) + ParentIndex(4) + padding(4) = 0x10 stride
        uint64_t boneInfoPtr = Paradise_hook->read<uint64_t>(skelMesh + offset.RefBoneInfo);
        int boneInfoCount = Paradise_hook->read<int>(skelMesh + offset.RefBoneInfo + 0x8);

        printf("=== %s (0x%llX) BoneCount=%d ===\n",
               ca.className.c_str(), (unsigned long long)ca.actorAddr, boneInfoCount);

        // 读取 ComponentToWorld 用于诊断
        FTransform meshTransform = Paradise_hook->read<FTransform>(skelMeshComp + offset.ComponentToWorld);
        float mq2 = meshTransform.Rotation.X * meshTransform.Rotation.X
                  + meshTransform.Rotation.Y * meshTransform.Rotation.Y
                  + meshTransform.Rotation.Z * meshTransform.Rotation.Z
                  + meshTransform.Rotation.W * meshTransform.Rotation.W;
        const char* mtag = (mq2 < 0.9f || mq2 > 1.1f) ? " *** BAD" : "";
        printf("  ComponentToWorld: Rot(%.3f,%.3f,%.3f,%.3f)|%.3f%s Pos(%.1f,%.1f,%.1f) Scl(%.2f,%.2f,%.2f)\n",
               meshTransform.Rotation.X, meshTransform.Rotation.Y, meshTransform.Rotation.Z, meshTransform.Rotation.W, mq2, mtag,
               meshTransform.Translation.X, meshTransform.Translation.Y, meshTransform.Translation.Z,
               meshTransform.Scale3D.X, meshTransform.Scale3D.Y, meshTransform.Scale3D.Z);

        constexpr int kBoneInfoStride = 0x10;
        int count = (boneInfoCount > 0 && boneInfoCount < 300) ? boneInfoCount : boneCount;

        // 读取骨骼 transform 数据用于诊断
        uint64_t boneDataPtr = Paradise_hook->read<uint64_t>(skelMeshComp + offset.ComponentSpaceTransforms);
        for (int i = 0; i < count; i++) {
            int32_t nameIndex = Paradise_hook->read<int32_t>(boneInfoPtr + i * kBoneInfoStride);
            int32_t parentIdx = Paradise_hook->read<int32_t>(boneInfoPtr + i * kBoneInfoStride + 0x08);
            std::string boneName = GetNameByIndex(nameIndex, sLibUE4);

            // 按当前 48 字节 stride 读取
            FTransform bone;
            Paradise_hook->read(boneDataPtr + i * sizeof(FTransform), &bone, sizeof(FTransform));
            float quatLen2 = bone.Rotation.X * bone.Rotation.X
                           + bone.Rotation.Y * bone.Rotation.Y
                           + bone.Rotation.Z * bone.Rotation.Z
                           + bone.Rotation.W * bone.Rotation.W;
            const char* tag = (quatLen2 < 0.9f || quatLen2 > 1.1f) ? " *** BAD QUAT" : "";
            printf("  [%d] %s (parent=%d) Rot(%.3f,%.3f,%.3f,%.3f)|%.3f "
                   "Pos(%.1f,%.1f,%.1f) Scl(%.2f,%.2f,%.2f)%s\n",
                   i, boneName.c_str(), parentIdx,
                   bone.Rotation.X, bone.Rotation.Y, bone.Rotation.Z, bone.Rotation.W, quatLen2,
                   bone.Translation.X, bone.Translation.Y, bone.Translation.Z,
                   bone.Scale3D.X, bone.Scale3D.Y, bone.Scale3D.Z, tag);
        }
    }
}

// ═══════════════════════════════════════════
//  读取线程
// ═══════════════════════════════════════════

// 读取游戏 FPS Level（仅读取 FPS Level，不打印其他设置）
static int ReadGameFPSLevel() {
    if (!Paradise_hook || driver_stat.load(std::memory_order_relaxed) <= 0) {
        return 6; // 默认 60fps
    }

    // 通过 GameInstance → FrontendHUD → UserSettings 访问 SettingConfig
    uint64_t uworld = Paradise_hook->read<uint64_t>(sLibUE4 + offset.Gworld);
    if (uworld == 0) return 6;

    uint64_t gameInstance = Paradise_hook->read<uint64_t>(uworld + 0xb00);
    if (gameInstance == 0) return 6;

    uint64_t frontendHUD = Paradise_hook->read<uint64_t>(gameInstance + 0x4a8);
    if (frontendHUD == 0) return 6;

    uint64_t userSettings = Paradise_hook->read<uint64_t>(frontendHUD + 0xbc8);
    if (userSettings == 0) return 6;

    // 读取 FPS Level (offset 0x120)
    int fpsLevel = Paradise_hook->read<int32_t>(userSettings + 0x120);

    // 验证范围 (2-9)
    if (fpsLevel < 2 || fpsLevel > 9) {
        return 6; // 默认 60fps
    }

    return fpsLevel;
}

// FPS Level → 实际 FPS 映射
static int GetFPSFromLevel(int fpsLevel) {
    switch (fpsLevel) {
        case 2: return 20;
        case 3: return 25;
        case 4: return 30;
        case 5: return 40;
        case 6: return 60;
        case 7: return 90;
        case 8: return 120;
        case 9: return 144;
        default: return 60;  // 默认 60fps
    }
}

// 获取游戏目标 FPS（供外部调用）
int GetGameTargetFPS() {
    int fpsLevel = gGameFPSLevel.load(std::memory_order_relaxed);
    return GetFPSFromLevel(fpsLevel);
}

// FPS Level → 读取间隔（已废弃，读取线程固定 500ms）
// GetReadIntervalFromFPSLevel 不再使用

static std::mutex gActorListMtx;
static std::shared_ptr<ClassifiedActors> gClassifiedActors =
    std::make_shared<ClassifiedActors>();

// 新接口：获取分类后的 actor 列表
std::shared_ptr<ClassifiedActors> GetClassifiedActors() {
    std::lock_guard<std::mutex> lock(gActorListMtx);
    return gClassifiedActors;
}

// 旧接口：保留兼容性，返回所有 actor（合并三个列表）
std::shared_ptr<std::vector<CachedActor>> GetCachedActors() {
    std::lock_guard<std::mutex> lock(gActorListMtx);
    auto all = std::make_shared<std::vector<CachedActor>>();
    all->reserve(gClassifiedActors->players.size() +
                 gClassifiedActors->vehicles.size() +
                 gClassifiedActors->others.size());
    all->insert(all->end(), gClassifiedActors->players.begin(), gClassifiedActors->players.end());
    all->insert(all->end(), gClassifiedActors->vehicles.begin(), gClassifiedActors->vehicles.end());
    all->insert(all->end(), gClassifiedActors->others.begin(), gClassifiedActors->others.end());
    return all;
}

static void readThreadFunc() {
    SetThreadAffinity(CoreTier::MEDIUM);

    // 上一次的 uworld 地址，用于检测 world 切换
    uint64_t prevUworld = 0;

    while (gReadThreadRunning.load(std::memory_order_relaxed)) {
        if (driver_stat.load(std::memory_order_acquire) <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        ReadFrameData frame;

        // ── 每次扫描都读取 uworld（检测 world 切换）──
        frame.uworld = Paradise_hook->read<uint64_t>(sLibUE4 + offset.Gworld);
        if (frame.uworld == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // 检测 world 切换 → 清空 name 缓存
        if (frame.uworld != prevUworld) {
            nameCache.clear();
            nameCache.clear();
            prevUworld = frame.uworld;
        }

        // ── 每次扫描都刷新低频数据 ──

        // 刷新游戏 FPS Level
        int newFPSLevel = ReadGameFPSLevel();
        gGameFPSLevel.store(newFPSLevel, std::memory_order_relaxed);
        gTargetFPS = GetFPSFromLevel(newFPSLevel);

        // 刷新 LocalPlayerActor
        uint64_t netDriver = Paradise_hook->read<uint64_t>(frame.uworld + offset.NetDriver);
        if (netDriver != 0) {
            uint64_t serverConnection = Paradise_hook->read<uint64_t>(netDriver + offset.ServerConnection);
            uint64_t connection = 0;
            if (serverConnection != 0) {
                connection = serverConnection;
            } else {
                uint64_t clientConnectionsArray = netDriver + 0x90;
                uint64_t clientConnectionsData = Paradise_hook->read<uint64_t>(clientConnectionsArray);
                int clientConnectionsCount = Paradise_hook->read<int>(clientConnectionsArray + 0x8);
                if (clientConnectionsData != 0 && clientConnectionsCount > 0) {
                    connection = Paradise_hook->read<uint64_t>(clientConnectionsData);
                }
            }
            if (connection != 0) {
                uint64_t playerController = Paradise_hook->read<uint64_t>(connection + offset.PlayerController);
                if (playerController != 0) {
                    address.LocalPlayerActor = Paradise_hook->read<uint64_t>(playerController + offset.AcknowledgedPawn);
                }
            }
        }

        // ── Actor 列表扫描 ──
        frame.persistentLevel = Paradise_hook->read<uint64_t>(frame.uworld + offset.PersistentLevel);
        uint64_t TArray = Paradise_hook->read<uint64_t>(frame.persistentLevel + offset.TArray);
        frame.actorCount = Paradise_hook->read<int>(frame.persistentLevel + offset.TArray + 0x8);
        if (frame.actorCount <= 0) frame.actorCount = 0;

        std::vector<uint64_t> ptrBuf(frame.actorCount);
        Paradise_hook->read(TArray, ptrBuf.data(), frame.actorCount * sizeof(uint64_t));

        std::vector<CachedActor> newActors;
        newActors.reserve(frame.actorCount);

        for (int i = 0; i < frame.actorCount; i++) {
            uint64_t addr = ptrBuf[i];
            if (addr <= 0x10000000 || addr == 0 || addr % 4 != 0 || addr >= 0x10000000000)
                continue;

            // 每次都完整读取，不使用缓存
            uint64_t rootComp = Paradise_hook->read<uint64_t>(addr + offset.RootComponent);

            std::string className;
            uint64_t classPtr = Paradise_hook->read<uint64_t>(addr + 0x10);
            if (classPtr != 0) {
                int32_t classNameIdx = Paradise_hook->read<int32_t>(classPtr + 0x18);
                className = GetNameByIndex(classNameIdx, sLibUE4);
            }

            auto [displayName, actorType] = ClassifyActor(className);
            // 不过滤任何 actor，即使不在映射表中也保留（使用 className 显示）

            CachedActor ca(addr, rootComp, std::move(className), std::move(displayName), actorType);

            {
                uint64_t fstringData = Paradise_hook->read<uint64_t>(addr + offset.PlayerName);
                int32_t fstringLen = Paradise_hook->read<int32_t>(addr + offset.PlayerName + 0x8);
                if (fstringData != 0 && fstringLen > 0 && fstringLen < 128) {
                    std::vector<char16_t> utf16buf(fstringLen);
                    Paradise_hook->read(fstringData, utf16buf.data(), fstringLen * sizeof(char16_t));
                    std::string name;
                    name.reserve(fstringLen * 3);
                    for (int j = 0; j < fstringLen - 1; j++) {
                        char16_t c = utf16buf[j];
                        if (c == 0) break;
                        if (c < 0x80) {
                            name += (char)c;
                        } else if (c < 0x800) {
                            name += (char)(0xC0 | (c >> 6));
                            name += (char)(0x80 | (c & 0x3F));
                        } else {
                            name += (char)(0xE0 | (c >> 12));
                            name += (char)(0x80 | ((c >> 6) & 0x3F));
                            name += (char)(0x80 | (c & 0x3F));
                        }
                    }
                    ca.playerName = std::move(name);
                }
            }

            ca.teamID = Paradise_hook->read<int32_t>(addr + offset.TeamID);

            uint64_t skelMeshComp = Paradise_hook->read<uint64_t>(addr + offset.SkeletalMeshComponent);
            if (skelMeshComp != 0) {
                ca.skelMeshCompAddr = skelMeshComp;
                int boneCount = Paradise_hook->read<int>(skelMeshComp + offset.ComponentSpaceTransforms + 0x8);
                uint64_t boneDataPtr = Paradise_hook->read<uint64_t>(skelMeshComp + offset.ComponentSpaceTransforms);
                if (boneCount > 0 && boneCount < 300 && boneDataPtr != 0) {
                    ca.cachedBoneCount = boneCount;
                    ca.boneDataPtr = boneDataPtr;
                }

                uint64_t skelMesh = Paradise_hook->read<uint64_t>(skelMeshComp + offset.SkeletalMesh);
                if (skelMesh != 0) {
                    uint64_t boneInfoPtr = Paradise_hook->read<uint64_t>(skelMesh + offset.RefBoneInfo);
                    int boneInfoCount = Paradise_hook->read<int>(skelMesh + offset.RefBoneInfo + 0x8);

                    if (boneInfoCount > 0 && boneInfoCount < 300) {
                        constexpr int kBoneInfoStride = 0x10;
                        for (int j = 0; j < boneInfoCount; j++) {
                            int32_t nameIndex = Paradise_hook->read<int32_t>(boneInfoPtr + j * kBoneInfoStride);
                            std::string boneName = GetNameByIndex(nameIndex, sLibUE4);

                            auto it = gBoneNameToID.find(boneName);
                            if (it != gBoneNameToID.end()) {
                                ca.boneMap[it->second] = j;
                            }
                        }
                        ca.boneMapBuilt = true;
                    }
                }
            }

            newActors.push_back(std::move(ca));
        }

        // 分类
        auto classified = std::make_shared<ClassifiedActors>();
        for (auto& actor : newActors) {
            switch (actor.actorType) {
                case ActorType::PLAYER:
                    classified->players.push_back(std::move(actor));
                    break;
                case ActorType::VEHICLE:
                    classified->vehicles.push_back(std::move(actor));
                    break;
                case ActorType::OTHER:
                    classified->others.push_back(std::move(actor));
                    break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(gActorListMtx);
            gClassifiedActors = classified;
        }

        frame.valid = true;
        gFrameSync.submit(frame);

        // 固定 500ms 扫描间隔
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

// ═══════════════════════════════════════════
//  扫描 GUObjectArray 查找特定类的实例
// ═══════════════════════════════════════════

// 存储扫描结果
static std::vector<std::pair<uint64_t, std::string>> gScanResults;
static std::mutex gScanResultsMutex;
void DebugGUObjectArray() {
      printf("=== Debug GUObjectArray ===\n");
      printf("libUE4 base: 0x%lx\n", sLibUE4);

      // 测试不同的地址
      uint64_t test1 = sLibUE4 + 0x147063A0;
      uint64_t test2 = sLibUE4 + 0x14706480;
      uint64_t test3 = sLibUE4 + 0x1470653C;
      uint64_t test4 = sLibUE4 + 0x14706540;

      printf("GUObjectArray (0x147063A0): 0x%lx\n", test1);
      printf("ObjObjects (0x14706480): 0x%lx\n", test2);
      printf("NumElements addr (0x1470653C): 0x%lx\n", test3);
      printf("MaxElements addr (0x14706540): 0x%lx\n", test4);

      // 读取值
      int32_t numElements = Paradise_hook->read<int32_t>(test3);
      int32_t maxElements = Paradise_hook->read<int32_t>(test4);

      printf("NumElements value: %d\n", numElements);
      printf("MaxElements value: %d\n", maxElements);

      // 读取 chunk 指针
      uint64_t chunkPtr = Paradise_hook->read<uint64_t>(test2 + 0xC8);
      printf("First chunk pointer: 0x%lx\n", chunkPtr);

      // 读取第一个对象
      if (chunkPtr != 0) {
          uint64_t firstObj = Paradise_hook->read<uint64_t>(chunkPtr);
          printf("First object: 0x%lx\n", firstObj);
      }
      fflush(stdout);
}
void ScanForClass(const char* className) {
    printf("=== ScanForClass called for: %s ===\n", className);
    fflush(stdout);

    DebugGUObjectArray();

    if (!Paradise_hook || driver_stat.load(std::memory_order_relaxed) <= 0) {
        printf("ERROR: Driver not initialized\n");
        fflush(stdout);
        return;
    }

    std::vector<std::pair<uint64_t, std::string>> results;

    // 根据文档，offset.GUObject = 0x14706480 是 ObjObjects 的静态地址
    // 方法1: 使用 GUObjectArray 基址 (0x147063A0)
    uint64_t GUObjectArrayBase = sLibUE4 + 0x147063A0;

    // 读取 NumElements (GUObjectArray + 0x19C)
    int32_t NumElements = Paradise_hook->read<int32_t>(GUObjectArrayBase + 0x19C);
    int32_t MaxElements = Paradise_hook->read<int32_t>(GUObjectArrayBase + 0x1A0);

    printf("NumElements: %d, MaxElements: %d\n", NumElements, MaxElements);
    fflush(stdout);

    if (NumElements <= 0 || NumElements > 2000000) {
        printf("ERROR: Invalid NumElements: %d\n", NumElements);
        fflush(stdout);
        return;
    }

    // ObjObjects 在 GUObjectArray + 0xE0
    uint64_t ObjObjectsBase = GUObjectArrayBase + 0xE0;

    // 读取第一个 chunk 指针 (ObjObjects + 0xC8)
    uint64_t FirstChunkPtr = Paradise_hook->read<uint64_t>(ObjObjectsBase + 0xC8);

    printf("ObjObjectsBase: 0x%lx\n", ObjObjectsBase);
    printf("FirstChunkPtr: 0x%lx\n", FirstChunkPtr);
    fflush(stdout);

    if (FirstChunkPtr == 0) {
        printf("ERROR: FirstChunkPtr is NULL!\n");
        fflush(stdout);

        // 尝试备用方法：直接从 offset.GUObject 读取
        printf("Trying alternative method...\n");
        fflush(stdout);
        uint64_t altChunkPtr = Paradise_hook->read<uint64_t>(sLibUE4 + offset.GUObject);
        printf("Alternative chunk pointer: 0x%lx\n", altChunkPtr);
        fflush(stdout);

        if (altChunkPtr == 0) {
            printf("ERROR: Alternative method also failed\n");
            fflush(stdout);
            return;
        }
        FirstChunkPtr = altChunkPtr;
    }

    printf("Scanning GUObjectArray: FirstChunk=0x%lx, Num=%d\n", FirstChunkPtr, NumElements);
    printf("Looking for class: %s\n", className);
    fflush(stdout);

    int foundCount = 0;
    int sampleCount = 0;
    int validObjectCount = 0;
    std::string targetClassName(className);

    // 遍历对象数组（简化版：假设只有一个 chunk）
    for (int i = 0; i < NumElements && i < 500000; i++) {
        // FUObjectItem 大小 = 0x18 (24 字节)
        // +0x00: UObject* Object
        uint64_t objectPtr = Paradise_hook->read<uint64_t>(FirstChunkPtr + i * 0x18);

        if (objectPtr == 0 || objectPtr < 0x10000ULL || objectPtr > 0x800000000000ULL) {
            continue;
        }

        validObjectCount++;

        // 获取对象名称
        std::string objName = GetObjectName(objectPtr, sLibUE4);

        // 打印前 50 个有效对象的详细信息
        if (sampleCount < 50 && !objName.empty()) {
            // 读取对象的类指针
            uint64_t classPtr = Paradise_hook->read<uint64_t>(objectPtr + 0x10);
            std::string className_actual;
            if (classPtr != 0) {
                int32_t classNameIdx = Paradise_hook->read<int32_t>(classPtr + 0x18);
                className_actual = GetNameByIndex(classNameIdx, sLibUE4);
            }

            printf("Sample[%d] Obj=0x%lx Name=%s Class=%s\n",
                   sampleCount, objectPtr, objName.c_str(), className_actual.c_str());
            fflush(stdout);
            sampleCount++;
        }

        // 检查是否包含目标类名（不区分大小写）
        std::string objNameLower = objName;
        std::string targetLower = targetClassName;
        std::transform(objNameLower.begin(), objNameLower.end(), objNameLower.begin(), ::tolower);
        std::transform(targetLower.begin(), targetLower.end(), targetLower.begin(), ::tolower);

        if (objNameLower.find(targetLower) != std::string::npos) {
            // 读取类名进行验证
            uint64_t classPtr = Paradise_hook->read<uint64_t>(objectPtr + 0x10);
            std::string className_actual;
            if (classPtr != 0) {
                int32_t classNameIdx = Paradise_hook->read<int32_t>(classPtr + 0x18);
                className_actual = GetNameByIndex(classNameIdx, sLibUE4);
            }

            results.push_back({objectPtr, objName + " [Class: " + className_actual + "]"});
            foundCount++;
            printf("[%d] Found: 0x%lx -> %s (Class: %s)\n",
                   foundCount, objectPtr, objName.c_str(), className_actual.c_str());
            fflush(stdout);

            // 限制结果数量，避免过多
            if (foundCount >= 100) {
                printf("Reached limit of 100 results, stopping scan\n");
                fflush(stdout);
                break;
            }
        }

        // 每 10000 个对象打印一次进度
        if (i > 0 && i % 10000 == 0) {
            printf("Progress: %d/%d objects scanned, %d valid, found %d matches\n",
                   i, NumElements, validObjectCount, foundCount);
            fflush(stdout);
        }
    }

    printf("Scan complete: scanned %d objects, %d valid, found %d instances of %s\n",
           NumElements, validObjectCount, foundCount, className);
    fflush(stdout);

    // 保存结果
    {
        std::lock_guard<std::mutex> lock(gScanResultsMutex);
        gScanResults = std::move(results);
    }
}

std::vector<std::pair<uint64_t, std::string>> GetScanResults() {
    std::lock_guard<std::mutex> lock(gScanResultsMutex);
    return gScanResults;
}

void ClearScanResults() {
    std::lock_guard<std::mutex> lock(gScanResultsMutex);
    gScanResults.clear();
}

// 专门查找 UClass 对象（类定义本身）
void FindUClass(const char* className) {
    printf("=== FindUClass called for: %s ===\n", className);
    fflush(stdout);

    if (!Paradise_hook || driver_stat.load(std::memory_order_relaxed) <= 0) {
        printf("ERROR: Driver not initialized\n");
        fflush(stdout);
        return;
    }

    std::vector<std::pair<uint64_t, std::string>> results;

    uint64_t GUObjectArrayBase = sLibUE4 + 0x147063A0;
    int32_t NumElements = Paradise_hook->read<int32_t>(GUObjectArrayBase + 0x19C);
    uint64_t ObjObjectsBase = GUObjectArrayBase + 0xE0;
    uint64_t FirstChunkPtr = Paradise_hook->read<uint64_t>(ObjObjectsBase + 0xC8);

    if (FirstChunkPtr == 0) {
        printf("ERROR: FirstChunkPtr is NULL!\n");
        fflush(stdout);
        return;
    }

    printf("Looking for UClass: %s\n", className);
    fflush(stdout);

    int foundCount = 0;
    std::string targetClassName(className);
    std::transform(targetClassName.begin(), targetClassName.end(), targetClassName.begin(), ::tolower);

    for (int i = 0; i < NumElements && i < 500000; i++) {
        uint64_t objectPtr = Paradise_hook->read<uint64_t>(FirstChunkPtr + i * 0x18);

        if (objectPtr == 0 || objectPtr < 0x10000ULL || objectPtr > 0x800000000000ULL) {
            continue;
        }

        // 读取对象的类指针
        uint64_t classPtr = Paradise_hook->read<uint64_t>(objectPtr + 0x10);
        if (classPtr == 0) continue;

        // 获取类的名称
        int32_t classNameIdx = Paradise_hook->read<int32_t>(classPtr + 0x18);
        std::string className_actual = GetNameByIndex(classNameIdx, sLibUE4);

        // 检查这个对象的类是否是 "Class"（即这个对象本身是一个 UClass）
        std::string classNameLower = className_actual;
        std::transform(classNameLower.begin(), classNameLower.end(), classNameLower.begin(), ::tolower);

        if (classNameLower == "class") {
            // 这是一个 UClass 对象，获取它的名称
            std::string objName = GetObjectName(objectPtr, sLibUE4);
            std::string objNameLower = objName;
            std::transform(objNameLower.begin(), objNameLower.end(), objNameLower.begin(), ::tolower);

            // 检查是否匹配目标类名
            if (objNameLower.find(targetClassName) != std::string::npos) {
                results.push_back({objectPtr, objName});
                foundCount++;
                printf("[%d] Found UClass: 0x%lx -> %s\n", foundCount, objectPtr, objName.c_str());
                fflush(stdout);

                if (foundCount >= 100) {
                    break;
                }
            }
        }

        if (i > 0 && i % 10000 == 0) {
            printf("Progress: %d/%d objects scanned, found %d UClass matches\n", i, NumElements, foundCount);
            fflush(stdout);
        }
    }

    printf("FindUClass complete: found %d UClass objects matching %s\n", foundCount, className);
    fflush(stdout);

    // 保存结果
    {
        std::lock_guard<std::mutex> lock(gScanResultsMutex);
        gScanResults = std::move(results);
    }
}

// 通过 GameInstance → FrontendHUD → UserSettings 访问 SettingConfig
void FindSettingConfigViaGameInstance() {
    printf("=== Reading Game FPS Level ===\n");
    fflush(stdout);

    if (!Paradise_hook || driver_stat.load(std::memory_order_relaxed) <= 0) {
        printf("ERROR: Driver not initialized\n");
        fflush(stdout);
        return;
    }

    // 1. 从 UWorld 获取 GameInstance
    uint64_t uworld = Paradise_hook->read<uint64_t>(sLibUE4 + offset.Gworld);
    if (uworld == 0) {
        printf("ERROR: UWorld is NULL\n");
        fflush(stdout);
        return;
    }

    uint64_t gameInstance = Paradise_hook->read<uint64_t>(uworld + 0xb00);
    if (gameInstance == 0) {
        printf("ERROR: GameInstance not found\n");
        fflush(stdout);
        return;
    }

    // 2. 从 GameInstance 获取 AssociatedFrontendHUD (offset 0x4a8)
    uint64_t frontendHUD = Paradise_hook->read<uint64_t>(gameInstance + 0x4a8);
    if (frontendHUD == 0) {
        printf("ERROR: AssociatedFrontendHUD is NULL\n");
        fflush(stdout);
        return;
    }

    // 3. 从 FrontendHUD 读取 UserSettings (offset 0xbc8)
    uint64_t userSettings = Paradise_hook->read<uint64_t>(frontendHUD + 0xbc8);
    if (userSettings == 0) {
        printf("ERROR: UserSettings is NULL\n");
        fflush(stdout);
        return;
    }

    // 4. 读取 FPS Level (offset 0x120)
    int fpsLevel = Paradise_hook->read<int32_t>(userSettings + 0x120);

    // FPS Level 映射
    const char* fpsMapping[] = {
        nullptr, nullptr,
        "20fps", "25fps", "30fps", "40fps", "60fps", "90fps", "120fps", "144fps"
    };

    printf("FPS Level: %d", fpsLevel);
    if (fpsLevel >= 2 && fpsLevel <= 9) {
        printf(" (%s)\n", fpsMapping[fpsLevel]);
    } else {
        printf(" (Invalid)\n");
    }

    printf("Read interval: 500ms (fixed)\n");
    fflush(stdout);
}
