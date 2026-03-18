#include "draw_objects.h"
#include "read_mem.h"
#include "ImGuiLayer.h"
#include "driver_manager.h"
#include "game_fps_monitor.h"
#include <unordered_map>
#include <vector>
#include <algorithm>

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
bool gEnableBoneSmoothing = false;  // 默认关闭，避免骨骼视觉上慢半拍
int gBoneCount = 0;
float gMaxSkeletonDistance = 200.0f;  // 默认 200 米，超过不绘制骨骼

// 分类显示开关（默认全部显示）
bool gShowPlayers = true;
bool gShowVehicles = true;
bool gShowOthers = true;

// 绘制模块开关（默认全部启用）
bool gDrawSkeleton = true;   // 绘制骨骼线条
bool gDrawDistance = true;   // 绘制距离信息
bool gDrawName = true;       // 绘制名称标签
bool gDrawBox = false;       // 绘制包围盒（预留，默认关闭）

// 骨骼屏幕坐标缓存（渲染线程写入，auto-aim 读取——同一线程，无需 mutex）
static std::unordered_map<uint64_t, BoneScreenData> gBoneScreenCache;

// ── 骨骼插值缓存 ──
// 缓存上一帧每个 actor 的骨骼世界坐标，用于 lerp 平滑
struct BoneWorldCache {
    Vec3 positions[BONE_COUNT];
    bool valid[BONE_COUNT];
    uint64_t lastFrameCounter;  // 上次更新时的引擎帧号
    bool initialized;
};
static std::unordered_map<uint64_t, BoneWorldCache> gBoneWorldCache;

// 上一帧引擎帧号（用于检测引擎是否推进了新帧）
static uint64_t gLastEngineFrame = 0;

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
    const std::vector<CachedActor>& actors,
    const FMatrix& VPMat,
    const Vec3& localPlayerPos,
    uint64_t engineFrame,
    float renderDeltaTime);

// 读取游戏数据（在 fence wait 前调用，减少延迟）
GameFrameData ReadGameData() {
    GameFrameData data;
    data.valid = false;
    data.gameDeltaTime = 0.0f;
    data.frameCounter = 0;

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

    // 读取 FApp::DeltaTime（GOT 表，两次解引用）
    if (address.libUE4 != 0 && offset.FAppDeltaTimeGOT != 0) {
        uint64_t gotEntry = Paradise_hook->read<uint64_t>(address.libUE4 + offset.FAppDeltaTimeGOT);
        if (gotEntry != 0) {
            double deltaTime = Paradise_hook->read<double>(gotEntry);
            if (deltaTime > 0.0 && deltaTime < 1.0) {
                PushGameDeltaTime(deltaTime);
                data.gameDeltaTime = (float)deltaTime;
            }
        }
    }

    // 读取 GFrameCounter（与 DeltaTime 同步）
    if (address.libUE4 != 0 && offset.GFrameCounterGOT != 0) {
        uint64_t gotEntry = Paradise_hook->read<uint64_t>(address.libUE4 + offset.GFrameCounterGOT);
        if (gotEntry != 0) {
            data.frameCounter = Paradise_hook->read<uint64_t>(gotEntry);
        }
    }

    data.valid = true;
    return data;
}

// 仅读取 GFrameCounter（轻量，用于帧同步轮询）
uint64_t ReadFrameCounter() {
    if (driver_stat.load(std::memory_order_relaxed) <= 0) return 0;
    if (address.libUE4 == 0 || offset.GFrameCounterGOT == 0) return 0;
    uint64_t gotEntry = Paradise_hook->read<uint64_t>(address.libUE4 + offset.GFrameCounterGOT);
    if (gotEntry == 0) return 0;
    return Paradise_hook->read<uint64_t>(gotEntry);
}

// 使用预读数据绘制（在 fence wait 后调用）
void DrawObjectsWithData(const GameFrameData& data, float renderDeltaTime) {
    if (!gShowObjects) return;
    if (!data.valid) return;

    // 每帧重建屏幕骨骼缓存，避免 auto-aim 读取到上一帧残留坐标。
    gBoneScreenCache.clear();

    // 定期清理缓存（每 2 秒，假设 60 FPS）
    static int cleanupCounter = 0;
    if (++cleanupCounter >= 120) {
        cleanupCounter = 0;
        // 清理过期的插值缓存（超过 120 帧未更新的 actor）
        for (auto it = gBoneWorldCache.begin(); it != gBoneWorldCache.end(); ) {
            if (data.frameCounter > it->second.lastFrameCounter + 120) {
                it = gBoneWorldCache.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 更新引擎帧号（用于插值缓存过期判断）
    gLastEngineFrame = data.frameCounter;

    // VP 矩阵直接使用帧数据中读取的（每帧一次，不再高频轮询）
    const FMatrix& vpToUse = data.VPMat;

    if (gShowAllClassNames) {
        auto allActors = GetCachedActors();
        if (!allActors || allActors->empty()) return;
        DrawObjectsWithDataInternal(*allActors, vpToUse, data.localPlayerPos,
                                    data.frameCounter, renderDeltaTime);
        return;
    }

    // 获取分类后的 actor 列表
    auto classified = GetClassifiedActors();
    if (!classified) return;

    // 按类型分别绘制（根据开关控制，直接传引用避免拷贝）
    if (gShowPlayers && !classified->players.empty()) {
        DrawObjectsWithDataInternal(classified->players, vpToUse, data.localPlayerPos,
                                    data.frameCounter, renderDeltaTime);
    }
    if (gShowVehicles && !classified->vehicles.empty()) {
        DrawObjectsWithDataInternal(classified->vehicles, vpToUse, data.localPlayerPos,
                                    data.frameCounter, renderDeltaTime);
    }
    if (gShowOthers && !classified->others.empty()) {
        DrawObjectsWithDataInternal(classified->others, vpToUse, data.localPlayerPos,
                                    data.frameCounter, renderDeltaTime);
    }
}

// 旧接口（保留兼容性，立即读取并绘制）
void DrawObjects() {
    GameFrameData data = ReadGameData();
    DrawObjectsWithData(data, 0.016f);  // 默认 ~60fps
}

// 实际的绘制逻辑（提取为独立函数）
static void DrawObjectsWithDataInternal(
    const std::vector<CachedActor>& actors,
    const FMatrix& VPMat,
    const Vec3& localPlayerPos,
    uint64_t engineFrame,
    float renderDeltaTime)
{

    // 绘制有映射的 actor
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    float screenW = io.DisplaySize.x;
    float screenH = io.DisplaySize.y;

    for (const auto& ca : actors) {
        // 跳过本地玩家 actor
        if (address.LocalPlayerActor != 0 && ca.actorAddr == address.LocalPlayerActor) {
            continue;
        }

        // 默认只显示 ClassNameMap 中已映射的对象。
        // 只有开启“显示所有类名”时，才回退显示原始 className。
        const char* label = nullptr;
        if (!ca.displayName.empty()) {
            label = ca.displayName.c_str();
        } else if (gShowAllClassNames && !ca.className.empty()) {
            label = ca.className.c_str();
        } else {
            continue;
        }

        // 检查是否有骨骼映射（使用 boneMapBuilt 标志）
        bool hasBoneMap = ca.boneMapBuilt && ca.skelMeshCompAddr != 0;

        // 对于有骨骼的 actor，从 mesh 变换中提取位置（合并读取，省 1 次 ioctl）
        Vec3 actorPos = Vec3::Zero();
        float distance = 0.0f;
        FTransform meshTransform;
        bool hasMeshTransform = false;

        if (hasBoneMap) {
            // 骨骼 actor：读取 mesh 世界变换，同时获取位置
            meshTransform = Paradise_hook->read<FTransform>(ca.skelMeshCompAddr + offset.ComponentToWorld);
            actorPos = meshTransform.Translation;
            hasMeshTransform = true;
            distance = Vec3::Distance(localPlayerPos, actorPos) / 100.0f;
        } else if (ca.rootCompAddr != 0) {
            // 无骨骼 actor：读取 RootComponent 位置（1 次 ioctl）
            FTransform actorTransform = Paradise_hook->read<FTransform>(ca.rootCompAddr + offset.ComponentToWorld);
            actorPos = actorTransform.Translation;
            distance = Vec3::Distance(localPlayerPos, actorPos) / 100.0f;
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

        // 无骨骼对象也给一个基于 actor 原点的文本锚点，确保“显示所有类名”能覆盖整个 actor 数组。
        topLabelWorldPos = actorPos;
        topLabelWorldPos.Z += 10.0f;
        hasTopLabel = true;

        if (drawSkeleton) {
            // 屏幕外剔除：先用 actor 位置做粗略检测，屏幕外跳过骨骼读取
            Vec2 cullScreenPos;
            bool onScreen = WorldToScreen(actorPos, VPMat, screenW, screenH, cullScreenPos);
            // 带边距检测（角色可能部分在屏幕外但骨骼可见）
            float margin = 200.0f;
            bool inBounds = onScreen &&
                cullScreenPos.x > -margin && cullScreenPos.x < screenW + margin &&
                cullScreenPos.y > -margin && cullScreenPos.y < screenH + margin;

            if (inBounds && hasMeshTransform && ca.cachedBoneCount > 0 && ca.boneDataPtr != 0) {
                // 使用缓存的静态指针，无需再读取 SkeletalMeshComponent/BoneCount/BoneDataPtr
                int BoneCount = ca.cachedBoneCount;
                uint64_t BoneDataPtr = ca.boneDataPtr;
                constexpr int MAX_BONE_COUNT = 150;

                if (BoneCount <= MAX_BONE_COUNT) {
                    // meshTransform 已在位置读取时获取，直接使用
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
                        // 计算所有关键骨骼的世界坐标 + 插值平滑 + 屏幕投影
                        Vec2 boneScreenPos[BONE_COUNT];
                        bool boneOnScreen[BONE_COUNT] = {false};

                        // 查找/创建插值缓存
                        auto& cache = gBoneWorldCache[ca.actorAddr];

                        float lerpFactor = 1.0f;
                        if (gEnableBoneSmoothing) {
                            // lerpFactor 越小越平滑，越大越跟手。
                            lerpFactor = std::min(1.0f, renderDeltaTime * 35.0f);
                        }

                        for (int boneID = 0; boneID < BONE_COUNT; boneID++) {
                            if (ca.boneMap[boneID] < 0) continue;

                            // 骨骼世界坐标 = mesh transform + bone translation
                            Vec3 boneLocal = boneTranslations[boneID];
                            Vec3 worldPos = {
                                meshMatrix.M[0][0] * boneLocal.X + meshMatrix.M[1][0] * boneLocal.Y + meshMatrix.M[2][0] * boneLocal.Z + meshMatrix.M[3][0],
                                meshMatrix.M[0][1] * boneLocal.X + meshMatrix.M[1][1] * boneLocal.Y + meshMatrix.M[2][1] * boneLocal.Z + meshMatrix.M[3][1],
                                meshMatrix.M[0][2] * boneLocal.X + meshMatrix.M[1][2] * boneLocal.Y + meshMatrix.M[2][2] * boneLocal.Z + meshMatrix.M[3][2]
                            };

                            // 插值平滑：与上一帧缓存位置做 lerp
                            if (gEnableBoneSmoothing && cache.initialized && cache.valid[boneID]) {
                                Vec3 prev = cache.positions[boneID];
                                // 距离过大说明瞬移/传送，不插值
                                float dist = Vec3::Distance(prev, worldPos);
                                if (dist < 500.0f) {
                                    worldPos = Vec3{
                                        prev.X + (worldPos.X - prev.X) * lerpFactor,
                                        prev.Y + (worldPos.Y - prev.Y) * lerpFactor,
                                        prev.Z + (worldPos.Z - prev.Z) * lerpFactor
                                    };
                                }
                            }

                            // 更新缓存
                            cache.positions[boneID] = worldPos;
                            cache.valid[boneID] = true;

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

                        // 标记缓存已初始化
                        cache.lastFrameCounter = engineFrame;
                        cache.initialized = true;

                        // 绘制骨骼连接线（使用距离衰减的透明度，超过设定距离几乎透明）
                        if (gDrawSkeleton) {
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

                        // 缓存骨骼屏幕坐标供 auto-aim 使用
                        BoneScreenData bsd;
                        std::copy(std::begin(boneScreenPos), std::end(boneScreenPos), std::begin(bsd.screenPos));
                        std::copy(std::begin(boneOnScreen), std::end(boneOnScreen), std::begin(bsd.onScreen));
                        bsd.distance = distance;
                        bsd.teamID = ca.teamID;
                        bsd.actorAddr = ca.actorAddr;
                        bsd.frameCounter = engineFrame;
                        bsd.valid = true;

                        gBoneScreenCache[ca.actorAddr] = bsd;
                    }
                }
            }
        }

        // 绘制 Top Label（名称）
        if (gDrawName && hasTopLabel) {
            Vec2 topLabelScreenPos;
            if (WorldToScreen(topLabelWorldPos, VPMat, io.DisplaySize.x, io.DisplaySize.y, topLabelScreenPos)) {
                ImVec2 labelPos(topLabelScreenPos.x, topLabelScreenPos.y);
                ImU32 teamColor = GetTeamColor(ca.teamID);

                char displayName[256];
                if (!ca.playerName.empty() && ca.teamID >= 0) {
                    snprintf(displayName, sizeof(displayName), "[%d] %s", ca.teamID, ca.playerName.c_str());
                } else if (!ca.playerName.empty()) {
                    snprintf(displayName, sizeof(displayName), "%s", ca.playerName.c_str());
                } else {
                    snprintf(displayName, sizeof(displayName), "%s", label);
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
        if (gDrawDistance && hasBottomLabel && distance > 0.0f) {
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

// 供 auto-aim 访问骨骼缓存（同线程调用，返回引用避免拷贝）
const std::unordered_map<uint64_t, BoneScreenData>& GetBoneScreenCache() {
    return gBoneScreenCache;
}

// 清理资源
void ShutdownDrawObjects() {
    gBoneScreenCache.clear();
    gBoneWorldCache.clear();
}
