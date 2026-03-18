#pragma once
#include "mem_struct.h"
#include "read_mem.h"
#include "draw_objects.h"
#include <unordered_map>

// 自瞄配置
struct AutoAimConfig {
    bool enabled = false;
    bool onlyWhenFiring = true;      // 仅在开火时启用
    int targetBone = BONE_HEAD;
    float maxDistance = 200.0f;      // 最大目标距离（米）
    float fovLimit = 30.0f;          // FOV 限制（度）
    float KpX = 0.5f;                // X 轴比例增益
    float KdX = 0.1f;                // X 轴微分增益
    float KpY = 0.5f;                // Y 轴比例增益
    float KdY = 0.1f;                // Y 轴微分增益
    float outputScaleX = 1.0f;       // X 轴输出倍率
    float outputScaleY = 1.0f;       // Y 轴输出倍率
    bool filterTeammates = true;     // 过滤队友
    float hysteresisThreshold = 50.0f; // 目标切换阈值（像素）
    bool drawDebug = false;          // 绘制调试信息
    float holtAlpha = 0.7f;          // Holt 位置平滑系数 (越高越灵敏)
    float holtBeta = 0.4f;           // Holt 趋势平滑系数 (越高越快适应速度变化)
};

// Holt 双指数平滑状态
struct HoltState {
    Vec2 level;           // 平滑位置 (L_t)
    Vec2 trend;           // 平滑趋势/速度 (T_t)
    bool initialized = false;
};

// 目标跟踪状态
struct TargetState {
    uint64_t actorAddr = 0;
    int boneID = -1;
    Vec2 lastScreenPos;
    Vec2 lastError;
    Vec2 lastOutput;
    bool valid = false;
    HoltState holt;
    // debug 显示用
    float debugBulletSpeed = 0.0f;
    float debugLeadTime = 0.0f;
    Vec3 debugTargetVel;
    Vec2 debugHoltPredicted;
    float debugCameraFOV = 0.0f;
    float debugFovScale = 1.0f;
    float debugGyroScale = 1.0f;
    float debugSensCompensate = 1.0f;
    // 灵敏度 debug
    float debugSensHipfire = 0.0f;
    float debugSensRedDot = 0.0f;
    float debugSens2x = 0.0f;
    float debugSens3x = 0.0f;
    float debugSens4x = 0.0f;
    float debugSens6x = 0.0f;
    float debugSens8x = 0.0f;
    float debugSensFireHipfire = 0.0f;
    float debugSensFireRedDot = 0.0f;
    float debugSensFire2x = 0.0f;
    float debugSensFire3x = 0.0f;
    float debugSensFire4x = 0.0f;
    float debugSensFire6x = 0.0f;
    float debugSensFire8x = 0.0f;
};

// 自瞄控制器
class AutoAimController {
public:
    AutoAimController();

    void Update(float deltaTime);
    void ResetTarget();

    AutoAimConfig& GetConfig() { return config; }
    const TargetState& GetTargetState() const { return targetState; }

private:
    AutoAimConfig config;
    TargetState targetState;

    bool SelectTarget(uint64_t& outActorAddr, int& outBoneID, Vec2& outScreenPos);
    Vec2 ComputePDOutput(const Vec2& targetPos, const Vec2& screenCenter,
                         const Vec2& feedforward, float deltaTime, float fovRatio);
    float DistanceToScreenCenter(const Vec2& pos, const Vec2& center) const;
    bool IsInFOV(const Vec2& pos, const Vec2& center) const;
    bool ShouldSwitchTarget(const Vec2& currentPos, const Vec2& newPos, const Vec2& center) const;
    void DrawDebugVisuals(const Vec2& targetPos, const Vec2& screenCenter);
    bool IsLocalPlayerFiring();
};

extern AutoAimController* gAutoAim;

void InitAutoAim();
void ShutdownAutoAim();
