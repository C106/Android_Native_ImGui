#include "auto_aim.h"
#include "ImGuiLayer.h"
#include "Gyro.h"
#include "driver_manager.h"
#include <cmath>
#include <algorithm>

extern Gyro* Gyro_Controller;
extern Addresses address;
extern Offsets offset;

AutoAimController* gAutoAim = nullptr;

namespace {
constexpr uintptr_t kGameInstanceOffset = 0xb00;
constexpr uintptr_t kFrontendHUDOffset = 0x4a8;
constexpr uintptr_t kUserSettingsOffset = 0xbc8;
constexpr uintptr_t kIsGunADSOffset = 0x1798;
constexpr uintptr_t kFPViewSwitchOffset = 0x2a0;

constexpr uintptr_t kGyroscopeSenNoneSniperOffset = 0x88;
constexpr uintptr_t kGyroscopeSenRedDotOffset = 0x8c;
constexpr uintptr_t kGyroscopeSen2XOffset = 0x90;
constexpr uintptr_t kGyroscopeSen3XOffset = 0x2d4;
constexpr uintptr_t kGyroscopeSen4XOffset = 0x94;
constexpr uintptr_t kGyroscopeSen6XOffset = 0x2d8;
constexpr uintptr_t kGyroscopeSen8XOffset = 0x98;
constexpr uintptr_t kGyroscopeSenNoneSniperFPOffset = 0x2b4;

constexpr uintptr_t kFireGyroscopeSenNoneSniperOffset = 0x58c;
constexpr uintptr_t kFireGyroscopeSenRedDotOffset = 0x590;
constexpr uintptr_t kFireGyroscopeSen2XOffset = 0x594;
constexpr uintptr_t kFireGyroscopeSen4XOffset = 0x598;
constexpr uintptr_t kFireGyroscopeSen8XOffset = 0x59c;
constexpr uintptr_t kFireGyroscopeSen3XOffset = 0x5a0;
constexpr uintptr_t kFireGyroscopeSen6XOffset = 0x5a4;
constexpr uintptr_t kFireGyroscopeSenNoneSniperFPOffset = 0x5b0;

// PlayerCameraManager → CameraCache → POV → FOV
constexpr uintptr_t kCameraCachePOVFOVOffset = 0x680;  // 0x640 + 0x10 + 0x30
constexpr float kDefaultFOV = 80.0f;  // 默认无镜 FOV（度）

constexpr float kDefaultGyroSensitivity = 1.0f;
constexpr float kMaxGyroStrength = 20.0f;
constexpr float kAutoAimBoost = 2.6f;
constexpr float kMinGyroDrive = 0.55f;
constexpr float kDerivativeClamp = 4.0f;
constexpr float kHorizontalNearCenterThreshold = 0.02f;
constexpr float kVerticalNearCenterThreshold = 0.02f;
constexpr float kHorizontalNearCenterMinScale = 0.50f;
constexpr float kVerticalNearCenterMinScale = 0.50f;

// 目标速度
constexpr uintptr_t kSTCharacterMovementOffset = 0x2b48;  // actor → STCharacterMovementComponent*
constexpr uintptr_t kMovementVelocityOffset = 0x13c;       // CMC → Velocity (Vec3, cm/s)
constexpr uintptr_t kCurrentVehicleOffset = 0x1288;         // actor → CurrentVehicle* (STExtraVehicleBase*)
constexpr uintptr_t kReplicatedMovementOffset = 0x170;       // Actor → ReplicatedMovement (RepMovement)
// RepMovement.LinearVelocity 在偏移 0x0，所以载具速度 = actor + 0x170

// 本地玩家武器 → 子弹速度
constexpr uintptr_t kCurrentWeaponOffset = 0x10e8;          // actor → CurrentUsingWeaponSafety
constexpr uintptr_t kShootWeaponEntityCompOffset = 0x1d08;  // STExtraShootWeapon → ShootWeaponEntityComp*
constexpr uintptr_t kBulletFireSpeedOffset = 0x159c;        // ShootWeaponEntity → BulletFireSpeed (float, cm/s)
constexpr uintptr_t kRecoilInfoOffset = 0x1dd0;             // ShootWeaponEntity → SRecoilInfo
constexpr uintptr_t kAccessoriesVRecoilFactorOffset = 0x1e98;
constexpr uintptr_t kAccessoriesVRecoilFactorModifierOffset = 0x1e9c;
constexpr uintptr_t kVerticalRecoilFactorModifierOffset = 0x1ea0;
constexpr uintptr_t kAccessoriesHRecoilFactorOffset = 0x1ea4;
constexpr uintptr_t kAccessoriesHRecoilFactorModifierOffset = 0x1ea8;
constexpr uintptr_t kHorizontalRecoilFactorModifierOffset = 0x1eac;
constexpr uintptr_t kAccessoriesAllRecoilFactorModifierOffset = 0x1eb0;
constexpr uintptr_t kAccessoriesRecoveryFactorOffset = 0x1eb4;

// 默认值
constexpr float kDefaultBulletSpeed = 80000.0f;  // 默认子弹速度 cm/s (800 m/s)
constexpr float kMaxLeadTime = 1.5f;             // 最大预判时间（秒）
constexpr float kFeedforwardGain = 0.5f;         // 前馈增益（归一化屏幕速率 → 陀螺仪输出）

struct RecoilArrayHeader {
    uint64_t data = 0;
    int32_t num = 0;
    int32_t max = 0;
};

struct LocalRecoilInfo {
    float verticalRecoilMin;              // 0x00
    float verticalRecoilMax;              // 0x04
    float verticalRecoilVariation;        // 0x08
    float verticalRecoveryModifier;       // 0x0c
    float verticalRecoveryClamp;          // 0x10
    float verticalRecoveryMax;            // 0x14
    float leftMax;                        // 0x18
    float rightMax;                       // 0x1c
    float horizontalTendency;             // 0x20
    uint32_t pad24;                       // 0x24
    uint64_t recoilCurve;                 // 0x28
    uint64_t recoilCurveOneBurst;         // 0x30
    uint64_t recoilCurveMultiBurst;       // 0x38
    int32_t bulletPerSwitch;              // 0x40
    float timePerSwitch;                  // 0x44
    uint8_t switchOnTime;                 // 0x48
    uint8_t pad49[3];                     // 0x49
    float recoilSpeedVertical;            // 0x4c
    float recoilSpeedHorizontal;          // 0x50
    float recoverySpeedVertical;          // 0x54
    float recoilValueClimb;               // 0x58
    float recoilValueFail;                // 0x5c
    float recoilModifierStand;            // 0x60
    float recoilModifierCrouch;           // 0x64
    float recoilModifierProne;            // 0x68
    float recoilHorizontalMinScalar;      // 0x6c
    float burstEmptyDelay;                // 0x70
    uint8_t shootSightReturn;             // 0x74
    uint8_t pad75[3];                     // 0x75
    float shootSightReturnSpeed;          // 0x78
    float recoilCurveStart;               // 0x7c
    float recoilCurveEnd;                 // 0x80
    float recoilCurveOneBurstStart;       // 0x84
    float recoilCurveOneBurstEnd;         // 0x88
    float recoilCurveMultiBurstStart;     // 0x8c
    float recoilCurveMultiBurstEnd;       // 0x90
    float recoilCurveSamplingInterval;    // 0x94
    RecoilArrayHeader recoilCurveArray;   // 0x98
    RecoilArrayHeader recoilCurveOneBurstArray; // 0xa8
    RecoilArrayHeader recoilCurveMultiBurstArray; // 0xb8
};

static_assert(sizeof(RecoilArrayHeader) == 0x10, "RecoilArrayHeader size mismatch");
static_assert(sizeof(LocalRecoilInfo) == 0xc8, "LocalRecoilInfo size mismatch");

static float SanitizeRecoilFactor(float value) {
    if (!std::isfinite(value)) return 1.0f;
    return value;
}

static Vec2 UpdateRecoilCenterOffset(const AutoAimConfig& config, TargetState& state, const RecoilDebugInfo& recoil,
                                     bool isFiring, float deltaTime, float fovRatio,
                                     float screenHeight) {
    if (deltaTime <= 0.0f) {
        return Vec2(0.0f, -(state.recoilBaseLiftOffset + state.recoilKickOffset));
    }

    if (!recoil.valid) {
        const float fallbackRecover = screenHeight * 1.5f * deltaTime;
        state.recoilKickOffset = std::max(0.0f, state.recoilKickOffset - fallbackRecover);
        state.recoilBaseLiftOffset = std::max(0.0f, state.recoilBaseLiftOffset - fallbackRecover);
        return Vec2(0.0f, -(state.recoilBaseLiftOffset + state.recoilKickOffset));
    }

    const float zoomScale = 1.0f / std::max(fovRatio, 0.20f);
    const float maxOffset = screenHeight * config.maxRecoilOffsetFraction;
    const float baseRiseStep =
        recoil.realtimeVerticalRecoilSpeed * config.recoilBaseOffsetScale * deltaTime * zoomScale;
    const float recoverStep =
        recoil.realtimeRecoverySpeed * config.recoilRecoveryReturnScale * deltaTime * zoomScale;
    const float sightReturnStep = recoil.shootSightReturn
        ? (recoil.shootSightReturnSpeed * 0.18f * deltaTime * zoomScale)
        : 0.0f;
    const float kickTarget = isFiring
        ? std::clamp((config.recoilKickOffsetScale / std::max(recoil.realtimeRecoverySpeed, 1.0f)) * zoomScale,
                     0.0f, maxOffset * 0.45f)
        : 0.0f;
    const float kickFollow = std::clamp((recoil.realtimeRecoverySpeed * 0.02f + 8.0f) * deltaTime, 0.0f, 1.0f);

    if (isFiring) {
        state.recoilBaseLiftOffset = std::clamp(state.recoilBaseLiftOffset + baseRiseStep, 0.0f, maxOffset);
    }

    const float totalRecover = std::max(recoverStep + sightReturnStep, 0.0f);
    state.recoilBaseLiftOffset = std::max(0.0f, state.recoilBaseLiftOffset - totalRecover);
    state.recoilKickOffset += (kickTarget - state.recoilKickOffset) * kickFollow;
    if (!isFiring && state.recoilKickOffset < 0.01f) {
        state.recoilKickOffset = 0.0f;
    }

    return Vec2(0.0f, -(state.recoilBaseLiftOffset + state.recoilKickOffset));
}

struct UserGyroSensitivity {
    float hipfire = kDefaultGyroSensitivity;
    float hipfireFP = kDefaultGyroSensitivity;
    float redDot = kDefaultGyroSensitivity;
    float scope2x = kDefaultGyroSensitivity;
    float scope3x = kDefaultGyroSensitivity;
    float scope4x = kDefaultGyroSensitivity;
    float scope6x = kDefaultGyroSensitivity;
    float scope8x = kDefaultGyroSensitivity;
    float firingHipfire = kDefaultGyroSensitivity;
    float firingFP = kDefaultGyroSensitivity;
    float firingRedDot = kDefaultGyroSensitivity;
    float firing2x = kDefaultGyroSensitivity;
    float firing3x = kDefaultGyroSensitivity;
    float firing4x = kDefaultGyroSensitivity;
    float firing6x = kDefaultGyroSensitivity;
    float firing8x = kDefaultGyroSensitivity;
    bool fpViewSwitch = false;
};

static uint64_t GetUserSettingsPtr() {
    if (!Paradise_hook || address.libUE4 == 0) return 0;

    uint64_t uworld = Paradise_hook->read<uint64_t>(address.libUE4 + offset.Gworld);
    if (uworld == 0) return 0;

    uint64_t gameInstance = Paradise_hook->read<uint64_t>(uworld + kGameInstanceOffset);
    if (gameInstance == 0) return 0;

    uint64_t frontendHUD = Paradise_hook->read<uint64_t>(gameInstance + kFrontendHUDOffset);
    if (frontendHUD == 0) return 0;

    return Paradise_hook->read<uint64_t>(frontendHUD + kUserSettingsOffset);
}

static float ReadValidSensitivity(uint64_t base, uintptr_t fieldOffset, float fallback) {
    if (base == 0) return fallback;
    float value = Paradise_hook->read<float>(base + fieldOffset);
    if (value <= 0.0f || value > 5.0f) return fallback;
    return value;
}

static UserGyroSensitivity ReadUserGyroSensitivity() {
    UserGyroSensitivity sens;
    uint64_t userSettings = GetUserSettingsPtr();
    if (userSettings == 0) return sens;

    sens.hipfire = ReadValidSensitivity(userSettings, kGyroscopeSenNoneSniperOffset, sens.hipfire);
    sens.hipfireFP = ReadValidSensitivity(userSettings, kGyroscopeSenNoneSniperFPOffset, sens.hipfireFP);
    sens.redDot = ReadValidSensitivity(userSettings, kGyroscopeSenRedDotOffset, sens.redDot);
    sens.scope2x = ReadValidSensitivity(userSettings, kGyroscopeSen2XOffset, sens.scope2x);
    sens.scope3x = ReadValidSensitivity(userSettings, kGyroscopeSen3XOffset, sens.scope3x);
    sens.scope4x = ReadValidSensitivity(userSettings, kGyroscopeSen4XOffset, sens.scope4x);
    sens.scope6x = ReadValidSensitivity(userSettings, kGyroscopeSen6XOffset, sens.scope6x);
    sens.scope8x = ReadValidSensitivity(userSettings, kGyroscopeSen8XOffset, sens.scope8x);
    sens.firingHipfire = ReadValidSensitivity(userSettings, kFireGyroscopeSenNoneSniperOffset, sens.firingHipfire);
    sens.firingFP = ReadValidSensitivity(userSettings, kFireGyroscopeSenNoneSniperFPOffset, sens.firingFP);
    sens.firingRedDot = ReadValidSensitivity(userSettings, kFireGyroscopeSenRedDotOffset, sens.firingRedDot);
    sens.firing2x = ReadValidSensitivity(userSettings, kFireGyroscopeSen2XOffset, sens.firing2x);
    sens.firing3x = ReadValidSensitivity(userSettings, kFireGyroscopeSen3XOffset, sens.firing3x);
    sens.firing4x = ReadValidSensitivity(userSettings, kFireGyroscopeSen4XOffset, sens.firing4x);
    sens.firing6x = ReadValidSensitivity(userSettings, kFireGyroscopeSen6XOffset, sens.firing6x);
    sens.firing8x = ReadValidSensitivity(userSettings, kFireGyroscopeSen8XOffset, sens.firing8x);
    sens.fpViewSwitch = Paradise_hook->read<uint8_t>(userSettings + kFPViewSwitchOffset) != 0;
    return sens;
}

static bool IsLocalPlayerADS() {
    if (address.LocalPlayerActor == 0 || !Paradise_hook) return false;
    uint8_t adsByte = Paradise_hook->read<uint8_t>(address.LocalPlayerActor + kIsGunADSOffset);
    return (adsByte & 0x01) != 0;
}

static float ReadCameraFOV() {
    if (!Paradise_hook || address.libUE4 == 0) return kDefaultFOV;

    uint64_t uworld = Paradise_hook->read<uint64_t>(address.libUE4 + offset.Gworld);
    if (uworld == 0) return kDefaultFOV;

    // UWorld → NetDriver → ServerConnection → PlayerController → PlayerCameraManager
    uint64_t netDriver = Paradise_hook->read<uint64_t>(uworld + offset.NetDriver);
    if (netDriver == 0) return kDefaultFOV;
    uint64_t serverConn = Paradise_hook->read<uint64_t>(netDriver + offset.ServerConnection);
    if (serverConn == 0) return kDefaultFOV;
    uint64_t playerController = Paradise_hook->read<uint64_t>(serverConn + offset.PlayerController);
    if (playerController == 0) return kDefaultFOV;
    uint64_t pcm = Paradise_hook->read<uint64_t>(playerController + offset.PlayerCameraManager);
    if (pcm == 0) return kDefaultFOV;

    float fov = Paradise_hook->read<float>(pcm + kCameraCachePOVFOVOffset);
    if (fov <= 0.0f || fov > 180.0f) return kDefaultFOV;
    return fov;
}

// 根据当前 FOV 判断倍镜类型，选择对应灵敏度
static float SelectGyroSensitivityScale(const UserGyroSensitivity& sens, bool isFiring, bool isADS, float cameraFOV) {
    // 根据 FOV 范围判断倍镜等级
    // 典型 FOV 值: 无镜~80, 红点~55, 2x~40, 3x~27, 4x~20, 6x~14, 8x~10
    float selected;
    if (!isADS || cameraFOV >= 70.0f) {
        // 腰射 / 无镜
        if (isFiring) {
            selected = sens.fpViewSwitch ? sens.firingFP : sens.firingHipfire;
        } else {
            selected = sens.fpViewSwitch ? sens.hipfireFP : sens.hipfire;
        }
    } else if (cameraFOV >= 45.0f) {
        // 红点 / 全息
        selected = isFiring ? sens.firingRedDot : sens.redDot;
    } else if (cameraFOV >= 33.0f) {
        // 2x
        selected = isFiring ? sens.firing2x : sens.scope2x;
    } else if (cameraFOV >= 23.0f) {
        // 3x
        selected = isFiring ? sens.firing3x : sens.scope3x;
    } else if (cameraFOV >= 16.0f) {
        // 4x
        selected = isFiring ? sens.firing4x : sens.scope4x;
    } else if (cameraFOV >= 11.0f) {
        // 6x
        selected = isFiring ? sens.firing6x : sens.scope6x;
    } else {
        // 8x
        selected = isFiring ? sens.firing8x : sens.scope8x;
    }

    if (selected <= 0.0f) return 1.0f;
    return std::clamp(selected, 0.1f, 4.0f);
}

static Vec2 ClampGyroStrength(const Vec2& input) {
    return Vec2(
        std::clamp(input.x, -kMaxGyroStrength, kMaxGyroStrength),
        std::clamp(input.y, -kMaxGyroStrength, kMaxGyroStrength));
}

static float ApplyGyroFloor(float value) {
    float absValue = std::fabs(value);
    if (absValue < 0.02f) return 0.0f;
    return value;
}

static float ApplyVerticalGyroFloor(float value) {
    float absValue = std::fabs(value);
    if (absValue < 0.02f) return 0.0f;
    return value;
}

static float ScaleAxisNearCenter(float value, float normalizedError, float threshold, float minScale) {
    if (normalizedError >= threshold) return value;
    float factor = std::clamp(normalizedError / threshold, minScale, 1.0f);
    return value * factor;
}

static Vec3 ReadActorVelocity(uint64_t actorAddr) {
    if (actorAddr == 0 || !Paradise_hook) return Vec3::Zero();

    // 若目标在载具中，从载具的 ReplicatedMovement.LinearVelocity 读取速度
    // 载具是 Pawn 而非 Character，没有 CharacterMovementComponent
    uint64_t vehicle = Paradise_hook->read<uint64_t>(actorAddr + kCurrentVehicleOffset);
    if (vehicle != 0) {
        Vec3 vel;
        Paradise_hook->read(vehicle + kReplicatedMovementOffset, &vel, sizeof(Vec3));
        return vel;
    }

    // 角色：从 CharacterMovementComponent.Velocity 读取
    uint64_t cmc = Paradise_hook->read<uint64_t>(actorAddr + kSTCharacterMovementOffset);
    if (cmc == 0) return Vec3::Zero();
    Vec3 vel;
    Paradise_hook->read(cmc + kMovementVelocityOffset, &vel, sizeof(Vec3));
    return vel;
}

static float ReadBulletFireSpeed() {
    if (address.LocalPlayerActor == 0 || !Paradise_hook) return kDefaultBulletSpeed;
    uint64_t weapon = Paradise_hook->read<uint64_t>(address.LocalPlayerActor + kCurrentWeaponOffset);
    if (weapon == 0) return kDefaultBulletSpeed;
    uint64_t entityComp = Paradise_hook->read<uint64_t>(weapon + kShootWeaponEntityCompOffset);
    if (entityComp == 0) return kDefaultBulletSpeed;
    float speed = Paradise_hook->read<float>(entityComp + kBulletFireSpeedOffset);
    if (speed <= 0.0f) return kDefaultBulletSpeed;
    return speed;
}

static uint64_t ReadCurrentShootWeaponEntityComp() {
    if (address.LocalPlayerActor == 0 || !Paradise_hook) return 0;
    uint64_t weapon = Paradise_hook->read<uint64_t>(address.LocalPlayerActor + kCurrentWeaponOffset);
    if (weapon == 0) return 0;
    return Paradise_hook->read<uint64_t>(weapon + kShootWeaponEntityCompOffset);
}

static RecoilDebugInfo ReadCurrentRecoilDebugInfo() {
    RecoilDebugInfo info;
    if (!Paradise_hook) return info;

    const uint64_t entityComp = ReadCurrentShootWeaponEntityComp();
    if (entityComp == 0) return info;

    LocalRecoilInfo recoil{};
    Paradise_hook->read(entityComp + kRecoilInfoOffset, &recoil, sizeof(recoil));

    info.entityComp = entityComp;
    info.valid = true;
    info.verticalRecoilMin = recoil.verticalRecoilMin;
    info.verticalRecoilMax = recoil.verticalRecoilMax;
    info.verticalRecoilVariation = recoil.verticalRecoilVariation;
    info.verticalRecoveryModifier = recoil.verticalRecoveryModifier;
    info.verticalRecoveryClamp = recoil.verticalRecoveryClamp;
    info.verticalRecoveryMax = recoil.verticalRecoveryMax;
    info.leftMax = recoil.leftMax;
    info.rightMax = recoil.rightMax;
    info.horizontalTendency = recoil.horizontalTendency;
    info.bulletPerSwitch = recoil.bulletPerSwitch;
    info.timePerSwitch = recoil.timePerSwitch;
    info.switchOnTime = recoil.switchOnTime != 0;
    info.recoilSpeedVertical = recoil.recoilSpeedVertical;
    info.recoilSpeedHorizontal = recoil.recoilSpeedHorizontal;
    info.recoverySpeedVertical = recoil.recoverySpeedVertical;
    info.recoilValueClimb = recoil.recoilValueClimb;
    info.recoilValueFail = recoil.recoilValueFail;
    info.recoilModifierStand = recoil.recoilModifierStand;
    info.recoilModifierCrouch = recoil.recoilModifierCrouch;
    info.recoilModifierProne = recoil.recoilModifierProne;
    info.recoilHorizontalMinScalar = recoil.recoilHorizontalMinScalar;
    info.burstEmptyDelay = recoil.burstEmptyDelay;
    info.shootSightReturn = recoil.shootSightReturn != 0;
    info.shootSightReturnSpeed = recoil.shootSightReturnSpeed;
    info.recoilCurveStart = recoil.recoilCurveStart;
    info.recoilCurveEnd = recoil.recoilCurveEnd;
    info.recoilCurveOneBurstStart = recoil.recoilCurveOneBurstStart;
    info.recoilCurveOneBurstEnd = recoil.recoilCurveOneBurstEnd;
    info.recoilCurveMultiBurstStart = recoil.recoilCurveMultiBurstStart;
    info.recoilCurveMultiBurstEnd = recoil.recoilCurveMultiBurstEnd;
    info.recoilCurveSamplingInterval = recoil.recoilCurveSamplingInterval;
    info.recoilCurve = recoil.recoilCurve;
    info.recoilCurveOneBurst = recoil.recoilCurveOneBurst;
    info.recoilCurveMultiBurst = recoil.recoilCurveMultiBurst;
    info.recoilCurveArrayNum = recoil.recoilCurveArray.num;
    info.recoilCurveOneBurstArrayNum = recoil.recoilCurveOneBurstArray.num;
    info.recoilCurveMultiBurstArrayNum = recoil.recoilCurveMultiBurstArray.num;

    info.accessoriesVRecoilFactor = Paradise_hook->read<float>(entityComp + kAccessoriesVRecoilFactorOffset);
    info.accessoriesVRecoilFactorModifier = Paradise_hook->read<float>(entityComp + kAccessoriesVRecoilFactorModifierOffset);
    info.verticalRecoilFactorModifier = Paradise_hook->read<float>(entityComp + kVerticalRecoilFactorModifierOffset);
    info.accessoriesHRecoilFactor = Paradise_hook->read<float>(entityComp + kAccessoriesHRecoilFactorOffset);
    info.accessoriesHRecoilFactorModifier = Paradise_hook->read<float>(entityComp + kAccessoriesHRecoilFactorModifierOffset);
    info.horizontalRecoilFactorModifier = Paradise_hook->read<float>(entityComp + kHorizontalRecoilFactorModifierOffset);
    info.accessoriesAllRecoilFactorModifier = Paradise_hook->read<float>(entityComp + kAccessoriesAllRecoilFactorModifierOffset);
    info.accessoriesRecoveryFactor = Paradise_hook->read<float>(entityComp + kAccessoriesRecoveryFactorOffset);

    const float allRecoilFactor = SanitizeRecoilFactor(info.accessoriesAllRecoilFactorModifier);
    const float verticalFactor =
        SanitizeRecoilFactor(info.accessoriesVRecoilFactor) *
        SanitizeRecoilFactor(info.accessoriesVRecoilFactorModifier) *
        SanitizeRecoilFactor(info.verticalRecoilFactorModifier) *
        allRecoilFactor;
    const float horizontalFactor =
        SanitizeRecoilFactor(info.accessoriesHRecoilFactor) *
        SanitizeRecoilFactor(info.accessoriesHRecoilFactorModifier) *
        SanitizeRecoilFactor(info.horizontalRecoilFactorModifier) *
        allRecoilFactor;
    const float recoveryFactor = SanitizeRecoilFactor(info.accessoriesRecoveryFactor);

    // 推导值：以 SRecoilInfo 基础速度乘以当前附件/全局修正，得到实时后坐速度。
    info.realtimeVerticalRecoilSpeed = info.recoilSpeedVertical * verticalFactor;
    info.realtimeHorizontalRecoilSpeed = info.recoilSpeedHorizontal * horizontalFactor;
    info.realtimeRecoverySpeed = info.recoverySpeedVertical * recoveryFactor;
    return info;
}

static Vec3 ReadActorRootWorldPos(uint64_t actorAddr) {
    if (actorAddr == 0 || !Paradise_hook) return Vec3::Zero();
    uint64_t rootComp = Paradise_hook->read<uint64_t>(actorAddr + offset.RootComponent);
    if (rootComp == 0) return Vec3::Zero();
    FTransform transform = Paradise_hook->read<FTransform>(rootComp + offset.ComponentToWorld);
    return transform.Translation;
}

static Vec3 ReadCameraWorldPos() {
    if (!Paradise_hook || address.libUE4 == 0) return ReadActorRootWorldPos(address.LocalPlayerActor);

    uint64_t uworld = Paradise_hook->read<uint64_t>(address.libUE4 + offset.Gworld);
    if (uworld == 0) return ReadActorRootWorldPos(address.LocalPlayerActor);

    uint64_t netDriver = Paradise_hook->read<uint64_t>(uworld + offset.NetDriver);
    if (netDriver == 0) return ReadActorRootWorldPos(address.LocalPlayerActor);
    uint64_t serverConn = Paradise_hook->read<uint64_t>(netDriver + offset.ServerConnection);
    if (serverConn == 0) return ReadActorRootWorldPos(address.LocalPlayerActor);
    uint64_t playerController = Paradise_hook->read<uint64_t>(serverConn + offset.PlayerController);
    if (playerController == 0) return ReadActorRootWorldPos(address.LocalPlayerActor);
    uint64_t pcm = Paradise_hook->read<uint64_t>(playerController + offset.PlayerCameraManager);
    if (pcm == 0) return ReadActorRootWorldPos(address.LocalPlayerActor);

    Vec3 cameraWorldPos = Paradise_hook->read<Vec3>(pcm + offset.CameraCache + offset.POV);
    if (!std::isfinite(cameraWorldPos.X) || !std::isfinite(cameraWorldPos.Y) || !std::isfinite(cameraWorldPos.Z)) {
        return ReadActorRootWorldPos(address.LocalPlayerActor);
    }
    return cameraWorldPos;
}

static void HoltUpdate(HoltState& state, const Vec2& observation, float alpha, float beta) {
    if (!state.initialized) {
        state.level = observation;
        state.trend = Vec2(0, 0);
        state.initialized = true;
        return;
    }
    Vec2 prevLevel = state.level;
    state.level = observation * alpha + (state.level + state.trend) * (1.0f - alpha);
    state.trend = (state.level - prevLevel) * beta + state.trend * (1.0f - beta);
}

static Vec2 HoltPredict(const HoltState& state, float h) {
    return state.level + state.trend * h;
}
} // namespace

void InitAutoAim() {
    if (!gAutoAim) {
        gAutoAim = new AutoAimController();
    }
}

void ShutdownAutoAim() {
    if (gAutoAim) {
        delete gAutoAim;
        gAutoAim = nullptr;
    }
}

AutoAimController::AutoAimController() {
    config.enabled = false;
    config.onlyWhenFiring = true;
    config.targetBone = BONE_HEAD;
    config.maxDistance = 200.0f;
    config.fovLimit = 30.0f;
    config.KpX = 2.00f;
    config.KdX = 0.50f;
    config.KpY = 1.50f;
    config.KdY = 0.50f;
    config.outputScaleX = 1.00f;
    config.outputScaleY = 1.00f;
    config.filterTeammates = true;
    config.visibilityCheck = false;
    config.hysteresisThreshold = 35.0f;
    config.drawDebug = false;

    ResetTarget();
}

void AutoAimController::ResetTarget() {
    targetState.actorAddr = 0;
    targetState.boneID = -1;
    targetState.lastScreenPos = Vec2(0, 0);
    targetState.lastError = Vec2(0, 0);
    targetState.lastOutput = Vec2(0, 0);
    targetState.debugRawScreenCenter = Vec2(0, 0);
    targetState.debugEffectiveScreenCenter = Vec2(0, 0);
    targetState.debugRecoilCenterOffset = Vec2(0, 0);
    targetState.valid = false;
    targetState.holt.initialized = false;
    targetState.holt.level = Vec2(0, 0);
    targetState.holt.trend = Vec2(0, 0);
}

float AutoAimController::DistanceToScreenCenter(const Vec2& pos, const Vec2& center) const {
    float dx = pos.x - center.x;
    float dy = pos.y - center.y;
    return sqrtf(dx * dx + dy * dy);
}

bool AutoAimController::IsInFOV(const Vec2& pos, const Vec2& center) const {
    ImGuiIO& io = ImGui::GetIO();
    float distancePixels = DistanceToScreenCenter(pos, center);
    float screenDiagonal = sqrtf(io.DisplaySize.x * io.DisplaySize.x +
                                  io.DisplaySize.y * io.DisplaySize.y);
    float fovRadiusPixels = tanf(config.fovLimit * M_PI / 180.0f) * screenDiagonal / 2.0f;
    return distancePixels <= fovRadiusPixels;
}

bool AutoAimController::ShouldSwitchTarget(const Vec2& currentPos, const Vec2& newPos, const Vec2& center) const {
    if (!targetState.valid) return true;

    float currentDist = DistanceToScreenCenter(currentPos, center);
    float newDist = DistanceToScreenCenter(newPos, center);

    // 只有新目标明显更近时才切换（滞后阈值）
    return (currentDist - newDist) > config.hysteresisThreshold;
}

bool AutoAimController::SelectTarget(const Vec2& screenCenter, uint64_t& outActorAddr, int& outBoneID, Vec2& outScreenPos) {
    const auto& boneCache = GetBoneScreenCache();
    if (boneCache.empty()) return false;
    uint64_t currentFrame = ReadFrameCounter();
    const Vec3 cameraWorldPos = ReadCameraWorldPos();

    uint64_t bestActor = 0;
    int bestBone = -1;
    Vec2 bestPos;
    float bestDistance = FLT_MAX;

    for (const auto& pair : boneCache) {
        const BoneScreenData& bsd = pair.second;
        if (!bsd.valid) continue;
        if (currentFrame != 0 && bsd.frameCounter != 0 && bsd.frameCounter != currentFrame) continue;

        // 过滤本地玩家
        if (bsd.actorAddr == address.LocalPlayerActor) continue;

        // 过滤队友（可选）
        if (config.filterTeammates) {
            // TODO: 需要获取本地玩家的 teamID 进行比较
            // 暂时跳过队友过滤实现
        }

        // 距离过滤
        if (bsd.distance > config.maxDistance) continue;

        int candidateBone = config.targetBone;
        const bool requireVisibility = gUseDepthBufferVisibility && config.visibilityCheck;
        auto boneUsable = [&](int boneID) -> bool {
            return bsd.onScreen[boneID] && (!requireVisibility || bsd.visible[boneID]);
        };

        if (!boneUsable(candidateBone)) {
            static const int kFallbackBones[] = {
                BONE_CHEST, BONE_NECK, BONE_PELVIS, BONE_HEAD
            };
            bool foundFallback = false;
            for (int boneID : kFallbackBones) {
                if (boneUsable(boneID)) {
                    candidateBone = boneID;
                    foundFallback = true;
                    break;
                }
            }
            if (!foundFallback) continue;
        }

        Vec2 targetPos = bsd.screenPos[candidateBone];

        // FOV 过滤
        if (!IsInFOV(targetPos, screenCenter)) continue;

        Vec3 targetWorldPos;
        if (!GetCachedBoneWorldPos(bsd.actorAddr, candidateBone, currentFrame, targetWorldPos)) {
            targetWorldPos = ReadActorRootWorldPos(bsd.actorAddr);
        }

        float distToCenter = DistanceToScreenCenter(targetPos, screenCenter);

        // 如果有当前目标，应用滞后阈值
        if (targetState.valid && targetState.actorAddr == bsd.actorAddr) {
            // 当前目标仍然有效，优先保持
            bestActor = bsd.actorAddr;
            bestBone = candidateBone;
            bestPos = targetPos;
            bestDistance = distToCenter;
            break;
        }

        // 选择最近的目标
        if (distToCenter < bestDistance) {
            bestDistance = distToCenter;
            bestActor = bsd.actorAddr;
            bestBone = candidateBone;
            bestPos = targetPos;
        }
    }

    if (bestActor != 0) {
        outActorAddr = bestActor;
        outBoneID = bestBone;
        outScreenPos = bestPos;
        return true;
    }

    return false;
}

Vec2 AutoAimController::ComputePDOutput(const Vec2& aimPos, const Vec2& screenCenter,
                                         const Vec2& feedforward, float deltaTime, float fovRatio) {
    Vec2 screenHalf(std::max(screenCenter.x, 1.0f), std::max(screenCenter.y, 1.0f));

    Vec2 error = aimPos - screenCenter;
    Vec2 normalizedError(error.x / screenHalf.x, error.y / screenHalf.y);

    // 误差变化率（用于阻尼）
    Vec2 errorDerivative(0, 0);
    if (deltaTime > 0.0001f) {
        Vec2 lastNormalizedError(
            targetState.lastError.x / screenHalf.x,
            targetState.lastError.y / screenHalf.y);
        errorDerivative = (normalizedError - lastNormalizedError) / deltaTime;
    }
    errorDerivative.x = std::clamp(errorDerivative.x, -kDerivativeClamp, kDerivativeClamp);
    errorDerivative.y = std::clamp(errorDerivative.y, -kDerivativeClamp, kDerivativeClamp);

    auto shapeAxis = [](float v) -> float {
        float sign = (v >= 0.0f) ? 1.0f : -1.0f;
        float mag = std::fabs(v);
        if (mag < 0.002f) return 0.0f;
        if (mag <= 1.0f) {
            return sign * std::pow(mag, 0.72f);
        }
        return sign * (1.0f + (mag - 1.0f));
    };

    Vec2 proportional(shapeAxis(normalizedError.x), shapeAxis(normalizedError.y));

    // PD 部分：位置修正 + 阻尼
    Vec2 pd;
    pd.x = (proportional.x * config.KpX * kMaxGyroStrength) -
        (errorDerivative.x * config.KdX);
    pd.y = (proportional.y * config.KpY * kMaxGyroStrength) -
        (errorDerivative.y * config.KdY);

    // 近中心缩放只作用于 PD，防止近中心时位置修正过大产生抖动
    pd.x = ScaleAxisNearCenter(pd.x, std::fabs(normalizedError.x),
        kHorizontalNearCenterThreshold, kHorizontalNearCenterMinScale);
    pd.y = ScaleAxisNearCenter(pd.y, std::fabs(normalizedError.y),
        kVerticalNearCenterThreshold, kVerticalNearCenterMinScale);

    // FOV 补偿仅作用于 PD：高倍镜下 PD 环路增益 ∝ 1/tan²(FOV/2)，
    // 乘 fovRatio 使各倍镜下位置修正速率一致，防止高倍镜震荡
    pd.x *= fovRatio;
    pd.y *= fovRatio;

    // 前馈不受 FOV 衰减：前馈表示的归一化屏幕速率在高倍镜下自然放大，
    // 与陀螺仪在高倍镜下的放大效果正好匹配，无需额外补偿
    Vec2 output;
    output.x = feedforward.x * kFeedforwardGain + pd.x;
    output.y = feedforward.y * kFeedforwardGain + pd.y;

    // 更新误差历史
    targetState.lastError = Vec2(normalizedError.x * screenHalf.x, normalizedError.y * screenHalf.y);

    return output;
}

void AutoAimController::DrawDebugVisuals(const Vec2& targetPos, const Vec2& screenCenter) {
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    ImGuiIO& io = ImGui::GetIO();
    const Vec2 rawCenter(io.DisplaySize.x / 2.0f, io.DisplaySize.y / 2.0f);

    // 绘制实际镜心（白色）
    float crossSize = 20.0f;
    draw_list->AddLine(
        ImVec2(screenCenter.x - crossSize, screenCenter.y),
        ImVec2(screenCenter.x + crossSize, screenCenter.y),
        IM_COL32(255, 255, 255, 255), 2.0f);
    draw_list->AddLine(
        ImVec2(screenCenter.x, screenCenter.y - crossSize),
        ImVec2(screenCenter.x, screenCenter.y + crossSize),
        IM_COL32(255, 255, 255, 255), 2.0f);

    // 绘制原始屏幕中心（灰色），便于观察枪口上跳带来的偏移
    draw_list->AddLine(
        ImVec2(rawCenter.x - crossSize * 0.6f, rawCenter.y),
        ImVec2(rawCenter.x + crossSize * 0.6f, rawCenter.y),
        IM_COL32(160, 160, 160, 180), 1.0f);
    draw_list->AddLine(
        ImVec2(rawCenter.x, rawCenter.y - crossSize * 0.6f),
        ImVec2(rawCenter.x, rawCenter.y + crossSize * 0.6f),
        IM_COL32(160, 160, 160, 180), 1.0f);

    // 绘制目标指示器（红色圆圈 = 当前骨骼位置）
    draw_list->AddCircle(
        ImVec2(targetPos.x, targetPos.y),
        10.0f,
        IM_COL32(255, 0, 0, 255), 12, 2.0f);

    // 绘制 Holt 预测位置标记（青色菱形）
    const Vec2& hp = targetState.debugHoltPredicted;
    float d = 8.0f;
    draw_list->AddQuadFilled(
        ImVec2(hp.x, hp.y - d), ImVec2(hp.x + d, hp.y),
        ImVec2(hp.x, hp.y + d), ImVec2(hp.x - d, hp.y),
        IM_COL32(0, 255, 255, 160));

    // 绘制中心到目标连线
    draw_list->AddLine(
        ImVec2(screenCenter.x, screenCenter.y),
        ImVec2(targetPos.x, targetPos.y),
        IM_COL32(255, 255, 0, 128), 1.0f);

    // 绘制 FOV 圆圈（绿色）
    float screenDiagonal = sqrtf(io.DisplaySize.x * io.DisplaySize.x +
                                  io.DisplaySize.y * io.DisplaySize.y);
    float fovRadiusPixels = tanf(config.fovLimit * M_PI / 180.0f) * screenDiagonal / 2.0f;
    draw_list->AddCircle(
        ImVec2(screenCenter.x, screenCenter.y),
        fovRadiusPixels,
        IM_COL32(0, 255, 0, 128), 64, 1.0f);

    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f, 12.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove;

    if (ImGui::Begin("##AutoAimDebug", nullptr, windowFlags)) {
        ImGui::Text("Auto-Aim Debug");
        ImGui::Separator();
        ImGui::Text("Enabled: %s", config.enabled ? "true" : "false");
        ImGui::Text("Only Fire: %s", config.onlyWhenFiring ? "true" : "false");
        ImGui::Text("Target Actor: 0x%llX", (unsigned long long)targetState.actorAddr);
        ImGui::Text("Target Bone: %d", targetState.boneID);
        ImGui::Text("Aim Pos: %.1f, %.1f", targetPos.x, targetPos.y);
        ImGui::Text("Screen Center: %.1f, %.1f", rawCenter.x, rawCenter.y);
        ImGui::Text("Effective Center: %.1f, %.1f", screenCenter.x, screenCenter.y);
        ImGui::Text("Recoil Offset: %.1f, %.1f",
            targetState.debugRecoilCenterOffset.x, targetState.debugRecoilCenterOffset.y);
        ImGui::Text("Recoil Lift/Kick: %.1f / %.1f",
            targetState.recoilBaseLiftOffset, targetState.recoilKickOffset);
        ImGui::Text("Error: %.1f, %.1f", targetState.lastError.x, targetState.lastError.y);
        ImGui::Text("Output: %.2f, %.2f", targetState.lastOutput.x, targetState.lastOutput.y);
        ImGui::Separator();
        ImGui::Text("Bullet Speed: %.0f cm/s", targetState.debugBulletSpeed);
        ImGui::Text("Lead Time: %.3f s", targetState.debugLeadTime);
        ImGui::Text("Target Vel: %.0f, %.0f, %.0f",
            targetState.debugTargetVel.X, targetState.debugTargetVel.Y, targetState.debugTargetVel.Z);
        ImGui::Text("Holt Predicted: %.1f, %.1f", hp.x, hp.y);
        ImGui::Text("Holt Alpha/Beta: %.2f / %.2f", config.holtAlpha, config.holtBeta);
        ImGui::Separator();
        ImGui::Text("Camera FOV: %.1f", targetState.debugCameraFOV);
        ImGui::Text("FOV Ratio: %.2f", targetState.debugFovScale);
        ImGui::Text("Gyro Sens: %.2f", targetState.debugGyroScale);
        ImGui::Text("Sens Compensate: %.2f", targetState.debugSensCompensate);
        ImGui::Separator();
        ImGui::Text("X Kp/Kd: %.2f / %.2f", config.KpX, config.KdX);
        ImGui::Text("Y Kp/Kd: %.2f / %.2f", config.KpY, config.KdY);
        ImGui::Text("FOV: %.1f", config.fovLimit);
        ImGui::Text("MaxDist: %.1f", config.maxDistance);
        ImGui::Text("SwitchThresh: %.1f", config.hysteresisThreshold);
    }
    ImGui::End();
}

void AutoAimController::DrawRecoilSpeedDebugWindow() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f, 230.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));

    ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove;

    if (ImGui::Begin("##RecoilSpeedDebug", nullptr, windowFlags)) {
        const RecoilDebugInfo& recoil = targetState.debugRecoil;
        ImGui::Text("Recoil Speed");
        ImGui::Separator();
        ImGui::Text("Weapon Entity: 0x%llX", (unsigned long long)recoil.entityComp);
        ImGui::Text("Valid: %s", recoil.valid ? "true" : "false");
        if (recoil.valid) {
            ImGui::Separator();
            ImGui::Text("Base Recoil V/H: %.3f / %.3f",
                recoil.recoilSpeedVertical, recoil.recoilSpeedHorizontal);
            ImGui::Text("Base Recovery V: %.3f", recoil.recoverySpeedVertical);
            ImGui::Separator();
            ImGui::Text("Realtime Recoil V/H: %.3f / %.3f",
                recoil.realtimeVerticalRecoilSpeed, recoil.realtimeHorizontalRecoilSpeed);
            ImGui::Text("Realtime Recovery V: %.3f", recoil.realtimeRecoverySpeed);
            ImGui::Separator();
            ImGui::Text("V Factors: %.3f * %.3f * %.3f * %.3f",
                recoil.accessoriesVRecoilFactor,
                recoil.accessoriesVRecoilFactorModifier,
                recoil.verticalRecoilFactorModifier,
                recoil.accessoriesAllRecoilFactorModifier);
            ImGui::Text("H Factors: %.3f * %.3f * %.3f * %.3f",
                recoil.accessoriesHRecoilFactor,
                recoil.accessoriesHRecoilFactorModifier,
                recoil.horizontalRecoilFactorModifier,
                recoil.accessoriesAllRecoilFactorModifier);
            ImGui::Text("Recovery Factor: %.3f", recoil.accessoriesRecoveryFactor);
        }
    }
    ImGui::End();
}

bool AutoAimController::IsLocalPlayerFiring() {
    if (address.LocalPlayerActor == 0) return false;
    if (!Paradise_hook) return false;

    // 读取 bIsWeaponFiring 标志
    uint8_t firingByte = Paradise_hook->read<uint8_t>(address.LocalPlayerActor + offset.bIsWeaponFiring);
    return (firingByte & 0x01) != 0;  // 检查第一个 bit
}

void AutoAimController::Update(float deltaTime) {
    if (!config.enabled) return;
    if (!Gyro_Controller || !Gyro_Controller->bGyroConnect()) return;

    targetState.debugRecoil = ReadCurrentRecoilDebugInfo();

    const bool isFiring = IsLocalPlayerFiring();
    const bool isADS = IsLocalPlayerADS();
    const float cameraFOV = ReadCameraFOV();
    const UserGyroSensitivity gyroSens = ReadUserGyroSensitivity();
    const float gyroScale = SelectGyroSensitivityScale(gyroSens, isFiring, isADS, cameraFOV);
    const float sensCompensate = 1.0f / std::max(gyroScale, 0.1f);

    targetState.debugCameraFOV = cameraFOV;
    targetState.debugGyroScale = gyroScale;
    targetState.debugSensCompensate = sensCompensate;
    targetState.debugSensHipfire = gyroSens.hipfire;
    targetState.debugSensRedDot = gyroSens.redDot;
    targetState.debugSens2x = gyroSens.scope2x;
    targetState.debugSens3x = gyroSens.scope3x;
    targetState.debugSens4x = gyroSens.scope4x;
    targetState.debugSens6x = gyroSens.scope6x;
    targetState.debugSens8x = gyroSens.scope8x;
    targetState.debugSensFireHipfire = gyroSens.firingHipfire;
    targetState.debugSensFireRedDot = gyroSens.firingRedDot;
    targetState.debugSensFire2x = gyroSens.firing2x;
    targetState.debugSensFire3x = gyroSens.firing3x;
    targetState.debugSensFire4x = gyroSens.firing4x;
    targetState.debugSensFire6x = gyroSens.firing6x;
    targetState.debugSensFire8x = gyroSens.firing8x;

    // 如果启用了"仅开火时自瞄"，检查开火状态
    if (config.onlyWhenFiring && !isFiring) {
        if (config.drawDebug) {
            DrawRecoilSpeedDebugWindow();
        }
        ResetTarget();
        Gyro_Controller->update(0, 0);
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    Vec2 rawScreenCenter(io.DisplaySize.x / 2.0f, io.DisplaySize.y / 2.0f);
    const float fovRatio = cameraFOV / kDefaultFOV;  // <1 表示放大（高倍镜）
    const Vec2 recoilCenterOffset = UpdateRecoilCenterOffset(
        config, targetState, targetState.debugRecoil, isFiring, deltaTime, fovRatio, io.DisplaySize.y);
    Vec2 screenCenter = rawScreenCenter + recoilCenterOffset;
    targetState.debugRawScreenCenter = rawScreenCenter;
    targetState.debugEffectiveScreenCenter = screenCenter;
    targetState.debugRecoilCenterOffset = recoilCenterOffset;

    uint64_t targetActor;
    int targetBone;
    Vec2 targetScreenPos;

    if (SelectTarget(screenCenter, targetActor, targetBone, targetScreenPos)) {
        // 目标切换时重置 Holt
        if (targetState.valid && targetState.actorAddr != targetActor) {
            targetState.holt.initialized = false;
            targetState.holt.level = Vec2(0, 0);
            targetState.holt.trend = Vec2(0, 0);
        }

        // 查找目标距离（从 bone cache）
        float targetDistance = 0.0f;
        {
            const auto& boneCache = GetBoneScreenCache();
            auto it = boneCache.find(targetActor);
            if (it != boneCache.end()) {
                targetDistance = it->second.distance;
            }
        }

        // a) 读取目标 3D 速度和本地玩家 3D 速度
        Vec3 targetVel = ReadActorVelocity(targetActor);
        Vec3 localVel = ReadActorVelocity(address.LocalPlayerActor);
        // 相对速度 = 目标速度 - 本地速度（世界坐标 cm/s）
        Vec3 relVel = {
            targetVel.X - localVel.X,
            targetVel.Y - localVel.Y,
            targetVel.Z - localVel.Z
        };

        // b) 读取子弹速度
        float bulletSpeed = ReadBulletFireSpeed();

        // c) 计算飞行时间: distance(米)*100 → cm, / bulletSpeed(cm/s) → 秒
        float leadTime = 0.0f;
        if (bulletSpeed > 0.0f && targetDistance > 0.0f) {
            leadTime = std::clamp((targetDistance * 100.0f) / bulletSpeed, 0.0f, kMaxLeadTime);
        }

        // d) 读取目标根世界位置
        Vec3 rootPos = ReadActorRootWorldPos(targetActor);

        // e) 用相对速度计算弹道预测位置
        Vec3 predictedRoot = {
            rootPos.X + relVel.X * leadTime,
            rootPos.Y + relVel.Y * leadTime,
            rootPos.Z + relVel.Z * leadTime
        };

        // f) 读取 VP 矩阵
        FMatrix vpMat;
        Paradise_hook->read(address.Matrix, &vpMat, sizeof(FMatrix));

        float SW = io.DisplaySize.x;
        float SH = io.DisplaySize.y;

        // g) W2S 当前根位置和预测根位置
        Vec2 currentRootScreen, predictedRootScreen;
        bool w2sCurrent = WorldToScreen(rootPos, vpMat, SW, SH, currentRootScreen);
        bool w2sPredicted = WorldToScreen(predictedRoot, vpMat, SW, SH, predictedRootScreen);

        // h) 屏幕偏移
        Vec2 screenDelta(0, 0);
        if (w2sCurrent && w2sPredicted) {
            screenDelta = predictedRootScreen - currentRootScreen;
        }

        // i) 预测骨骼屏幕位置
        Vec2 predictedBoneScreen = targetScreenPos + screenDelta;

        // 直接用弹道预测后的位置作为瞄准点
        Vec2 aimPos = predictedBoneScreen;

        // 计算相对速度前馈：将世界相对速度投影到屏幕速率（归一化/秒）
        // 用小步长 deltaTime 投影，避免近距离透视非线性放大，然后除以 deltaTime 转为速率
        Vec2 feedforward(0, 0);
        if (w2sCurrent && deltaTime > 0.0001f) {
            Vec3 rootPlusVel = {
                rootPos.X + relVel.X * deltaTime,
                rootPos.Y + relVel.Y * deltaTime,
                rootPos.Z + relVel.Z * deltaTime
            };
            Vec2 futureScreen;
            if (WorldToScreen(rootPlusVel, vpMat, SW, SH, futureScreen)) {
                Vec2 screenHalf(SW / 2.0f, SH / 2.0f);
                // 每帧位移 / deltaTime → 归一化速率（/秒），与 PD 输出单位一致
                feedforward.x = (futureScreen.x - currentRootScreen.x) / screenHalf.x / deltaTime;
                feedforward.y = (futureScreen.y - currentRootScreen.y) / screenHalf.y / deltaTime;
            }
        }

        // 存储 debug 信息
        targetState.debugBulletSpeed = bulletSpeed;
        targetState.debugLeadTime = leadTime;
        targetState.debugTargetVel = relVel;
        targetState.debugHoltPredicted = aimPos;
        targetState.debugCameraFOV = cameraFOV;
        targetState.debugFovScale = fovRatio;

        // l) 前馈 + PD 控制器
        Vec2 gyroAdjust = ComputePDOutput(aimPos, screenCenter, feedforward, deltaTime, fovRatio);

        // 灵敏度补偿：灵敏度越高，同样陀螺仪值转角越大，需要缩小输出
        // FOV 补偿已在 ComputePDOutput 内部对 PD 分量单独处理
        Vec2 finalAdjust = ClampGyroStrength(gyroAdjust * sensCompensate * kAutoAimBoost);
        finalAdjust.x = ApplyGyroFloor(finalAdjust.x);
        finalAdjust.y = ApplyVerticalGyroFloor(finalAdjust.y);
        finalAdjust = ClampGyroStrength(finalAdjust);

        // 存储 pre-negation 值用于下一帧参考
        targetState.lastOutput = finalAdjust;

        finalAdjust.x = -finalAdjust.x;
        Gyro_Controller->update(finalAdjust.x, finalAdjust.y);

        targetState.actorAddr = targetActor;
        targetState.boneID = targetBone;
        targetState.lastScreenPos = targetScreenPos;
        targetState.valid = true;

        if (config.drawDebug) {
            DrawDebugVisuals(aimPos, screenCenter);
            DrawRecoilSpeedDebugWindow();
        }
    } else {
        if (config.drawDebug) {
            DrawRecoilSpeedDebugWindow();
        }
        ResetTarget();
        Gyro_Controller->update(0, 0);
    }
}
