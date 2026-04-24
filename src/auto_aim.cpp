#include "auto_aim.h"
#include "ImGuiLayer.h"
#include "Gyro.h"
#include "driver_manager.h"
#include "game_frame_reader.h"
#include "hook_touch_event.h"
#include <cmath>
#include <algorithm>
#include <chrono>

extern Gyro* Gyro_Controller;
extern Addresses address;
extern Offsets offset;

AutoAimController* gAutoAim = nullptr;

namespace {
constexpr float kDefaultFOV = 80.0f;  // 默认无镜 FOV（度）

constexpr float kDefaultGyroSensitivity = 1.0f;
constexpr float kMaxGyroStrength = 20.0f;
constexpr float kAutoAimBoost = 2.6f;
constexpr float kMinGyroDrive = 0.55f;
constexpr float kDerivativeClamp = 4.0f;
constexpr float kHorizontalNearCenterThreshold = 0.012f;
constexpr float kVerticalNearCenterThreshold = 0.012f;
constexpr float kHorizontalNearCenterMinScale = 0.50f;
constexpr float kVerticalNearCenterMinScale = 0.50f;

// 默认值
constexpr float kDefaultBulletSpeed = 80000.0f;  // 默认子弹速度 cm/s (800 m/s)
constexpr float kMaxLeadTime = 1.5f;             // 最大预判时间（秒）
constexpr float kFeedforwardGain = 0.5f;         // 前馈增益（归一化屏幕速率 → 陀螺仪输出）

static float ComputeAngularFovScale(float cameraFOV) {
    const float clampedFOV = std::clamp(cameraFOV, 1.0f, 179.0f);
    const float halfAngle = clampedFOV * 0.5f * static_cast<float>(M_PI) / 180.0f;
    const float defaultHalfAngle = kDefaultFOV * 0.5f * static_cast<float>(M_PI) / 180.0f;
    const float scale = std::tan(halfAngle) / std::tan(defaultHalfAngle);
    return std::clamp(scale, 0.02f, 2.5f);
}

static bool HasGyroOutputChannel() {
    if (Gyro_Controller && Gyro_Controller->bGyroConnect()) return true;
    return GetDriverManager().getType() == DRIVER_PARADISE;
}

static bool HasTouchOutputChannel() {
    return GetDriverManager().supports_touch();
}

static bool IsTriggerBotHitScanBoneEnabled(const AutoAimConfig& config, int boneID) {
    switch (boneID) {
        case BONE_HEAD: return config.triggerBotHitScanHead;
        case BONE_NECK: return config.triggerBotHitScanNeck;
        case BONE_CHEST: return config.triggerBotHitScanChest;
        case BONE_PELVIS: return config.triggerBotHitScanPelvis;
        default: return false;
    }
}

static void SendGyroOutput(float x, float y) {
    if (Gyro_Controller && Gyro_Controller->bGyroConnect()) {
        Gyro_Controller->update(x, y);
        return;
    }
    if (GetDriverManager().getType() == DRIVER_PARADISE) {
        GetDriverManager().gyro_update(x, y);
    }
}

static Vec2 MapScreenAdjustToGyroOutput(const Vec2& screenAdjust, int orientation) {
    Vec2 gyro(-screenAdjust.x, screenAdjust.y);
    switch (orientation) {
        case 1:  // Rotation 90
            return Vec2(gyro.x, gyro.y);
        case 2:  // Rotation 180
            return Vec2(-gyro.x, -gyro.y);
        case 3:  // Rotation 270
            return Vec2(-gyro.x, -gyro.y);
        case 0:  // Rotation 0
        default:
            return gyro;
    }
}

static float NextNoiseUnit(uint32_t& seed) {
    seed = seed * 1664525u + 1013904223u;
    const float normalized = static_cast<float>((seed >> 8) & 0x00FFFFFFu) / 16777215.0f;
    return normalized * 2.0f - 1.0f;
}

static Vec2 ApplyHumanizeNoise(const AutoAimConfig& config, TargetState& state,
                               const Vec2& baseAdjust, float deltaTime) {
    if (!config.humanizeNoise || deltaTime <= 0.0f) {
        state.noiseCurrent = Vec2(0, 0);
        state.noiseTarget = Vec2(0, 0);
        state.noiseRetargetTimer = 0.0f;
        state.debugNoise = Vec2(0, 0);
        return Vec2(0, 0);
    }

    state.noiseTime += deltaTime;
    state.noiseRetargetTimer -= deltaTime;

    const float baseMagnitude = std::sqrt(baseAdjust.x * baseAdjust.x + baseAdjust.y * baseAdjust.y);
    const float driveScale = std::clamp(baseMagnitude / kMaxGyroStrength, 0.20f, 1.0f);
    const float retargetInterval = 1.0f / std::max(config.noiseChangeRate, 0.5f);

    if (state.noiseRetargetTimer <= 0.0f) {
        state.noiseRetargetTimer += retargetInterval;
        state.noiseTarget.x = NextNoiseUnit(state.noiseSeed) * config.noiseStrengthX * driveScale;
        state.noiseTarget.y = NextNoiseUnit(state.noiseSeed) * config.noiseStrengthY * driveScale;
    }

    const float follow = std::clamp(config.noiseSmoothing * deltaTime, 0.0f, 1.0f);
    state.noiseCurrent.x += (state.noiseTarget.x - state.noiseCurrent.x) * follow;
    state.noiseCurrent.y += (state.noiseTarget.y - state.noiseCurrent.y) * follow;

    const Vec2 microNoise(
        std::sin(state.noiseTime * 7.1f) * config.noiseMicroJitter * driveScale,
        std::cos(state.noiseTime * 9.7f) * config.noiseMicroJitter * 0.85f * driveScale);

    state.debugNoise = state.noiseCurrent + microNoise;
    return state.debugNoise;
}

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

static float SanitizeNonNegative(float value) {
    if (!std::isfinite(value) || value < 0.0f) return 0.0f;
    return value;
}

static void ClearRecoilState(TargetState& state) {
    state.recoilBaseLiftOffset = 0.0f;
    state.recoilKickOffset = 0.0f;
    state.debugRecoilCenterOffset = Vec2(0, 0);
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
        if (state.recoilKickOffset < 0.01f) state.recoilKickOffset = 0.0f;
        if (state.recoilBaseLiftOffset < 0.01f) state.recoilBaseLiftOffset = 0.0f;
        return Vec2(0.0f, -(state.recoilBaseLiftOffset + state.recoilKickOffset));
    }

    const float zoomScale = 1.0f / std::max(fovRatio, 0.20f);
    const float maxOffset = screenHeight * config.maxRecoilOffsetFraction;
    const float recoilRiseSpeed = SanitizeNonNegative(recoil.realtimeVerticalRecoilSpeed);
    const float recoilRecoverySpeed = SanitizeNonNegative(recoil.realtimeRecoverySpeed);
    const float sightReturnSpeed = SanitizeNonNegative(recoil.shootSightReturnSpeed);
    const float baseRiseStep =
        recoilRiseSpeed * config.recoilBaseOffsetScale * deltaTime * zoomScale;
    const float recoverStep =
        recoilRecoverySpeed * config.recoilRecoveryReturnScale * deltaTime * zoomScale;
    const float sightReturnStep = recoil.shootSightReturn
        ? (sightReturnSpeed * 0.18f * deltaTime * zoomScale)
        : 0.0f;
    const float kickTarget = isFiring
        ? std::clamp((config.recoilKickOffsetScale / std::max(recoilRecoverySpeed, 1.0f)) * zoomScale,
                     0.0f, maxOffset * 0.45f)
        : 0.0f;
    const float kickFollow = std::clamp((recoilRecoverySpeed * 0.02f + 8.0f) * deltaTime, 0.0f, 1.0f);

    if (isFiring) {
        state.recoilBaseLiftOffset = std::clamp(state.recoilBaseLiftOffset + baseRiseStep, 0.0f, maxOffset);
    }

    const float fallbackRecover = isFiring
        ? screenHeight * 0.18f * deltaTime
        : screenHeight * 1.50f * deltaTime;
    float totalRecover = std::max(recoverStep + sightReturnStep, 0.0f);
    if (!isFiring && state.recoilBaseLiftOffset > 0.0f) {
        totalRecover = std::max(totalRecover, fallbackRecover);
    }

    state.recoilBaseLiftOffset = std::max(0.0f, state.recoilBaseLiftOffset - totalRecover);
    state.recoilKickOffset += (kickTarget - state.recoilKickOffset) * kickFollow;
    if (!isFiring && state.recoilKickOffset > 0.0f) {
        state.recoilKickOffset = std::max(0.0f, state.recoilKickOffset - fallbackRecover);
    }
    if (!isFiring && state.recoilKickOffset < 0.01f) {
        state.recoilKickOffset = 0.0f;
    }
    if (!isFiring && state.recoilBaseLiftOffset < 0.01f) {
        state.recoilBaseLiftOffset = 0.0f;
    }
    if (!isFiring) {
        const float combinedOffset = state.recoilBaseLiftOffset + state.recoilKickOffset;
        if (combinedOffset < std::max(0.35f, screenHeight * 0.0006f)) {
            ClearRecoilState(state);
        }
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
    if (address.libUE4 == 0) return 0;

    uint64_t uworld = GetDriverManager().read<uint64_t>(address.libUE4 + offset.Gworld);
    if (uworld == 0) return 0;

    uint64_t gameInstance = GetDriverManager().read<uint64_t>(uworld + offset.GameInstance);
    if (gameInstance == 0) return 0;

    uint64_t frontendHUD = GetDriverManager().read<uint64_t>(gameInstance + offset.AssociatedFrontendHUD);
    if (frontendHUD == 0) return 0;

    return GetDriverManager().read<uint64_t>(frontendHUD + offset.FrontendHUDUserSettings);
}

static float ReadValidSensitivity(uint64_t base, uintptr_t fieldOffset, float fallback) {
    if (base == 0) return fallback;
    float value = GetDriverManager().read<float>(base + fieldOffset);
    if (value <= 0.0f || value > 5.0f) return fallback;
    return value;
}

static UserGyroSensitivity ReadUserGyroSensitivity() {
    UserGyroSensitivity sens;
    uint64_t userSettings = GetUserSettingsPtr();
    if (userSettings == 0) return sens;

    sens.hipfire = ReadValidSensitivity(userSettings, offset.GyroscopeSenNoneSniper, sens.hipfire);
    sens.hipfireFP = ReadValidSensitivity(userSettings, offset.GyroscopeSenNoneSniperFP, sens.hipfireFP);
    sens.redDot = ReadValidSensitivity(userSettings, offset.GyroscopeSenRedDot, sens.redDot);
    sens.scope2x = ReadValidSensitivity(userSettings, offset.GyroscopeSen2X, sens.scope2x);
    sens.scope3x = ReadValidSensitivity(userSettings, offset.GyroscopeSen3X, sens.scope3x);
    sens.scope4x = ReadValidSensitivity(userSettings, offset.GyroscopeSen4X, sens.scope4x);
    sens.scope6x = ReadValidSensitivity(userSettings, offset.GyroscopeSen6X, sens.scope6x);
    sens.scope8x = ReadValidSensitivity(userSettings, offset.GyroscopeSen8X, sens.scope8x);
    sens.firingHipfire = ReadValidSensitivity(userSettings, offset.FireGyroscopeSenNoneSniper, sens.firingHipfire);
    sens.firingFP = ReadValidSensitivity(userSettings, offset.FireGyroscopeSenNoneSniperFP, sens.firingFP);
    sens.firingRedDot = ReadValidSensitivity(userSettings, offset.FireGyroscopeSenRedDot, sens.firingRedDot);
    sens.firing2x = ReadValidSensitivity(userSettings, offset.FireGyroscopeSen2X, sens.firing2x);
    sens.firing3x = ReadValidSensitivity(userSettings, offset.FireGyroscopeSen3X, sens.firing3x);
    sens.firing4x = ReadValidSensitivity(userSettings, offset.FireGyroscopeSen4X, sens.firing4x);
    sens.firing6x = ReadValidSensitivity(userSettings, offset.FireGyroscopeSen6X, sens.firing6x);
    sens.firing8x = ReadValidSensitivity(userSettings, offset.FireGyroscopeSen8X, sens.firing8x);
    sens.fpViewSwitch = GetDriverManager().read<uint8_t>(userSettings + offset.UserSettingsFPViewSwitch) != 0;
    return sens;
}

static bool IsLocalPlayerADS() {
    if (address.LocalPlayerActor == 0) return false;
    uint8_t adsByte = GetDriverManager().read<uint8_t>(address.LocalPlayerActor + offset.bIsGunADS);
    return (adsByte & 0x01) != 0;
}

static float ReadCameraFOV() {
    if (address.libUE4 == 0) return kDefaultFOV;

    uint64_t uworld = GetDriverManager().read<uint64_t>(address.libUE4 + offset.Gworld);
    if (uworld == 0) return kDefaultFOV;

    // UWorld → NetDriver → ServerConnection → PlayerController → PlayerCameraManager
    uint64_t netDriver = GetDriverManager().read<uint64_t>(uworld + offset.NetDriver);
    if (netDriver == 0) return kDefaultFOV;
    uint64_t serverConn = GetDriverManager().read<uint64_t>(netDriver + offset.ServerConnection);
    if (serverConn == 0) return kDefaultFOV;
    uint64_t playerController = GetDriverManager().read<uint64_t>(serverConn + offset.PlayerController);
    if (playerController == 0) return kDefaultFOV;
    uint64_t pcm = GetDriverManager().read<uint64_t>(playerController + offset.PlayerCameraManager);
    if (pcm == 0) return kDefaultFOV;

    float fov = GetDriverManager().read<float>(pcm + offset.CameraCache + offset.POV + offset.MinimalViewInfoFOV);
    if (fov <= 0.0f || fov > 180.0f) return kDefaultFOV;
    return fov;
}

static float SelectHipfireGyroSensitivity(const UserGyroSensitivity& sens, bool isFiring) {
    if (isFiring) {
        return sens.fpViewSwitch ? sens.firingFP : sens.firingHipfire;
    }
    return sens.fpViewSwitch ? sens.hipfireFP : sens.hipfire;
}

static float SelectScopedGyroSensitivity(const UserGyroSensitivity& sens, bool isFiring, float cameraFOV) {
    struct ScopeSensitivityEntry {
        float fov;
        float normalSensitivity;
        float firingSensitivity;
    };

    // ADS 状态下按当前 FOV 与各档位基准 FOV 的最近距离匹配。
    // 已知基准: NoneSniper 70, 2x 55, 3x 44.4, 4x 26.7, 6x 20~21, 8x 13.3/11。
    const ScopeSensitivityEntry entries[] = {
        {70.0f, sens.hipfire, sens.firingHipfire},
        {55.0f, sens.scope2x, sens.firing2x},
        {44.4f, sens.scope3x, sens.firing3x},
        {26.7f, sens.scope4x, sens.firing4x},
        {20.5f, sens.scope6x, sens.firing6x},
        {13.3f, sens.scope8x, sens.firing8x},
        {11.0f, sens.scope8x, sens.firing8x},
    };

    const size_t entryCount = sizeof(entries) / sizeof(entries[0]);
    const ScopeSensitivityEntry* bestEntry = &entries[0];
    float bestDelta = std::fabs(cameraFOV - entries[0].fov);
    for (size_t i = 1; i < entryCount; ++i) {
        const float delta = std::fabs(cameraFOV - entries[i].fov);
        if (delta < bestDelta) {
            bestDelta = delta;
            bestEntry = &entries[i];
        }
    }

    return isFiring ? bestEntry->firingSensitivity : bestEntry->normalSensitivity;
}

// 根据 ADS 状态 + 当前 FOV 判断档位，选择对应灵敏度
static float SelectGyroSensitivityScale(const UserGyroSensitivity& sens, bool isFiring, bool isADS, float cameraFOV) {
    float selected = isADS
        ? SelectScopedGyroSensitivity(sens, isFiring, cameraFOV)
        : SelectHipfireGyroSensitivity(sens, isFiring);

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
    if (actorAddr == 0) return Vec3::Zero();

    // 若目标在载具中，从载具的 ReplicatedMovement.LinearVelocity 读取速度
    // 载具是 Pawn 而非 Character，没有 CharacterMovementComponent
    uint64_t vehicle = GetDriverManager().read<uint64_t>(actorAddr + offset.CurrentVehicle);
    if (vehicle != 0) {
        Vec3 vel;
        GetDriverManager().read(vehicle + offset.ReplicatedMovement, &vel, sizeof(Vec3));
        return vel;
    }

    // 角色：从 CharacterMovementComponent.Velocity 读取
    uint64_t cmc = GetDriverManager().read<uint64_t>(actorAddr + offset.STCharacterMovement);
    if (cmc == 0) return Vec3::Zero();
    Vec3 vel;
    GetDriverManager().read(cmc + offset.MovementVelocity, &vel, sizeof(Vec3));
    return vel;
}

static float ReadBulletFireSpeed() {
    if (address.LocalPlayerActor == 0) return kDefaultBulletSpeed;
    uint64_t weapon = GetDriverManager().read<uint64_t>(address.LocalPlayerActor + offset.CurrentUsingWeaponSafety);
    if (weapon == 0) return kDefaultBulletSpeed;
    uint64_t entityComp = GetDriverManager().read<uint64_t>(weapon + offset.ShootWeaponEntityComp);
    if (entityComp == 0) return kDefaultBulletSpeed;
    float speed = GetDriverManager().read<float>(entityComp + offset.BulletFireSpeed);
    if (speed <= 0.0f) return kDefaultBulletSpeed;
    return speed;
}

static uint64_t ReadCurrentShootWeaponEntityComp() {
    if (address.LocalPlayerActor == 0) return 0;
    uint64_t weapon = GetDriverManager().read<uint64_t>(address.LocalPlayerActor + offset.CurrentUsingWeaponSafety);
    if (weapon == 0) return 0;
    return GetDriverManager().read<uint64_t>(weapon + offset.ShootWeaponEntityComp);
}

static const char* GetSightTypeLabel(uint8_t sightType) {
    switch (sightType) {
        case 0: return "None";
        case 1: return "Iron";
        case 2: return "Holo/RedDot";
        case 3: return "2x";
        case 4: return "3x";
        case 5: return "4x";
        case 6: return "6x";
        case 7: return "8x";
        case 8: return "Canted";
        default: return "Unknown";
    }
}

static FireStateDebugInfo ReadCurrentFireStateDebugInfo() {
    FireStateDebugInfo info;
    info.localActor = address.LocalPlayerActor;
    if (info.localActor == 0) return info;

    info.characterWeaponFiringRaw =
        GetDriverManager().read<uint8_t>(info.localActor + offset.bIsWeaponFiring);
    info.currentUsingWeapon =
        GetDriverManager().read<uint64_t>(info.localActor + offset.CurrentUsingWeaponSafety);
    info.curEquipWeapon =
        GetDriverManager().read<uint64_t>(info.localActor + offset.CurEquipWeapon);

    const uint64_t weapon =
        info.currentUsingWeapon != 0 ? info.currentUsingWeapon : info.curEquipWeapon;
    if (weapon == 0) return info;

    info.weaponWantsToFireRaw =
        GetDriverManager().read<uint8_t>(weapon + offset.WeaponBWantToFire);
    info.weaponWantsToFireCommonRaw =
        GetDriverManager().read<uint8_t>(weapon + offset.WeaponBWantToFireCommon);
    info.curShootWeaponState =
        GetDriverManager().read<uint8_t>(weapon + offset.CurShootWeaponState);
    return info;
}

static RecoilDebugInfo ReadCurrentRecoilDebugInfo() {
    RecoilDebugInfo info;
    if (address.LocalPlayerActor == 0) return info;

    const uint64_t weapon =
        GetDriverManager().read<uint64_t>(address.LocalPlayerActor + offset.CurrentUsingWeaponSafety);
    if (weapon == 0) return info;

    const uint64_t entityComp = GetDriverManager().read<uint64_t>(weapon + offset.ShootWeaponEntityComp);
    if (entityComp == 0) return info;

    LocalRecoilInfo recoil{};
    GetDriverManager().read(entityComp + offset.RecoilInfo, &recoil, sizeof(recoil));

    info.weapon = weapon;
    info.entityComp = entityComp;
    info.valid = true;
    info.sightType = GetDriverManager().read<uint8_t>(entityComp + offset.EntitySightType);
    info.angledSightID = GetDriverManager().read<int32_t>(weapon + offset.WeaponAngledSightID);
    info.curSightTypeID = GetDriverManager().read<int32_t>(weapon + offset.WeaponCurSightTypeID);
    info.curScopeID = GetDriverManager().read<int32_t>(weapon + offset.WeaponCurScopeID);
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

    info.accessoriesVRecoilFactor = GetDriverManager().read<float>(entityComp + offset.AccessoriesVRecoilFactor);
    info.accessoriesVRecoilFactorModifier = GetDriverManager().read<float>(entityComp + offset.AccessoriesVRecoilFactorModifier);
    info.verticalRecoilFactorModifier = GetDriverManager().read<float>(entityComp + offset.VerticalRecoilFactorModifier);
    info.accessoriesHRecoilFactor = GetDriverManager().read<float>(entityComp + offset.AccessoriesHRecoilFactor);
    info.accessoriesHRecoilFactorModifier = GetDriverManager().read<float>(entityComp + offset.AccessoriesHRecoilFactorModifier);
    info.horizontalRecoilFactorModifier = GetDriverManager().read<float>(entityComp + offset.HorizontalRecoilFactorModifier);
    info.accessoriesAllRecoilFactorModifier = GetDriverManager().read<float>(entityComp + offset.AccessoriesAllRecoilFactorModifier);
    info.accessoriesRecoveryFactor = GetDriverManager().read<float>(entityComp + offset.AccessoriesRecoveryFactor);

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
    if (actorAddr == 0) return Vec3::Zero();
    uint64_t rootComp = GetDriverManager().read<uint64_t>(actorAddr + offset.RootComponent);
    if (rootComp == 0) return Vec3::Zero();
    FTransform transform = GetDriverManager().read<FTransform>(rootComp + offset.ComponentToWorld);
    return transform.Translation;
}

static bool ReadFreshBoneWorldPos(const BoneScreenData& bsd, int boneID, Vec3& outWorldPos) {
    if (bsd.skelMeshCompAddr == 0 || bsd.boneDataPtr == 0 || bsd.cachedBoneCount <= 0)
        return false;
    if (boneID < 0 || boneID >= BONE_COUNT) return false;
    int cstIndex = bsd.boneMap[boneID];
    if (cstIndex < 0 || cstIndex >= bsd.cachedBoneCount) return false;

    FTransform meshTransform = GetDriverManager().read<FTransform>(
        bsd.skelMeshCompAddr + offset.ComponentToWorld);
    FTransform bone;
    if (!GetDriverManager().read(bsd.boneDataPtr + cstIndex * sizeof(FTransform),
                                 &bone, sizeof(FTransform)))
        return false;

    FMatrix m = TransformToMatrix(meshTransform);
    Vec3 b = bone.Translation;
    if (boneID == BONE_HEAD) b.Z += 7;
    outWorldPos = {
        m.M[0][0] * b.X + m.M[1][0] * b.Y + m.M[2][0] * b.Z + m.M[3][0],
        m.M[0][1] * b.X + m.M[1][1] * b.Y + m.M[2][1] * b.Z + m.M[3][1],
        m.M[0][2] * b.X + m.M[1][2] * b.Y + m.M[2][2] * b.Z + m.M[3][2]
    };
    return true;
}

static Vec3 ReadCameraWorldPos() {
    if (address.libUE4 == 0) return ReadActorRootWorldPos(address.LocalPlayerActor);

    uint64_t uworld = GetDriverManager().read<uint64_t>(address.libUE4 + offset.Gworld);
    if (uworld == 0) return ReadActorRootWorldPos(address.LocalPlayerActor);

    uint64_t netDriver = GetDriverManager().read<uint64_t>(uworld + offset.NetDriver);
    if (netDriver == 0) return ReadActorRootWorldPos(address.LocalPlayerActor);
    uint64_t serverConn = GetDriverManager().read<uint64_t>(netDriver + offset.ServerConnection);
    if (serverConn == 0) return ReadActorRootWorldPos(address.LocalPlayerActor);
    uint64_t playerController = GetDriverManager().read<uint64_t>(serverConn + offset.PlayerController);
    if (playerController == 0) return ReadActorRootWorldPos(address.LocalPlayerActor);
    uint64_t pcm = GetDriverManager().read<uint64_t>(playerController + offset.PlayerCameraManager);
    if (pcm == 0) return ReadActorRootWorldPos(address.LocalPlayerActor);

    Vec3 cameraWorldPos = GetDriverManager().read<Vec3>(pcm + offset.CameraCache + offset.POV);
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

bool ComputePredictedAimPoint(uint64_t actorAddr, float targetDistanceMeters,
                              const Vec2& currentTargetScreenPos, const FMatrix& vpMat,
                              float screenWidth, float screenHeight,
                              PredictedAimPoint& outPrediction) {
    outPrediction = {};
    outPrediction.currentScreenPos = currentTargetScreenPos;
    outPrediction.predictedScreenPos = currentTargetScreenPos;
    outPrediction.currentOnScreen = true;

    if (actorAddr == 0 || screenWidth <= 0.0f || screenHeight <= 0.0f) {
        return false;
    }

    outPrediction.targetVelocity = ReadActorVelocity(actorAddr);
    outPrediction.bulletSpeed = ReadBulletFireSpeed();
    if (outPrediction.bulletSpeed > 0.0f && targetDistanceMeters > 0.0f) {
        outPrediction.leadTime = std::clamp((targetDistanceMeters * 100.0f) / outPrediction.bulletSpeed,
                                            0.0f, 1.5f);
    }

    const Vec3 rootPos = ReadActorRootWorldPos(actorAddr);
    const Vec3 predictedRoot = {
        rootPos.X + outPrediction.targetVelocity.X * outPrediction.leadTime,
        rootPos.Y + outPrediction.targetVelocity.Y * outPrediction.leadTime,
        rootPos.Z + outPrediction.targetVelocity.Z * outPrediction.leadTime
    };

    Vec2 currentRootScreen;
    Vec2 predictedRootScreen;
    const bool w2sCurrent = WorldToScreen(rootPos, vpMat, screenWidth, screenHeight, currentRootScreen);
    const bool w2sPredicted = WorldToScreen(predictedRoot, vpMat, screenWidth, screenHeight, predictedRootScreen);
    outPrediction.currentOnScreen = w2sCurrent;
    outPrediction.predictedOnScreen = w2sCurrent && w2sPredicted;
    if (!outPrediction.predictedOnScreen) {
        return false;
    }

    outPrediction.predictedScreenPos = currentTargetScreenPos + (predictedRootScreen - currentRootScreen);
    return true;
}

AutoAimController::AutoAimController() {
    config.enabled = false;
    config.onlyWhenFiring = true;
    config.aimMode = AUTO_AIM_MODE_ASSIST;
    config.targetBone = BONE_HEAD;
    config.maxDistance = 200.0f;
    config.fovLimit = 30.0f;
    config.updateRate = 120.0f;
    config.KpX = 0.40f;
    config.KdX = 0.02f;
    config.KpY = 0.21f;
    config.KdY = 0.01f;
    config.outputScaleX = 1.00f;
    config.outputScaleY = 1.00f;
    config.humanizeNoise = false;
    config.noiseStrengthX = 0.18f;
    config.noiseStrengthY = 0.12f;
    config.noiseChangeRate = 5.0f;
    config.noiseSmoothing = 7.5f;
    config.noiseMicroJitter = 0.04f;
    config.magnetCaptureRadius = 0.055f;
    config.magnetReleaseRadius = 0.110f;
    config.magnetStrength = 0.42f;
    config.filterTeammates = true;
    config.visibilityCheck = false;
    config.hysteresisThreshold = 35.0f;
    config.drawDebug = false;

    ResetTarget();

    threadRunning_.store(true, std::memory_order_release);
    updateThread_ = std::thread(&AutoAimController::UpdateThreadFunc, this);
}

AutoAimController::~AutoAimController() {
    threadRunning_.store(false, std::memory_order_release);
    if (updateThread_.joinable()) {
        updateThread_.join();
    }
    Stop();
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
    targetState.recoilBaseLiftOffset = 0.0f;
    targetState.recoilKickOffset = 0.0f;
    targetState.noiseTime = 0.0f;
    targetState.noiseRetargetTimer = 0.0f;
    targetState.noiseCurrent = Vec2(0, 0);
    targetState.noiseTarget = Vec2(0, 0);
    targetState.debugNoise = Vec2(0, 0);
    targetState.magnetEngaged = false;
    targetState.debugMagnetDistance = 0.0f;
    targetState.valid = false;
    targetState.holt.initialized = false;
    targetState.holt.level = Vec2(0, 0);
    targetState.holt.trend = Vec2(0, 0);
}

void AutoAimController::Stop() {
    ResetTarget();
    ReleaseTriggerTouch();
    if (HasGyroOutputChannel()) {
        SendGyroOutput(0.0f, 0.0f);
    }
}

void AutoAimController::UpdateThreadFunc() {
    using namespace std::chrono;
    auto lastTime = steady_clock::now();

    while (threadRunning_.load(std::memory_order_acquire)) {
        const auto now = steady_clock::now();
        const float deltaTime = duration<float>(now - lastTime).count();
        lastTime = now;

        const float targetRate = std::clamp(config.updateRate, 30.0f, 500.0f);
        const float targetInterval = 1.0f / targetRate;

        Update(deltaTime);

        const auto afterUpdate = steady_clock::now();
        const float elapsed = duration<float>(afterUpdate - now).count();
        const float sleepTime = targetInterval - elapsed;

        if (sleepTime > 0.0f) {
            std::this_thread::sleep_for(duration<float>(sleepTime));
        }
    }
}

float AutoAimController::DistanceToScreenCenter(const Vec2& pos, const Vec2& center) const {
    float dx = pos.x - center.x;
    float dy = pos.y - center.y;
    return sqrtf(dx * dx + dy * dy);
}

bool AutoAimController::IsInFOV(const Vec2& pos, const Vec2& center) const {
    const float sw = displayWidth_.load(std::memory_order_relaxed);
    const float sh = displayHeight_.load(std::memory_order_relaxed);
    float distancePixels = DistanceToScreenCenter(pos, center);
    float screenDiagonal = sqrtf(sw * sw + sh * sh);
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

bool AutoAimController::SelectTarget(const Vec2& screenCenter, bool requireVisibility,
                                     uint64_t& outActorAddr, int& outBoneID, Vec2& outScreenPos) {
    const auto boneCache = GetBoneScreenCacheSnapshot();
    if (!boneCache || boneCache->empty()) return false;
    uint64_t currentFrame = ReadFrameCounter();
    const Vec3 cameraWorldPos = ReadCameraWorldPos();

    uint64_t bestActor = 0;
    int bestBone = -1;
    Vec2 bestPos;
    float bestDistance = FLT_MAX;

    for (const auto& pair : *boneCache) {
        const BoneScreenData& bsd = pair.second;
        if (!bsd.valid) continue;
        if (currentFrame != 0 && bsd.frameCounter != 0 && currentFrame > bsd.frameCounter + 2) continue;

        // 过滤本地玩家
        if (bsd.actorAddr == address.LocalPlayerActor) continue;
        if (address.LocalPlayerKey != 0 && bsd.playerKey == address.LocalPlayerKey) continue;

        // 过滤队友（可选）
        if (config.filterTeammates) {
            if (address.LocalPlayerTeamID >= 0 &&
                bsd.teamID >= 0 &&
                bsd.teamID == address.LocalPlayerTeamID) {
                continue;
            }
        }

        // 距离过滤
        if (bsd.distance > config.maxDistance) continue;

        int candidateBone = config.targetBone;
        auto boneUsable = [&](int boneID) -> bool {
            const bool needVisibility = requireVisibility && gUseDepthBufferVisibility;
            return bsd.onScreen[boneID] && (!needVisibility || bsd.visible[boneID]);
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

void AutoAimController::ReleaseTriggerTouch() {
    if (triggerTouchDown_ && HasTouchOutputChannel()) {
        GetDriverManager().touch_up(9);
    }
    triggerTouchDown_ = false;
}

void AutoAimController::UpdateTriggerBot(const Vec2& rawScreenCenter) {
    if (!config.triggerBotEnabled) {
        ReleaseTriggerTouch();
        return;
    }

    if (!HasTouchOutputChannel()) {
        ReleaseTriggerTouch();
        return;
    }

    if (!triggerTouchReady_) {
        triggerTouchReady_ = GetDriverManager().touch_init(&triggerTouchMaxX_, &triggerTouchMaxY_);
        if (!triggerTouchReady_) {
            ReleaseTriggerTouch();
            return;
        }
    }

    uint64_t targetActor = 0;
    int targetBone = -1;
    Vec2 targetScreenPos;
    if (!SelectTarget(rawScreenCenter, true, targetActor, targetBone, targetScreenPos)) {
        ReleaseTriggerTouch();
        return;
    }

    float targetDistance = 0.0f;
    BoneScreenData targetBoneDataCopy;
    bool foundBoneData = false;
    {
        const auto boneCache = GetBoneScreenCacheSnapshot();
        if (boneCache != nullptr) {
            auto it = boneCache->find(targetActor);
            if (it != boneCache->end()) {
                targetDistance = it->second.distance;
                targetBoneDataCopy = it->second;
                foundBoneData = true;
            }
        }
    }
    if (!foundBoneData) {
        ReleaseTriggerTouch();
        return;
    }
    const BoneScreenData* targetBoneData = &targetBoneDataCopy;

    const float SW = displayWidth_.load(std::memory_order_relaxed);
    const float SH = displayHeight_.load(std::memory_order_relaxed);
    FMatrix vpMat;
    GetDriverManager().read(address.Matrix, &vpMat, sizeof(FMatrix));

    static const int kTriggerBotHitScanBones[] = {
        BONE_HEAD, BONE_NECK, BONE_CHEST, BONE_PELVIS
    };
    bool anyHitScanSelected = false;
    bool shouldTrigger = false;
    for (int boneID : kTriggerBotHitScanBones) {
        if (!IsTriggerBotHitScanBoneEnabled(config, boneID)) continue;
        anyHitScanSelected = true;
        if (!targetBoneData->onScreen[boneID]) continue;
        if (gUseDepthBufferVisibility && !targetBoneData->visible[boneID]) continue;

        PredictedAimPoint prediction;
        Vec2 triggerJudgePos = targetBoneData->screenPos[boneID];
        if (ComputePredictedAimPoint(targetActor, targetDistance, targetBoneData->screenPos[boneID], vpMat,
                                     SW, SH, prediction)) {
            triggerJudgePos = prediction.predictedScreenPos;
        }
        if (DistanceToScreenCenter(triggerJudgePos, rawScreenCenter) <= config.triggerBotCenterRadius) {
            shouldTrigger = true;
            break;
        }
    }

    if (!anyHitScanSelected) {
        PredictedAimPoint prediction;
        Vec2 triggerJudgePos = targetScreenPos;
        if (ComputePredictedAimPoint(targetActor, targetDistance, targetScreenPos, vpMat,
                                     SW, SH, prediction)) {
            triggerJudgePos = prediction.predictedScreenPos;
        }
        shouldTrigger = DistanceToScreenCenter(triggerJudgePos, rawScreenCenter) <= config.triggerBotCenterRadius;
    }

    if (!shouldTrigger) {
        ReleaseTriggerTouch();
        return;
    }

    const float fireButtonScreenX =
        std::clamp(config.triggerBotFireButtonX, 0.0f, 1.0f) * std::max(SW, 1.0f);
    const float fireButtonScreenY =
        std::clamp(config.triggerBotFireButtonY, 0.0f, 1.0f) * std::max(SH, 1.0f);
    int touchX = 0;
    int touchY = 0;
    if (!MapScreenToTouch(fireButtonScreenX, fireButtonScreenY, touchX, touchY)) {
        ReleaseTriggerTouch();
        return;
    }

    const bool ok = triggerTouchDown_
        ? GetDriverManager().touch_move(9, touchX, touchY)
        : GetDriverManager().touch_down(9, touchX, touchY);
    if (ok) {
        triggerTouchDown_ = true;
    } else {
        ReleaseTriggerTouch();
    }
}

Vec2 AutoAimController::ComputePDOutput(const Vec2& aimPos, const Vec2& screenCenter,
                                         const Vec2& feedforward, float deltaTime, float fovScale) {
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

    // 检测本地玩家移动：读取速度，判断是否在移动
    Vec3 localVel = ReadActorVelocity(address.LocalPlayerActor);
    float localSpeed = std::sqrt(localVel.X * localVel.X + localVel.Y * localVel.Y + localVel.Z * localVel.Z);
    constexpr float kMovementThreshold = 100.0f;  // cm/s，低于此速度视为静止
    bool isLocalMoving = localSpeed > kMovementThreshold;

    // 近中心缩放只作用于 PD，防止近中心时位置修正过大产生抖动
    // 但自身移动时禁用缩放，让 PD 全力跟随
    if (!isLocalMoving) {
        // 开镜后同样的角误差会占据更大的屏幕比例。
        // 若仍使用固定屏幕阈值，减速区会在 ADS 时显得过大，导致追不上预判点。
        const float nearCenterThresholdScale = std::clamp(fovScale, 0.35f, 1.0f);
        const float nearCenterAssist = (1.0f - nearCenterThresholdScale) / (1.0f - 0.35f);
        const float horizontalThreshold = kHorizontalNearCenterThreshold * nearCenterThresholdScale;
        const float verticalThreshold = kVerticalNearCenterThreshold * nearCenterThresholdScale;
        const float horizontalMinScale =
            std::clamp(kHorizontalNearCenterMinScale + nearCenterAssist * 0.18f,
                       kHorizontalNearCenterMinScale, 0.68f);
        const float verticalMinScale =
            std::clamp(kVerticalNearCenterMinScale + nearCenterAssist * 0.18f,
                       kVerticalNearCenterMinScale, 0.68f);

        pd.x = ScaleAxisNearCenter(pd.x, std::fabs(normalizedError.x),
            horizontalThreshold, horizontalMinScale);
        pd.y = ScaleAxisNearCenter(pd.y, std::fabs(normalizedError.y),
            verticalThreshold, verticalMinScale);
    }

    // 小 FOV 下同样的角速度会投影成更大的屏幕速率，
    // PD 和前馈都需要按角度比例缩回去，否则会明显冲过预判点。
    pd.x *= fovScale;
    pd.y *= fovScale;

    Vec2 output;
    output.x = feedforward.x * kFeedforwardGain * fovScale + pd.x;
    output.y = feedforward.y * kFeedforwardGain * fovScale + pd.y;

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
        ImGui::Text("Aim Mode: %s", config.aimMode == AUTO_AIM_MODE_MAGNET ? "magnet" : "assist");
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
        ImGui::Text("FOV Scale: %.2f", targetState.debugFovScale);
        ImGui::Text("Gyro Sens: %.2f", targetState.debugGyroScale);
        ImGui::Text("Sens Compensate: %.2f", targetState.debugSensCompensate);
        ImGui::Text("Humanize Noise: %.2f, %.2f", targetState.debugNoise.x, targetState.debugNoise.y);
        ImGui::Text("Magnet: %s / Dist %.1f", targetState.magnetEngaged ? "engaged" : "idle",
            targetState.debugMagnetDistance);
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
        ImGui::Text("Weapon: 0x%llX", (unsigned long long)recoil.weapon);
        ImGui::Text("Weapon Entity: 0x%llX", (unsigned long long)recoil.entityComp);
        ImGui::Text("Valid: %s", recoil.valid ? "true" : "false");
        if (recoil.valid) {
            ImGui::Separator();
            ImGui::Text("Sight: %s", GetSightTypeLabel(recoil.sightType));
            ImGui::Text("SightType / CurSightTypeID: %u / %d",
                static_cast<unsigned>(recoil.sightType), recoil.curSightTypeID);
            ImGui::Text("CurScopeID / AngledSightID: %d / %d",
                recoil.curScopeID, recoil.angledSightID);
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

void AutoAimController::DrawFireStateDebugWindow() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f, 410.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));

    ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove;

    if (ImGui::Begin("##FireStateDebug", nullptr, windowFlags)) {
        const FireStateDebugInfo& fire = targetState.debugFireState;
        ImGui::Text("Fire State");
        ImGui::Separator();
        ImGui::Text("Local Actor: 0x%llX", (unsigned long long)fire.localActor);
        ImGui::Text("CurrentUsingWeapon: 0x%llX", (unsigned long long)fire.currentUsingWeapon);
        ImGui::Text("CurEquipWeapon: 0x%llX", (unsigned long long)fire.curEquipWeapon);
        ImGui::Text("STExtraCharacter::bIsWeaponFiring: %u (%s)",
            static_cast<unsigned int>(fire.characterWeaponFiringRaw),
            (fire.characterWeaponFiringRaw & 0x01) != 0 ? "true" : "false");
        ImGui::Text("STExtraShootWeapon::bWantsToFire: %u (%s)",
            static_cast<unsigned int>(fire.weaponWantsToFireRaw),
            (fire.weaponWantsToFireRaw & 0x01) != 0 ? "true" : "false");
        ImGui::Text("STExtraShootWeapon::bWantsToFireCommon: %u (%s)",
            static_cast<unsigned int>(fire.weaponWantsToFireCommonRaw),
            (fire.weaponWantsToFireCommonRaw & 0x01) != 0 ? "true" : "false");
        ImGui::Text("STExtraShootWeapon::CurShootWeaponState: %u",
            static_cast<unsigned int>(fire.curShootWeaponState));
    }
    ImGui::End();
}

bool AutoAimController::IsLocalPlayerFiring() {
    if (address.libUE4 == 0) return false;

    if (address.LocalPlayerActor == 0) return false;

    const uint8_t firingByte =
        GetDriverManager().read<uint8_t>(address.LocalPlayerActor + offset.bIsWeaponFiring);
    return (firingByte & 0x01) != 0;
}

void AutoAimController::Update(float deltaTime) {
    if (!config.enabled && !config.triggerBotEnabled) {
        Stop();
        return;
    }
    targetState.debugRecoil = ReadCurrentRecoilDebugInfo();
    targetState.debugFireState = ReadCurrentFireStateDebugInfo();

    const float SW = displayWidth_.load(std::memory_order_relaxed);
    const float SH = displayHeight_.load(std::memory_order_relaxed);
    Vec2 rawScreenCenter(SW / 2.0f, SH / 2.0f);
    UpdateTriggerBot(rawScreenCenter);

    if (!config.enabled) {
        return;
    }

    if (!HasGyroOutputChannel()) return;

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

    if (!isFiring) {
        targetState.lastOutput.y = 0.0f;
    }

    // 如果启用了"仅开火时自瞄"，检查开火状态
    if (config.onlyWhenFiring && !isFiring) {
        ClearRecoilState(targetState);
        ResetTarget();
        SendGyroOutput(0, 0);
        return;
    }

    const float fovRatio = cameraFOV / kDefaultFOV;
    const float fovScale = ComputeAngularFovScale(cameraFOV);
    const Vec2 recoilCenterOffset = UpdateRecoilCenterOffset(
        config, targetState, targetState.debugRecoil, isFiring, deltaTime, fovRatio, SH);
    Vec2 screenCenter = rawScreenCenter + recoilCenterOffset;
    targetState.debugRawScreenCenter = rawScreenCenter;
    targetState.debugEffectiveScreenCenter = screenCenter;
    targetState.debugRecoilCenterOffset = recoilCenterOffset;

    uint64_t targetActor;
    int targetBone;
    Vec2 targetScreenPos;

    if (SelectTarget(screenCenter, config.visibilityCheck, targetActor, targetBone, targetScreenPos)) {
        // 目标切换时重置 Holt 和平滑速度
        if (targetState.valid && targetState.actorAddr != targetActor) {
            targetState.holt.initialized = false;
            targetState.holt.level = Vec2(0, 0);
            targetState.holt.trend = Vec2(0, 0);
            targetState.smoothedTargetVelocity = Vec3::Zero();
        }

        FMatrix vpMat;
        GetDriverManager().read(address.Matrix, &vpMat, sizeof(FMatrix));
        float SW = displayWidth_.load(std::memory_order_relaxed);
        float SH = displayHeight_.load(std::memory_order_relaxed);

        // 查找目标距离，并用最新 VP 矩阵直接投影骨骼世界坐标（消除 cache 延迟导致的拉扯）
        float targetDistance = 0.0f;
        {
            const auto boneCache = GetBoneScreenCacheSnapshot();
            if (boneCache != nullptr) {
                auto it = boneCache->find(targetActor);
                if (it != boneCache->end()) {
                    targetDistance = it->second.distance;
                    Vec3 freshWorldPos;
                    Vec2 freshScreenPos;
                    if (ReadFreshBoneWorldPos(it->second, targetBone, freshWorldPos) &&
                        WorldToScreen(freshWorldPos, vpMat, SW, SH, freshScreenPos)) {
                        targetScreenPos = freshScreenPos;
                    }
                }
            }
        }
        PredictedAimPoint prediction;
        ComputePredictedAimPoint(targetActor, targetDistance, targetScreenPos, vpMat, SW, SH, prediction);
        Vec2 aimPos = prediction.predictedOnScreen ? prediction.predictedScreenPos : targetScreenPos;
        const float shortSide = std::max(1.0f, std::min(SW, SH));
        const float magnetCapturePixels =
            std::clamp(config.magnetCaptureRadius, 0.01f, 0.25f) * shortSide;
        const float magnetReleasePixels =
            std::max(magnetCapturePixels,
                     std::clamp(config.magnetReleaseRadius, 0.01f, 0.35f) * shortSide);
        const float magnetDistance = DistanceToScreenCenter(aimPos, screenCenter);
        targetState.debugMagnetDistance = magnetDistance;

        // 计算目标速度前馈：将目标世界速度投影到屏幕速率（归一化/秒）
        // 用小步长 deltaTime 投影，避免近距离透视非线性放大，然后除以 deltaTime 转为速率
        Vec2 feedforward(0, 0);
        if (deltaTime > 0.0001f) {
            // 对目标速度做指数平滑，防止变向时前馈突变导致脱靶
            constexpr float kVelSmoothAlpha = 8.0f;  // 平滑时间常数倒数（越大越快跟随）
            float alpha = std::clamp(kVelSmoothAlpha * deltaTime, 0.0f, 1.0f);
            const Vec3& rawVel = prediction.targetVelocity;
            Vec3& sv = targetState.smoothedTargetVelocity;
            sv.X += (rawVel.X - sv.X) * alpha;
            sv.Y += (rawVel.Y - sv.Y) * alpha;
            sv.Z += (rawVel.Z - sv.Z) * alpha;

            const Vec3 rootPos = ReadActorRootWorldPos(targetActor);
            Vec2 currentRootScreen;
            Vec3 rootPlusVel = {
                rootPos.X + sv.X * deltaTime,
                rootPos.Y + sv.Y * deltaTime,
                rootPos.Z + sv.Z * deltaTime
            };
            if (WorldToScreen(rootPos, vpMat, SW, SH, currentRootScreen)) {
                Vec2 futureScreen;
                if (WorldToScreen(rootPlusVel, vpMat, SW, SH, futureScreen)) {
                    Vec2 screenHalf(SW / 2.0f, SH / 2.0f);
                    feedforward.x = (futureScreen.x - currentRootScreen.x) / screenHalf.x / deltaTime;
                    feedforward.y = (futureScreen.y - currentRootScreen.y) / screenHalf.y / deltaTime;
                }
            }
        }

        // 存储 debug 信息
        targetState.debugBulletSpeed = prediction.bulletSpeed;
        targetState.debugLeadTime = prediction.leadTime;
        targetState.debugTargetVel = prediction.targetVelocity;
        targetState.debugHoltPredicted = aimPos;
        targetState.debugCameraFOV = cameraFOV;
        targetState.debugFovScale = fovScale;

        // l) 前馈 + PD 控制器
        Vec2 gyroAdjust(0, 0);
        if (config.aimMode == AUTO_AIM_MODE_MAGNET) {
            const bool sameTarget = targetState.valid && targetState.actorAddr == targetActor;
            if (targetState.magnetEngaged) {
                if (!sameTarget || magnetDistance > magnetReleasePixels) {
                    targetState.magnetEngaged = false;
                }
            }
            if (!targetState.magnetEngaged && magnetDistance <= magnetCapturePixels) {
                targetState.magnetEngaged = true;
            }
            if (targetState.magnetEngaged) {
                gyroAdjust = ComputePDOutput(aimPos, screenCenter, feedforward, deltaTime, fovScale);
                gyroAdjust *= std::clamp(config.magnetStrength, 0.0f, 1.0f);
            }
        } else {
            targetState.magnetEngaged = false;
            gyroAdjust = ComputePDOutput(aimPos, screenCenter, feedforward, deltaTime, fovScale);
        }

        // 灵敏度补偿：灵敏度越高，同样陀螺仪值转角越大，需要缩小输出
        // FOV 补偿已在 ComputePDOutput 内部对 PD 分量单独处理
        Vec2 finalAdjust = ClampGyroStrength(gyroAdjust * sensCompensate * kAutoAimBoost);
        finalAdjust.x *= config.outputScaleX;
        finalAdjust.y *= config.outputScaleY;
        finalAdjust += ApplyHumanizeNoise(config, targetState, finalAdjust, deltaTime);
        finalAdjust.x = ApplyGyroFloor(finalAdjust.x);
        finalAdjust.y = ApplyVerticalGyroFloor(finalAdjust.y);
        if (!isFiring && targetState.debugRecoilCenterOffset.y == 0.0f && std::fabs(finalAdjust.y) < 0.08f) {
            finalAdjust.y = 0.0f;
        }
        finalAdjust = ClampGyroStrength(finalAdjust);

        // 存储 pre-negation 值用于下一帧参考
        targetState.lastOutput = finalAdjust;

        const int displayOrientation = displayOrientation_.load(std::memory_order_relaxed);
        const Vec2 gyroOutput = MapScreenAdjustToGyroOutput(finalAdjust, displayOrientation);
        SendGyroOutput(gyroOutput.x, gyroOutput.y);

        targetState.actorAddr = targetActor;
        targetState.boneID = targetBone;
        targetState.lastScreenPos = targetScreenPos;
        targetState.valid = true;
        targetState.targetLostTimer = 0.0f;
    } else {
        constexpr float kGracePeriod = 0.08f;  // 80ms 宽限期，防止短暂遮挡导致抖动
        targetState.targetLostTimer += deltaTime;
        if (targetState.targetLostTimer >= kGracePeriod) {
            ResetTarget();
            SendGyroOutput(0, 0);
        }
        // 宽限期内保持上一帧输出，不发送新指令
    }
}

void AutoAimController::DrawDebug() {
    if (!config.drawDebug) return;
    ImGuiIO& io = ImGui::GetIO();
    Vec2 screenCenter(io.DisplaySize.x / 2.0f, io.DisplaySize.y / 2.0f);
    screenCenter = screenCenter + targetState.debugRecoilCenterOffset;
    DrawFireStateDebugWindow();
    DrawRecoilSpeedDebugWindow();
    if (targetState.valid) {
        DrawDebugVisuals(targetState.lastScreenPos, screenCenter);
    }
}
