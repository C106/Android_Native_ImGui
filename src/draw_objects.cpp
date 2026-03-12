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

void DrawObjects() {
    if (!gShowObjects) return;
    if (driver_stat.load(std::memory_order_relaxed) <= 0) return;

    auto actors = GetCachedActors();
    if (!actors || actors->empty()) return;

    // VP 矩阵即时读取（渲染前最后一刻，零延迟）
    FMatrix VPMat;
    if (address.Matrix != 0) {
        Paradise_hook->read(address.Matrix, &VPMat, sizeof(FMatrix));
    } else {
        return;
    }

    // 绘制有映射的 actor
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    Vec2 screenPos;

    for (const auto& ca : *actors) {
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

        // 检查是否有骨骼映射（使用 boneMapBuilt 标志）
        bool hasBoneMap = ca.boneMapBuilt;

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

                    // 计算 mesh 变换矩阵（提到循环外，避免重复计算）
                    FMatrix meshMatrix = TransformToMatrix(meshTransform);

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

    }
}
