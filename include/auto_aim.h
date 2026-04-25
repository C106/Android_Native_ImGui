#pragma once
#include "mem_struct.h"
#include "read_mem.h"
#include "draw_objects.h"
#include <unordered_map>
#include <atomic>
#include <thread>

enum AutoAimMode {
    AUTO_AIM_MODE_ASSIST = 0,
    AUTO_AIM_MODE_MAGNET = 1,
};

// 自瞄配置
struct AutoAimConfig {
    bool enabled = false;
    bool onlyWhenFiring = true;      // 仅在开火时启用
    int aimMode = AUTO_AIM_MODE_ASSIST;
    int targetBone = BONE_HEAD;
    float maxDistance = 200.0f;      // 最大目标距离（米）
    float fovLimit = 30.0f;          // FOV 限制（度）
    float updateRate = 120.0f;       // 自瞄更新频率（Hz）
    float KpX = 0.5f;                // X 轴比例增益
    float KdX = 0.1f;                // X 轴微分增益
    float KpY = 0.5f;                // Y 轴比例增益
    float KdY = 0.1f;                // Y 轴微分增益
    float outputScaleX = 1.0f;       // X 轴输出倍率
    float outputScaleY = 1.0f;       // Y 轴输出倍率
    bool humanizeNoise = false;      // 添加人手微抖噪声
    float noiseStrengthX = 0.18f;    // X 轴平滑随机漂移幅度
    float noiseStrengthY = 0.12f;    // Y 轴平滑随机漂移幅度
    float noiseChangeRate = 5.0f;    // 目标噪声更新频率（Hz）
    float noiseSmoothing = 7.5f;     // 噪声追踪平滑速度
    float noiseMicroJitter = 0.04f;  // 轻微高频颤动幅度
    float magnetCaptureRadius = 0.055f; // 进入吸附的半径，占短边比例
    float magnetReleaseRadius = 0.110f; // 维持吸附的半径，占短边比例
    float magnetStrength = 0.42f;       // 吸附模式输出强度
    bool filterTeammates = true;     // 过滤队友
    bool visibilityCheck = false;    // 仅锁定可见目标
    bool triggerBotEnabled = false;  // 准星压到目标时自动按下开火键
    bool triggerBotHitScanHead = false;   // Trigger Bot 命中检测：头部
    bool triggerBotHitScanNeck = false;   // Trigger Bot 命中检测：颈部
    bool triggerBotHitScanChest = true;   // Trigger Bot 命中检测：胸部
    bool triggerBotHitScanPelvis = false; // Trigger Bot 命中检测：骨盆
    float triggerBotCenterRadius = 18.0f; // 触发半径（像素）
    float triggerBotFireButtonX = 0.86f;  // 开火键 X 屏幕比例
    float triggerBotFireButtonY = 0.78f;  // 开火键 Y 屏幕比例
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

    uint64_t weapon = 0;
    uint64_t bulletTrackComp = 0;
    bool bulletTrackValid = false;
    float verticalRecoilTarget = 0.0f;
    float lastVerticalRecoilTarget = 0.0f;
    float verticalRecoilTargetDelta = 0.0f;
    float accVerticalRecoilTarget = 0.0f;
    int32_t weaponID = 0;
    uint8_t sightType = 0;
    int32_t angledSightID = 0;
    int32_t curSightTypeID = 0;
    int32_t curScopeID = 0;
};

struct FireStateDebugInfo {
    uint64_t localActor = 0;
    uint64_t currentUsingWeapon = 0;
    uint64_t curEquipWeapon = 0;
    uint8_t characterWeaponFiringRaw = 0;
    uint8_t weaponWantsToFireRaw = 0;
    uint8_t weaponWantsToFireCommonRaw = 0;
    uint8_t curShootWeaponState = 0;
};

struct PredictedAimPoint {
    Vec2 currentScreenPos;
    Vec2 predictedScreenPos;
    Vec3 targetVelocity;
    float bulletSpeed = 0.0f;
    float leadTime = 0.0f;
    bool currentOnScreen = false;
    bool predictedOnScreen = false;
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
    float noiseTime = 0.0f;
    float noiseRetargetTimer = 0.0f;
    Vec2 noiseCurrent;
    Vec2 noiseTarget;
    Vec2 debugNoise;
    bool magnetEngaged = false;
    float debugMagnetDistance = 0.0f;
    uint32_t noiseSeed = 0x13572468u;
    bool valid = false;
    float targetLostTimer = 0.0f;
    Vec3 smoothedTargetVelocity;  // 低通滤波后的目标速度，防止变向时前馈突变
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
    FireStateDebugInfo debugFireState;
};

// 自瞄控制器
class AutoAimController {
public:
    AutoAimController();
    ~AutoAimController();

    void Update(float deltaTime);
    void ResetTarget();
    void Stop();
    void SetDisplayState(float w, float h, int orientation) {
        displayWidth_.store(w, std::memory_order_relaxed);
        displayHeight_.store(h, std::memory_order_relaxed);
        displayOrientation_.store(orientation, std::memory_order_relaxed);
    }
    void DrawDebug();

    AutoAimConfig& GetConfig() { return config; }
    const TargetState& GetTargetState() const { return targetState; }

private:
    AutoAimConfig config;
    TargetState targetState;
    bool triggerTouchReady_ = false;
    bool triggerTouchDown_ = false;
    int triggerTouchMaxX_ = 0;
    int triggerTouchMaxY_ = 0;

    std::atomic<bool> threadRunning_{false};
    std::thread updateThread_;
    void UpdateThreadFunc();

    std::atomic<float> displayWidth_{1920.0f};
    std::atomic<float> displayHeight_{1080.0f};
    std::atomic<int> displayOrientation_{0};

    bool SelectTarget(const Vec2& screenCenter, bool requireVisibility,
                      uint64_t& outActorAddr, int& outBoneID, Vec2& outScreenPos);
    Vec2 ComputePDOutput(const Vec2& targetPos, const Vec2& screenCenter,
                         const Vec2& feedforward, float deltaTime, float fovScale);
    float DistanceToScreenCenter(const Vec2& pos, const Vec2& center) const;
    bool IsInFOV(const Vec2& pos, const Vec2& center) const;
    bool ShouldSwitchTarget(const Vec2& currentPos, const Vec2& newPos, const Vec2& center) const;
    void DrawDebugVisuals(const Vec2& targetPos, const Vec2& screenCenter);
    void DrawRecoilSpeedDebugWindow();
    void DrawFireStateDebugWindow();
    void ReleaseTriggerTouch();
    void UpdateTriggerBot(const Vec2& rawScreenCenter);
    bool IsLocalPlayerFiring();
};

extern AutoAimController* gAutoAim;

void InitAutoAim();
void ShutdownAutoAim();
bool ComputePredictedAimPoint(uint64_t actorAddr, float targetDistanceMeters,
                              const Vec2& currentTargetScreenPos, const FMatrix& vpMat,
                              float screenWidth, float screenHeight,
                              PredictedAimPoint& outPrediction);
