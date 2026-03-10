#include "draw_objects.h"
#include "read_mem.h"
#include "ImGuiLayer.h"
#include "driver_manager.h"
#include <unordered_map>
#include <vector>

// 类名 → 显示名称映射表，无映射的 actor 不绘制
static const std::unordered_map<std::string, std::string> kClassNameMap = {
    {"BP_TrainPlayerPawn_C", "骨骼测试"}
    // {"BP_Vehicle_C", "载具"},
    // 用户按需填充
};

// 骨骼连接定义（用于绘制骨架线条）
static const std::pair<int, int> kBoneConnections[] = {
    // 头部到躯干
    {BONE_HEAD, BONE_NECK},
    {BONE_NECK, BONE_CHEST},
    {BONE_CHEST, BONE_PELVIS},

    // 左臂
    {BONE_CHEST, BONE_L_SHOULDER},
    {BONE_L_SHOULDER, BONE_L_ELBOW},
    {BONE_L_ELBOW, BONE_L_HAND},

    // 右臂
    {BONE_CHEST, BONE_R_SHOULDER},
    {BONE_R_SHOULDER, BONE_R_ELBOW},
    {BONE_R_ELBOW, BONE_R_HAND},

    // 左腿
    {BONE_PELVIS, BONE_L_KNEE},
    {BONE_L_KNEE, BONE_L_FOOT},

    // 右腿
    {BONE_PELVIS, BONE_R_KNEE},
    {BONE_R_KNEE, BONE_R_FOOT},
};

bool gShowObjects = true;
bool gShowAllClassNames = false;
bool gUseBatchBoneRead = true;  // 默认使用批量读取（优化模式）
int gBoneCount = 0;

static std::shared_ptr<std::vector<CachedActor>> lastActorList;
static std::vector<FTransform> cachedRootTranForms;

void DrawObjects() {
    if (!gShowObjects) return;
    if (driver_stat.load(std::memory_order_relaxed) <= 0) return;

    auto actors = GetCachedActors();
    if (!actors || actors->empty()) return;


    // VP 矩阵每帧即时读取（1 次 ioctl）
    FMatrix VPMat;
    if (address.Matrix != 0) {
        Paradise_hook->read(address.Matrix, &VPMat, sizeof(FMatrix));
    } else {
        return;
    }

    int N = (int)actors->size();

    // Round-robin actor 位置读取：每帧更新一半 actor（降低 ioctl 调用）
    static int roundRobinOffset = 0;

    if (actors != lastActorList) {
        lastActorList = actors;
        cachedRootTranForms.resize(N);
        // 首次运行时初始化为零
        for (int i = 0; i < N; i++) {
            cachedRootTranForms[i] = {};
        }
    }

    // 每帧更新一半 actor（round-robin 隔帧交替）
    for (int i = roundRobinOffset; i < N; i += 2) {
        const auto& ca = (*actors)[i];
        if (ca.rootCompAddr == 0) {
            cachedRootTranForms[i] = {};
        } else {
            cachedRootTranForms[i] = Paradise_hook->read<FTransform>(
                ca.rootCompAddr + offset.ComponentToWorld);
        }
    }

    // 每帧切换偏移量（0 -> 1 -> 0 -> 1...）
    roundRobinOffset = 1 - roundRobinOffset;

    // 绘制有映射的 actor
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    Vec2 screenPos;

    for (int i = 0; i < N; i++) {
        const auto& ca = (*actors)[i];

        // 跳过本地玩家 actor
        if (address.LocalPlayerActor != 0 && ca.actorAddr == address.LocalPlayerActor) {
            continue;
        }

        auto mapIt = kClassNameMap.find(ca.className);
        const char* label = nullptr;
        if (mapIt != kClassNameMap.end()) {
            label = mapIt->second.c_str();
        } else if (gShowAllClassNames && !ca.className.empty()) {
            label = ca.className.c_str();
        } else {
            continue;
        }
        char logcount[128];
        sprintf(logcount, "%s", label);

        // 检查是否有骨骼映射（读取线程已为该 actor 构建 boneMap）
        bool hasBoneMap = false;
        for (int j = 0; j < BONE_COUNT; j++) {
            if (ca.boneMap[j] >= 0) {
                hasBoneMap = true;
                break;
            }
        }

        if (hasBoneMap) {
            uint64_t SkeletalMeshComponent = Paradise_hook->read<uint64_t>(ca.actorAddr + offset.SkeletalMeshComponent);
            if (SkeletalMeshComponent != 0) {
                // 限制 BoneCount 避免读取过大数组（防御性编程）
                constexpr int MAX_BONE_COUNT = 150;

                // 使用 ComponentSpaceTransforms（单缓冲）
                uint64_t ComponentSpaceTransforms = SkeletalMeshComponent + offset.ComponentSpaceTransforms;
                int BoneCount = Paradise_hook->read<int>(ComponentSpaceTransforms + 0x8);
                uint64_t BoneDataPtr = Paradise_hook->read<uint64_t>(ComponentSpaceTransforms);

                if (BoneCount > 0 && BoneCount <= MAX_BONE_COUNT && BoneDataPtr != 0) {
                    FTransform meshTransform = Paradise_hook->read<FTransform>(
                        SkeletalMeshComponent + offset.ComponentToWorld);

                    // 提取关键骨骼（简化：只使用 translation，无验证）
                    Vec3 boneTranslations[BONE_COUNT];
                    int validCount = 0;

                    if (gUseBatchBoneRead) {
                        // 批量读取（单缓冲）
                        FTransform allBones[MAX_BONE_COUNT];
                        if (!Paradise_hook->read(BoneDataPtr, allBones, BoneCount * sizeof(FTransform))) {
                            continue;
                        }

                        for (int boneID = 0; boneID < BONE_COUNT; boneID++) {
                            int cstIndex = ca.boneMap[boneID];
                            if (cstIndex < 0 || cstIndex >= BoneCount) continue;

                            boneTranslations[boneID] = allBones[cstIndex].Translation;
                            validCount++;
                        }
                    } else {
                        // 逐个读取（单缓冲）
                        for (int boneID = 0; boneID < BONE_COUNT; boneID++) {
                            int cstIndex = ca.boneMap[boneID];
                            if (cstIndex < 0 || cstIndex >= BoneCount) continue;

                            FTransform bone;
                            if (Paradise_hook->read(BoneDataPtr + cstIndex * sizeof(FTransform), &bone, sizeof(FTransform))) {
                                boneTranslations[boneID] = bone.Translation;
                                validCount++;
                            }
                        }
                    }

                    // 只有足够多的骨骼有效时才绘制
                    if (validCount >= 10) {
                        // 计算所有关键骨骼的屏幕坐标（简化：只使用 translation）
                        Vec2 boneScreenPos[BONE_COUNT];
                        bool boneOnScreen[BONE_COUNT] = {false};

                        for (int boneID = 0; boneID < BONE_COUNT; boneID++) {
                            if (ca.boneMap[boneID] < 0) continue;

                            // 骨骼世界坐标 = mesh transform + bone translation
                            Vec3 boneLocal = boneTranslations[boneID];
                            FMatrix meshMatrix = TransformToMatrix(meshTransform);
                            Vec3 worldPos = {
                                meshMatrix.M[0][0] * boneLocal.X + meshMatrix.M[1][0] * boneLocal.Y + meshMatrix.M[2][0] * boneLocal.Z + meshMatrix.M[3][0],
                                meshMatrix.M[0][1] * boneLocal.X + meshMatrix.M[1][1] * boneLocal.Y + meshMatrix.M[2][1] * boneLocal.Z + meshMatrix.M[3][1],
                                meshMatrix.M[0][2] * boneLocal.X + meshMatrix.M[1][2] * boneLocal.Y + meshMatrix.M[2][2] * boneLocal.Z + meshMatrix.M[3][2]
                            };

                            if (WorldToScreen(worldPos, VPMat, io.DisplaySize.x, io.DisplaySize.y, boneScreenPos[boneID])) {
                                boneOnScreen[boneID] = true;
                            }
                        }

                        // 绘制骨骼连接线
                        for (const auto& conn : kBoneConnections) {
                            int bone1 = conn.first;
                            int bone2 = conn.second;
                            if (boneOnScreen[bone1] && boneOnScreen[bone2]) {
                                ImVec2 p1(boneScreenPos[bone1].x, boneScreenPos[bone1].y);
                                ImVec2 p2(boneScreenPos[bone2].x, boneScreenPos[bone2].y);
                                draw_list->AddLine(p1, p2, IM_COL32(0, 255, 0, 255), 2.0f);
                            }
                        }

                    }
                }
            }
        }
        
        const Vec3& worldPos = cachedRootTranForms[i].Translation;
        if (worldPos.X == 0 && worldPos.Y == 0 && worldPos.Z == 0) continue;
        if (WorldToScreen(worldPos, VPMat, io.DisplaySize.x, io.DisplaySize.y, screenPos)) {
            ImVec2 pos(screenPos.x, screenPos.y);
            draw_list->AddCircleFilled(pos, 3.0f, IM_COL32(255, 0, 0, 255));
            ImVec2 textSize = ImGui::CalcTextSize(label);
            draw_list->AddText(
                ImVec2(pos.x - textSize.x * 0.5f, pos.y - textSize.y - 2.0f),
                IM_COL32(255, 255, 255, 255), label);
        }
    }
}
