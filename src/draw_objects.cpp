#include "draw_objects.h"
#include "read_mem.h"
#include "ImGuiLayer.h"
#include "driver.h"
#include <unordered_map>

extern Paradise_hook_driver* Paradise_hook;

// 类名 → 显示名称映射表，无映射的 actor 不绘制
static const std::unordered_map<std::string, std::string> kClassNameMap = {
    {"BP_TrainPlayerPawn_C", "骨骼测试"}
    // {"BP_Vehicle_C", "载具"},
    // 用户按需填充
};

bool gShowObjects = true;
bool gShowAllClassNames = false;
int gBoneCount = 0;

static std::shared_ptr<std::vector<CachedActor>> lastActorList;
static std::vector<FTransform> cachedRootTranForms;
static int roundRobinPhase = 0;

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

    // actor 列表变化时（每 500ms 扫描更新），一次性读取所有位置
    if (actors != lastActorList) {
        lastActorList = actors;
        cachedRootTranForms.resize(N);
        for (int i = 0; i < N; i++) {
            const auto& ca = (*actors)[i];
            if (ca.rootCompAddr == 0) {
                cachedRootTranForms[i] = {};
            } else {
                cachedRootTranForms[i] = Paradise_hook->read<FTransform>(
                    ca.rootCompAddr + offset.ComponentToWorld);
            }
        }
        roundRobinPhase = 0;
    } else {
        // 隔帧交替读取：偶数帧更新偶数索引，奇数帧更新奇数索引
        // ioctl 数量减半，每个 actor 仍保持 targetFPS/2 的更新率
        for (int i = roundRobinPhase; i < N; i += 2) {
            const auto& ca = (*actors)[i];
            if (ca.rootCompAddr == 0) continue;
            cachedRootTranForms[i] = Paradise_hook->read<FTransform>(
                    ca.rootCompAddr + offset.ComponentToWorld);
        }
        roundRobinPhase = 1 - roundRobinPhase;
    }

    // 绘制有映射的 actor
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    Vec2 screenPos;
    for (int i = 0; i < N; i++) {
        const auto& ca = (*actors)[i];
        auto mapIt = kClassNameMap.find(ca.className);
        const char* label = nullptr;
        if (mapIt != kClassNameMap.end()) {
            label = mapIt->second.c_str();
        } else if (gShowAllClassNames && !ca.className.empty()) {
            label = ca.className.c_str();
        } else {
            continue;
        }
        char logcount[4];
        if (!strncmp(ca.className.c_str(),"BP_TrainPlayerPawn_C",5))
        {
            uint64_t SkeletalMeshComponent = Paradise_hook->read<uint64_t>(ca.actorAddr + offset.SkeletalMeshComponent);
            uint64_t ComponentSpaceTransforms = SkeletalMeshComponent + offset.ComponentSpaceTransforms;
            int BoneCount = Paradise_hook->read<int>(ComponentSpaceTransforms + 0x8);
            gBoneCount = BoneCount;
            sprintf(logcount, "%d", BoneCount);
            uint64_t BoneDataPtr = Paradise_hook->read<uint64_t>(ComponentSpaceTransforms);
            FTransform Bones[BoneCount];
            Paradise_hook->read(BoneDataPtr, Bones, BoneCount * sizeof(FTransform));
            FTransform meshTransform = Paradise_hook->read<FTransform>(
                SkeletalMeshComponent + offset.ComponentToWorld);
            Vec3 BoneWorldPos;
            for (int j = 0; j < BoneCount; j++)
            {
                BoneWorldPos = TransformPosition(meshTransform, Bones[j].Translation);
                if (WorldToScreen(BoneWorldPos, VPMat, io.DisplaySize.x, io.DisplaySize.y, screenPos)){
                    ImVec2 pos(screenPos.x, screenPos.y);
                    draw_list->AddCircleFilled(pos, 3.0f, IM_COL32(255, 0, 0, 255));
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
                ImVec2(pos.x - textSize.x * 0.5f, pos.y  + 10.0f),
                IM_COL32(255, 255, 255, 255), logcount);
            draw_list->AddText(
                ImVec2(pos.x - textSize.x * 0.5f, pos.y - textSize.y - 2.0f),
                IM_COL32(255, 255, 255, 255), label);
        }
    }
}
