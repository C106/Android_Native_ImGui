#include "draw_objects.h"
#include "read_mem.h"
#include "ImGuiLayer.h"
#include "driver_manager.h"
#include <unordered_map>
#include <vector>

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
float gMaxSkeletonDistance = 200.0f;  // 默认 200 米，超过不绘制骨骼

// 分类显示开关（默认全部显示）
bool gShowPlayers = true;
bool gShowVehicles = true;
bool gShowOthers = true;

// TeamID → 颜色映射（使用固定色板，按 teamID % 数量 循环）
static ImU32 GetTeamColor(int teamID) {
    static const ImU32 kTeamColors[] = {
        IM_COL32(0,   200, 255, 255),  // 青色
        IM_COL32(255, 80,  80,  255),  // 红色
        IM_COL32(80,  255, 80,  255),  // 绿色
        IM_COL32(255, 200, 0,   255),  // 黄色
        IM_COL32(200, 80,  255, 255),  // 紫色
        IM_COL32(255, 140, 0,   255),  // 橙色
        IM_COL32(0,   200, 150, 255),  // 蓝绿
        IM_COL32(255, 100, 200, 255),  // 粉色
    };
    constexpr int kNumColors = sizeof(kTeamColors) / sizeof(kTeamColors[0]);
    if (teamID < 0) return IM_COL32(200, 200, 200, 255);  // 未知队伍：灰色
    return kTeamColors[teamID % kNumColors];
}

// 前向声明
static void DrawObjectsWithDataInternal(
    const std::shared_ptr<std::vector<CachedActor>>& actors,
    const FMatrix& VPMat,
    const Vec3& localPlayerPos);

// 读取游戏数据（在 fence wait 前调用，减少延迟）
GameFrameData ReadGameData() {
    GameFrameData data;
    data.valid = false;

    if (driver_stat.load(std::memory_order_relaxed) <= 0) return data;
    if (address.Matrix == 0) return data;

    // 读取 VP 矩阵（1 ioctl）
    if (!Paradise_hook->read(address.Matrix, &data.VPMat, sizeof(FMatrix))) {
        return data;
    }

    // 读取本地玩家位置（2 ioctl）
    data.localPlayerPos = Vec3::Zero();
    if (address.LocalPlayerActor != 0) {
        uint64_t localRootComponent = Paradise_hook->read<uint64_t>(
            address.LocalPlayerActor + offset.RootComponent);
        if (localRootComponent != 0) {
            FTransform localTransform = Paradise_hook->read<FTransform>(
                localRootComponent + offset.ComponentToWorld);
            data.localPlayerPos = localTransform.Translation;
        }
    }

    data.valid = true;
    return data;
}

// 使用预读数据绘制（在 fence wait 后调用）
void DrawObjectsWithData(const GameFrameData& data) {
    if (!gShowObjects) return;
    if (!data.valid) return;

    // 获取分类后的 actor 列表
    auto classified = GetClassifiedActors();
    if (!classified) return;

    // 按类型分别绘制（根据开关控制）
    if (gShowPlayers && !classified->players.empty()) {
        auto players = std::make_shared<std::vector<CachedActor>>(classified->players);
        DrawObjectsWithDataInternal(players, data.VPMat, data.localPlayerPos);
    }
    if (gShowVehicles && !classified->vehicles.empty()) {
        auto vehicles = std::make_shared<std::vector<CachedActor>>(classified->vehicles);
        DrawObjectsWithDataInternal(vehicles, data.VPMat, data.localPlayerPos);
    }
    if (gShowOthers && !classified->others.empty()) {
        auto others = std::make_shared<std::vector<CachedActor>>(classified->others);
        DrawObjectsWithDataInternal(others, data.VPMat, data.localPlayerPos);
    }
}

// 旧接口（保留兼容性，立即读取并绘制）
void DrawObjects() {
    GameFrameData data = ReadGameData();
    DrawObjectsWithData(data);
}

// 实际的绘制逻辑（提取为独立函数）
static void DrawObjectsWithDataInternal(
    const std::shared_ptr<std::vector<CachedActor>>& actors,
    const FMatrix& VPMat,
    const Vec3& localPlayerPos)
{

    // 绘制有映射的 actor
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    Vec2 screenPos;

    for (const auto& ca : *actors) {
        // 跳过本地玩家 actor
        if (address.LocalPlayerActor != 0 && ca.actorAddr == address.LocalPlayerActor) {
            continue;
        }

        // 使用预分类的 displayName（读取线程已完成类名比对）
        const char* label = nullptr;
        if (!ca.displayName.empty()) {
            label = ca.displayName.c_str();
        } else if (gShowAllClassNames && !ca.className.empty()) {
            label = ca.className.c_str();
        } else {
            continue;  // 无显示名称，跳过
        }

        // 检查是否有骨骼映射（使用 boneMapBuilt 标志）
        bool hasBoneMap = ca.boneMapBuilt;

        // 实时读取 actor 位置（每帧读取，确保与游戏引擎同步）
        Vec3 actorPos = Vec3::Zero();
        float distance = 0.0f;
        if (ca.rootCompAddr != 0) {
            FTransform actorTransform = Paradise_hook->read<FTransform>(ca.rootCompAddr + offset.ComponentToWorld);
            actorPos = actorTransform.Translation;
            distance = Vec3::Distance(localPlayerPos, actorPos) / 100.0f;  // 转换为米（UE4 单位是厘米）
        } else {
            continue;  // 无 RootComponent，跳过
        }

        // 距离剔除：超过 500 米完全跳过（节省 CPU）
        constexpr float MAX_RENDER_DISTANCE = 500.0f;
        if (distance > MAX_RENDER_DISTANCE) {
            continue;
        }

        // 距离衰减参数（可调整）
        constexpr float MIN_DISTANCE = 10.0f;   // 10米内全亮、全尺寸
        constexpr float MAX_DISTANCE = 200.0f;  // 300米外最小透明度、最小尺寸

        // 计算距离因子 [0, 1]：0=近，1=远
        float distanceFactor = 0.0f;
        if (distance > MIN_DISTANCE) {
            distanceFactor = (distance - MIN_DISTANCE) / (MAX_DISTANCE - MIN_DISTANCE);
            distanceFactor = std::min(1.0f, std::max(0.0f, distanceFactor));
        }

        // 文本缩放：1.0 → 0.5（近→远）
        float textScale = 1.0f - distanceFactor * 0.6f;

        // 透明度：255 → 100（近→远，保持一定可见度）
        int alpha = (int)(255 - distanceFactor * 200);

        // 骨骼透明度：根据距离和设定阈值计算
        int boneAlpha = alpha;
        if (distance > gMaxSkeletonDistance) {
            // 超过设定距离，骨骼变为几乎透明（alpha=20）
            boneAlpha = 20;
        }

        // 是否绘制骨骼：有骨骼映射即绘制（不再受距离限制，通过透明度控制）
        bool drawSkeleton = hasBoneMap;

        // 用于存储标签位置
        Vec3 topLabelWorldPos = Vec3::Zero();    // 头部位置（绘制名称）
        Vec3 bottomLabelWorldPos = Vec3::Zero(); // 根骨骼位置（绘制距离）
        bool hasTopLabel = false;
        bool hasBottomLabel = false;

        if (drawSkeleton) {
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
                        // 优化：只读取需要的骨骼，而不是全部
                        // 找出需要读取的最大索引
                        int maxIndex = -1;
                        for (int boneID = 0; boneID < BONE_COUNT; boneID++) {
                            int cstIndex = ca.boneMap[boneID];
                            if (cstIndex >= 0 && cstIndex < BoneCount) {
                                maxIndex = std::max(maxIndex, cstIndex);
                            }
                        }

                        if (maxIndex >= 0 && maxIndex < MAX_BONE_COUNT) {
                            // 只读取到最大索引的骨骼
                            int readCount = maxIndex + 1;
                            FTransform allBones[MAX_BONE_COUNT];
                            if (!Paradise_hook->read(BoneDataPtr, allBones, readCount * sizeof(FTransform))) {
                                continue;
                            }

                            for (int boneID = 0; boneID < BONE_COUNT; boneID++) {
                                int cstIndex = ca.boneMap[boneID];
                                if (cstIndex < 0 || cstIndex >= readCount) continue;

                                boneTranslations[boneID] = allBones[cstIndex].Translation;
                                if (boneID == BONE_HEAD){
                                    boneTranslations[boneID].Z += 7;
                                }

                                validCount++;
                            }
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
                        // 计算所有关键骨骼的屏幕坐标 + 标签位置（一次遍历）
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

                            // 保存标签位置
                            if (boneID == BONE_HEAD) {
                                topLabelWorldPos = worldPos;
                                topLabelWorldPos.Z += 7.0f;  // 头部上方 7 单位
                                hasTopLabel = true;
                            } else if (boneID == BONE_ROOT) {  // 根骨骼（用于 bottom label）
                                bottomLabelWorldPos = worldPos;
                                bottomLabelWorldPos.Z -= 10.0f;  // 根骨骼下方 10 单位
                                hasBottomLabel = true;
                            }

                            if (WorldToScreen(worldPos, VPMat, io.DisplaySize.x, io.DisplaySize.y, boneScreenPos[boneID])) {
                                boneOnScreen[boneID] = true;
                            }
                        }

                        // 绘制骨骼连接线（使用距离衰减的透明度，超过设定距离几乎透明）
                        for (const auto& conn : kBoneConnections) {
                            int bone1 = conn.first;
                            int bone2 = conn.second;
                            if (boneOnScreen[bone1] && boneOnScreen[bone2]) {
                                ImVec2 p1(boneScreenPos[bone1].x, boneScreenPos[bone1].y);
                                ImVec2 p2(boneScreenPos[bone2].x, boneScreenPos[bone2].y);
                                draw_list->AddLine(p1, p2, IM_COL32(0, 255, 0, boneAlpha), 2.0f);
                            }
                        }
                    }
                }
            }
        }
/*
        // Fallback: 如果没有骨骼标签位置，使用 actor 位置
        if (!hasTopLabel || !hasBottomLabel) {
            if (actorPos != Vec3::Zero()) {
                if (!hasTopLabel) {
                    topLabelWorldPos = actorPos;
                    topLabelWorldPos.Z += 100.0f;  // 假设角色高度约 100cm
                    hasTopLabel = true;
                }
                if (!hasBottomLabel) {
                    bottomLabelWorldPos = actorPos;
                    bottomLabelWorldPos.Z -= 20.0f;  // 脚下 20cm
                    hasBottomLabel = true;
                }
            } else if (ca.rootCompAddr != 0) {
                // 重新尝试读取 actor 位置
                FTransform actorTransform = Paradise_hook->read<FTransform>(ca.rootCompAddr + offset.ComponentToWorld);
                if (actorTransform.Translation != Vec3::Zero()) {
                    if (!hasTopLabel) {
                        topLabelWorldPos = actorTransform.Translation;
                        topLabelWorldPos.Z += 100.0f;
                        hasTopLabel = true;
                    }
                    if (!hasBottomLabel) {
                        bottomLabelWorldPos = actorTransform.Translation;
                        bottomLabelWorldPos.Z -= 40.0f;
                        hasBottomLabel = true;
                    }
                }
            }
        }
*/
        // 绘制 Top Label（名称）
        if (hasTopLabel && !ca.playerName.empty()) {
            Vec2 topLabelScreenPos;
            if (WorldToScreen(topLabelWorldPos, VPMat, io.DisplaySize.x, io.DisplaySize.y, topLabelScreenPos)) {
                ImVec2 labelPos(topLabelScreenPos.x, topLabelScreenPos.y);
                ImU32 teamColor = GetTeamColor(ca.teamID);
                ImU32 teamColorWithAlpha = (teamColor & 0x00FFFFFF) | (alpha << 24);

                // 构建显示文本：[队伍ID] 玩家名
                char displayName[256];
                if (ca.teamID >= 0) {
                    snprintf(displayName, sizeof(displayName), "[%d] %s", ca.teamID, ca.playerName.c_str());
                } else {
                    snprintf(displayName, sizeof(displayName), "%s", ca.playerName.c_str());
                }

                // 使用字体指针直接缩放（更明显的效果）
                ImFont* font = io.FontDefault;
                float originalFontSize = font->LegacySize;  // 使用 LegacySize 获取字体大小
                float scaledFontSize = originalFontSize * textScale;

                // 计算缩放后的文本大小
                ImVec2 textSize = font->CalcTextSizeA(scaledFontSize, FLT_MAX, 0.0f, displayName);
                ImVec2 textPos(labelPos.x - textSize.x * 0.5f, labelPos.y - textSize.y - 5.0f);


                // 绘制彩色文本（使用缩放后的字体大小）
                draw_list->AddText(font, scaledFontSize, textPos, teamColor, displayName);
            }
        }

        // 绘制 Bottom Label（距离）
        if (hasBottomLabel && distance > 0.0f) {
            Vec2 bottomLabelScreenPos;
            if (WorldToScreen(bottomLabelWorldPos, VPMat, io.DisplaySize.x, io.DisplaySize.y, bottomLabelScreenPos)) {
                ImVec2 labelPos(bottomLabelScreenPos.x, bottomLabelScreenPos.y);

                // 构建距离文本
                char distanceText[64];
                snprintf(distanceText, sizeof(distanceText), "%.0fm", distance);

                // 使用字体指针直接缩放（距离文本稍小）
                ImFont* font = io.FontDefault;
                float originalFontSize = font->LegacySize;  // 使用 LegacySize 获取字体大小
                float scaledFontSize = originalFontSize * textScale * 0.8f;  // 距离文本稍小

                // 计算缩放后的文本大小
                ImVec2 textSize = font->CalcTextSizeA(scaledFontSize, FLT_MAX, 0.0f, distanceText);
                ImVec2 textPos(labelPos.x - textSize.x * 0.5f, labelPos.y + 5.0f);

                // 绘制白色文本（使用缩放后的字体大小）
                draw_list->AddText(font, scaledFontSize, textPos, IM_COL32(255, 255, 255, 255), distanceText);
            }
        }

    }
}
