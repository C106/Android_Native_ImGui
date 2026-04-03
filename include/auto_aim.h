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
    bool visibilityCheck = false;    // 仅锁定可见目标
    float hysteresisThreshold = 50.0f; // 目标切换阈值（像素）
    bool drawDebug = false;          // 绘制调试信息
    float holtAlpha = 0.7f;          // Holt 位置平滑系数 (越高越灵敏)
    float holtBeta = 0.4f;           // Holt 趋势平滑系数 (越高越快适应速度变化)
    float recoilBaseOffsetScale = 0.35f;      // 基础抬升 → 镜心偏移
    float recoilKickOffsetScale = 120.0f;     // 枪口上跳基准幅度
    float recoilRecoveryReturnScale = 0.30f;  // 后座回正速度倍率
    float maxRecoilOffsetFraction = 0.35f;    // 最大镜心偏移占屏高比例
};

// Holt 双指数平滑状态
struct HoltState {
    Vec2 level;           // 平滑位置 (L_t)
    Vec2 trend;           // 平滑趋势/速度 (T_t)
    bool initialized = false;
};

struct RecoilDebugInfo {
    uint64_t entityComp = 0;
    bool valid = false;

    float verticalRecoilMin = 0.0f;
    float verticalRecoilMax = 0.0f;
    float verticalRecoilVariation = 0.0f;
    float verticalRecoveryModifier = 0.0f;
    float verticalRecoveryClamp = 0.0f;
    float verticalRecoveryMax = 0.0f;

    float leftMax = 0.0f;
    float rightMax = 0.0f;
    float horizontalTendency = 0.0f;

    int bulletPerSwitch = 0;
    float timePerSwitch = 0.0f;
    bool switchOnTime = false;

    float recoilSpeedVertical = 0.0f;
    float recoilSpeedHorizontal = 0.0f;
    float recoverySpeedVertical = 0.0f;
    float recoilValueClimb = 0.0f;
    float recoilValueFail = 0.0f;

    float recoilModifierStand = 0.0f;
    float recoilModifierCrouch = 0.0f;
    float recoilModifierProne = 0.0f;
    float recoilHorizontalMinScalar = 0.0f;
    float burstEmptyDelay = 0.0f;

    bool shootSightReturn = false;
    float shootSightReturnSpeed = 0.0f;

    float recoilCurveStart = 0.0f;
    float recoilCurveEnd = 0.0f;
    float recoilCurveOneBurstStart = 0.0f;
    float recoilCurveOneBurstEnd = 0.0f;
    float recoilCurveMultiBurstStart = 0.0f;
    float recoilCurveMultiBurstEnd = 0.0f;
    float recoilCurveSamplingInterval = 0.0f;

    uint64_t recoilCurve = 0;
    uint64_t recoilCurveOneBurst = 0;
    uint64_t recoilCurveMultiBurst = 0;
    int recoilCurveArrayNum = 0;
    int recoilCurveOneBurstArrayNum = 0;
    int recoilCurveMultiBurstArrayNum = 0;

    float accessoriesVRecoilFactor = 0.0f;
    float accessoriesVRecoilFactorModifier = 0.0f;
    float verticalRecoilFactorModifier = 0.0f;
    float accessoriesHRecoilFactor = 0.0f;
    float accessoriesHRecoilFactorModifier = 0.0f;
    float horizontalRecoilFactorModifier = 0.0f;
    float accessoriesAllRecoilFactorModifier = 0.0f;
    float accessoriesRecoveryFactor = 0.0f;

    float realtimeVerticalRecoilSpeed = 0.0f;
    float realtimeHorizontalRecoilSpeed = 0.0f;
    float realtimeRecoverySpeed = 0.0f;
};

// 目标跟踪状态
struct TargetState {
    uint64_t actorAddr = 0;
    int boneID = -1;
    Vec2 lastScreenPos;
    Vec2 lastError;
    Vec2 lastOutput;
    Vec2 debugRawScreenCenter;
    Vec2 debugEffectiveScreenCenter;
    Vec2 debugRecoilCenterOffset;
    float recoilBaseLiftOffset = 0.0f;
    float recoilKickOffset = 0.0f;
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
    RecoilDebugInfo debugRecoil;
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

    bool SelectTarget(const Vec2& screenCenter, uint64_t& outActorAddr, int& outBoneID, Vec2& outScreenPos);
    Vec2 ComputePDOutput(const Vec2& targetPos, const Vec2& screenCenter,
                         const Vec2& feedforward, float deltaTime, float fovRatio);
    float DistanceToScreenCenter(const Vec2& pos, const Vec2& center) const;
    bool IsInFOV(const Vec2& pos, const Vec2& center) const;
    bool ShouldSwitchTarget(const Vec2& currentPos, const Vec2& newPos, const Vec2& center) const;
    void DrawDebugVisuals(const Vec2& targetPos, const Vec2& screenCenter);
    void DrawRecoilSpeedDebugWindow();
    bool IsLocalPlayerFiring();
};

extern AutoAimController* gAutoAim;

void InitAutoAim();
void ShutdownAutoAim();
