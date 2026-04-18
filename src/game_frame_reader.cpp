#include "game_frame_reader.h"

#include "driver_manager.h"
#include "game_fps_monitor.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>

extern std::atomic<int> driver_stat;
extern Addresses address;
extern Offsets offset;
extern bool gUseCameraCacheVPMatrix;

namespace {

constexpr uintptr_t kMinimalViewInfoLocationOffset = 0x0;
constexpr uintptr_t kMinimalViewInfoRotationOffset = 0x18;
constexpr uintptr_t kMinimalViewInfoFOVOffset = 0x30;
constexpr uintptr_t kMinimalViewInfoAspectRatioOffset = 0x58;
constexpr uintptr_t kClientConnectionsArrayOffset = 0x90;

static uint64_t ReadLocalPlayerControllerFromWorld(uint64_t uworld) {
    if (uworld == 0) return 0;

    uint64_t netDriver = GetDriverManager().read<uint64_t>(uworld + offset.NetDriver);
    if (netDriver == 0) return 0;

    uint64_t connection = GetDriverManager().read<uint64_t>(netDriver + offset.ServerConnection);
    if (connection == 0) {
        uint64_t clientConnectionsArray = netDriver + kClientConnectionsArrayOffset;
        uint64_t clientConnectionsData = GetDriverManager().read<uint64_t>(clientConnectionsArray);
        int clientConnectionsCount = GetDriverManager().read<int>(clientConnectionsArray + 0x8);
        if (clientConnectionsData == 0 || clientConnectionsCount <= 0) {
            return 0;
        }
        connection = GetDriverManager().read<uint64_t>(clientConnectionsData);
    }

    if (connection == 0) return 0;
    return GetDriverManager().read<uint64_t>(connection + offset.PlayerController);
}

static bool BuildViewProjectionMatrixFromCameraCache(uint64_t uworld, FMatrix& outVP) {
    if (uworld == 0) return false;

    uint64_t playerController = ReadLocalPlayerControllerFromWorld(uworld);
    if (playerController == 0) return false;

    uint64_t playerCameraManager = GetDriverManager().read<uint64_t>(playerController + offset.PlayerCameraManager);
    if (playerCameraManager == 0) return false;

    uint64_t povAddr = playerCameraManager + offset.CameraCache + offset.POV;

    Vec3 location = GetDriverManager().read<Vec3>(povAddr + kMinimalViewInfoLocationOffset);
    FRotator rotation = GetDriverManager().read<FRotator>(povAddr + kMinimalViewInfoRotationOffset);
    float fov = GetDriverManager().read<float>(povAddr + kMinimalViewInfoFOVOffset);
    float aspectRatio = GetDriverManager().read<float>(povAddr + kMinimalViewInfoAspectRatioOffset);

    const bool validLocation = std::isfinite(location.X) && std::isfinite(location.Y) && std::isfinite(location.Z);
    const bool validRotation = std::isfinite(rotation.Pitch) && std::isfinite(rotation.Yaw) && std::isfinite(rotation.Roll);
    if (!validLocation || !validRotation) return false;

    if (!(fov > 1.0f && fov < 179.0f)) return false;

    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x > 1.0f && io.DisplaySize.y > 1.0f) {
        aspectRatio = io.DisplaySize.x / io.DisplaySize.y;
    }

    if (!(aspectRatio > 0.1f && aspectRatio < 5.0f)) {
        aspectRatio = 16.0f / 9.0f;
    }

    FMatrix view = BuildViewMatrix(location, rotation);
    FMatrix projection = BuildProjectionMatrix(fov, aspectRatio);
    outVP = MatrixMultiply(view, projection);
    return true;
}

}  // namespace

GameFrameData ReadGameData() {
    GameFrameData data;
    data.valid = false;
    data.gameDeltaTime = 0.0f;
    data.frameCounter = 0;

    if (driver_stat.load(std::memory_order_relaxed) <= 0) return data;
    if (!gUseCameraCacheVPMatrix && address.Matrix == 0) return data;

    const uint64_t uworld = GetDriverManager().read<uint64_t>(address.libUE4 + offset.Gworld);
    bool hasViewProjection = false;
    if (gUseCameraCacheVPMatrix) {
        hasViewProjection = BuildViewProjectionMatrixFromCameraCache(uworld, data.VPMat);
    } else if (address.Matrix != 0) {
        hasViewProjection = GetDriverManager().read(address.Matrix, &data.VPMat, sizeof(FMatrix));
    }
    if (!hasViewProjection) {
        return data;
    }

    data.localPlayerPos = Vec3::Zero();
    if (address.LocalPlayerActor != 0) {
        uint64_t localRootComponent = GetDriverManager().read<uint64_t>(
            address.LocalPlayerActor + offset.RootComponent);
        if (localRootComponent != 0) {
            FTransform localTransform = GetDriverManager().read<FTransform>(
                localRootComponent + offset.ComponentToWorld);
            data.localPlayerPos = localTransform.Translation;
        }
    }

    if (address.libUE4 != 0 && offset.FAppDeltaTimeGOT != 0) {
        uint64_t gotEntry = GetDriverManager().read<uint64_t>(address.libUE4 + offset.FAppDeltaTimeGOT);
        if (gotEntry != 0) {
            double deltaTime = GetDriverManager().read<double>(gotEntry);
            if (deltaTime > 0.0 && deltaTime < 1.0) {
                PushGameDeltaTime(deltaTime);
                data.gameDeltaTime = static_cast<float>(deltaTime);
            }
        }
    }

    if (address.libUE4 != 0 && offset.GFrameCounterGOT != 0) {
        uint64_t gotEntry = GetDriverManager().read<uint64_t>(address.libUE4 + offset.GFrameCounterGOT);
        if (gotEntry != 0) {
            data.frameCounter = GetDriverManager().read<uint64_t>(gotEntry);
        }
    }

    data.valid = true;
    return data;
}

uint64_t ReadFrameCounter() {
    if (driver_stat.load(std::memory_order_relaxed) <= 0) return 0;
    if (address.libUE4 == 0 || offset.GFrameCounterGOT == 0) return 0;
    uint64_t gotEntry = GetDriverManager().read<uint64_t>(address.libUE4 + offset.GFrameCounterGOT);
    if (gotEntry == 0) return 0;
    return GetDriverManager().read<uint64_t>(gotEntry);
}
