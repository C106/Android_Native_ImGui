#include "bullet_spread_monitor.h"

#include "HwBreakpointMgr4.h"
#include "cpu_affinity.h"
#include "driver_manager.h"
#include "read_mem.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <thread>
#include <unistd.h>
#include <mutex>

extern std::atomic<int> driver_stat;

namespace {

constexpr uintptr_t kBulletSpreadFuncOffset = 0x8087F88ULL;
constexpr uintptr_t kEngineLoopProbeOffset = 0xBFF6A08ULL;
constexpr uintptr_t kSpreadVecOffset = 0x26CULL;
constexpr uintptr_t kDeviationOffset = 0x70CULL;
constexpr uintptr_t kDeviationYawOffset = 0x710ULL;
constexpr uintptr_t kDeviationPitchOffset = 0x714ULL;
constexpr int kSampleIntervalMs = 200;
constexpr int kStartRetryDelayMs = 150;
constexpr const char* kDefaultAuthKey = "dce3771681d4c7a143d5d06b7d32548e";

void GetProcessTasks(int pid, std::vector<int>& vOutput) {
    char szTaskPath[256];
    snprintf(szTaskPath, sizeof(szTaskPath), "/proc/%d/task", pid);
    DIR* dir = opendir(szTaskPath);
    if (!dir) return;
    struct dirent* ptr;
    while ((ptr = readdir(dir)) != nullptr) {
        if (ptr->d_type != DT_DIR) continue;
        if (ptr->d_name[0] == '.') continue;
        if (strspn(ptr->d_name, "0123456789") != strlen(ptr->d_name)) continue;
        vOutput.push_back(atoi(ptr->d_name));
    }
    closedir(dir);
}

std::mutex gBulletSpreadStateMutex;
BulletSpreadDebugState gBulletSpreadState;
std::thread gBulletSpreadThread;
std::atomic<bool> gBulletSpreadThreadRunning{false};
std::atomic<bool> gBulletSpreadEnabled{false};
std::mutex gBulletSpreadConfigMutex;
int gBulletSpreadPid = 0;
uint64_t gBulletSpreadLibUE4 = 0;
BulletBreakpointTarget gBulletBreakpointTarget = BulletBreakpointTarget::BulletSpreadFuncEntry;

uintptr_t ResolveBreakpointOffset(BulletBreakpointTarget target) {
    switch (target) {
    case BulletBreakpointTarget::EngineLoopProbe:
        return kEngineLoopProbeOffset;
    case BulletBreakpointTarget::BulletSpreadFuncEntry:
    default:
        return kBulletSpreadFuncOffset;
    }
}

uint64_t GetMonotonicMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void UpdateState(const BulletSpreadDebugState& next) {
    std::lock_guard<std::mutex> lock(gBulletSpreadStateMutex);
    gBulletSpreadState = next;
}

void UpdateStateFlags(bool threadRunning, bool sessionOpen, bool breakpointArmed) {
    std::lock_guard<std::mutex> lock(gBulletSpreadStateMutex);
    gBulletSpreadState.threadRunning = threadRunning;
    gBulletSpreadState.sessionOpen = sessionOpen;
    gBulletSpreadState.breakpointArmed = breakpointArmed;
}

void UpdateStateError(int errorCode) {
    std::lock_guard<std::mutex> lock(gBulletSpreadStateMutex);
    gBulletSpreadState.lastError = errorCode;
}

void UpdateStateHit(const HW_HIT_ITEM& hitItem,
                    uintptr_t breakpointAddr,
                    uint64_t hitTotalCount,
                    uintptr_t thisPtr,
                    const Vec3& spread,
                    float deviation,
                    float deviationYaw,
                    float deviationPitch,
                    bool valid) {
    std::lock_guard<std::mutex> lock(gBulletSpreadStateMutex);
    gBulletSpreadState.valid = valid;
    gBulletSpreadState.tid = static_cast<int>(hitItem.task_id);
    gBulletSpreadState.hitCount++;
    gBulletSpreadState.hitTotalCount = hitTotalCount;
    gBulletSpreadState.lastHitMonotonicMs = GetMonotonicMs();
    gBulletSpreadState.breakpointAddr = breakpointAddr;
    gBulletSpreadState.pc = static_cast<uintptr_t>(hitItem.regs_info.pc);
    gBulletSpreadState.thisPtr = thisPtr;
    gBulletSpreadState.spread = spread;
    gBulletSpreadState.deviation = deviation;
    gBulletSpreadState.deviationYaw = deviationYaw;
    gBulletSpreadState.deviationPitch = deviationPitch;
    gBulletSpreadState.lastError = 0;
}

bool ReadSpreadState(uintptr_t thisPtr,
                     Vec3& spread,
                     float& deviation,
                     float& deviationYaw,
                     float& deviationPitch) {
    if (thisPtr < 0x10000ULL) {
        return false;
    }

    if (!GetDriverManager().read(thisPtr + kSpreadVecOffset, &spread, sizeof(spread))) {
        return false;
    }

    if (!GetDriverManager().read(thisPtr + kDeviationOffset, &deviation, sizeof(deviation))) {
        return false;
    }

    if (!GetDriverManager().read(thisPtr + kDeviationYawOffset, &deviationYaw, sizeof(deviationYaw))) {
        return false;
    }

    if (!GetDriverManager().read(thisPtr + kDeviationPitchOffset, &deviationPitch, sizeof(deviationPitch))) {
        return false;
    }

    return true;
}

void BulletSpreadThreadMain() {
    SetThreadAffinity(CoreTier::LITTLE);

    int pid = 0;
    uint64_t libUE4Base = 0;
    BulletBreakpointTarget target = BulletBreakpointTarget::BulletSpreadFuncEntry;
    {
        std::lock_guard<std::mutex> lock(gBulletSpreadConfigMutex);
        pid = gBulletSpreadPid;
        libUE4Base = gBulletSpreadLibUE4;
        target = gBulletBreakpointTarget;
    }

    BulletSpreadDebugState initial{};
    initial.requestedEnabled = gBulletSpreadEnabled.load(std::memory_order_acquire);
    initial.threadRunning = true;
    initial.pid = pid;
    initial.target = target;
    initial.breakpointAddr = libUE4Base ? libUE4Base + ResolveBreakpointOffset(target) : 0;
    UpdateState(initial);

    if (pid <= 0 || libUE4Base == 0) {
        UpdateStateError(EINVAL);
        UpdateStateFlags(false, false, false);
        return;
    }

    // 连接 CHwBreakpointMgr 驱动
    CHwBreakpointMgr driver;
    int connectErr = driver.ConnectDriver(kDefaultAuthKey);
    if (connectErr < 0) {
        UpdateStateError(-connectErr);
        UpdateStateFlags(false, false, false);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(gBulletSpreadStateMutex);
        gBulletSpreadState.driverConnected = true;
        gBulletSpreadState.sessionOpen = true;
    }

    const uintptr_t breakpointAddr = libUE4Base + ResolveBreakpointOffset(target);

    // 持续监控循环：对所有线程安装断点 → wait → suspend → read → uninstall → repeat
    while (gBulletSpreadThreadRunning.load(std::memory_order_acquire)) {
        // 枚举目标进程的所有 task（线程）
        std::vector<int> vTask;
        GetProcessTasks(pid, vTask);
        if (vTask.empty()) {
            UpdateStateError(ESRCH);
            std::this_thread::sleep_for(std::chrono::milliseconds(kStartRetryDelayMs));
            continue;
        }

        // 对每个线程安装硬件断点
        std::vector<uint64_t> vHwBpHandle;
        for (int taskId : vTask) {
            uint64_t hProcess = driver.OpenProcess(static_cast<uint64_t>(taskId));
            if (!hProcess) continue;

            uint64_t hwbpHandle = driver.InstProcessHwBp(
                hProcess, breakpointAddr, HW_BREAKPOINT_LEN_4, HW_BREAKPOINT_X);
            if (hwbpHandle) {
                vHwBpHandle.push_back(hwbpHandle);
            }
            driver.CloseHandle(hProcess);
        }

        if (vHwBpHandle.empty()) {
            UpdateStateError(ENODEV);
            UpdateStateFlags(true, true, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(kStartRetryDelayMs));
            continue;
        }

        UpdateStateFlags(true, true, true);

        // 等待采样间隔
        std::this_thread::sleep_for(std::chrono::milliseconds(kSampleIntervalMs));

        if (!gBulletSpreadThreadRunning.load(std::memory_order_acquire)) {
            for (uint64_t h : vHwBpHandle) driver.UninstProcessHwBp(h);
            break;
        }

        // 暂停所有断点
        for (uint64_t h : vHwBpHandle) {
            driver.SuspendProcessHwBp(h);
        }

        // 读取所有断点的命中信息，取最后一个命中项
        HW_HIT_ITEM bestHit{};
        uint64_t bestHitTotal = 0;
        bool hasHit = false;

        for (uint64_t h : vHwBpHandle) {
            uint64_t hitTotalCount = 0;
            std::vector<HW_HIT_ITEM> hitItems;
            BOOL readOk = driver.ReadHwBpInfo(h, hitTotalCount, hitItems);
            if (readOk && !hitItems.empty()) {
                bestHit = hitItems.back();
                bestHitTotal += hitTotalCount;
                hasHit = true;
            }
        }

        // 卸载所有断点
        for (uint64_t h : vHwBpHandle) {
            driver.UninstProcessHwBp(h);
        }

        if (hasHit) {
            uintptr_t thisPtr = static_cast<uintptr_t>(bestHit.regs_info.regs[0]);
            if (target == BulletBreakpointTarget::BulletSpreadFuncEntry && thisPtr < 0x10000ULL) {
                thisPtr = static_cast<uintptr_t>(bestHit.regs_info.regs[19]);
            }

            Vec3 spread = Vec3::Zero();
            float deviation = 0.0f;
            float deviationYaw = 0.0f;
            float deviationPitch = 0.0f;
            bool valid = static_cast<uintptr_t>(bestHit.regs_info.pc) == breakpointAddr
                      || static_cast<uintptr_t>(bestHit.hit_addr) == breakpointAddr;
            if (valid && target == BulletBreakpointTarget::BulletSpreadFuncEntry) {
                valid = ReadSpreadState(thisPtr, spread, deviation, deviationYaw, deviationPitch);
            }
            UpdateStateHit(bestHit, breakpointAddr, bestHitTotal,
                           thisPtr, spread, deviation, deviationYaw, deviationPitch, valid);
        }
    }

    driver.DisconnectDriver();
    gBulletSpreadThreadRunning.store(false, std::memory_order_release);
    UpdateStateFlags(false, false, false);
    {
        std::lock_guard<std::mutex> lock(gBulletSpreadStateMutex);
        gBulletSpreadState.driverConnected = false;
    }
}

void StartBulletSpreadThreadIfNeeded() {
    if (!gBulletSpreadEnabled.load(std::memory_order_acquire)) {
        return;
    }

    if (gBulletSpreadThreadRunning.load(std::memory_order_acquire)) {
        return;
    }

    int pid = 0;
    uint64_t libUE4Base = 0;
    BulletBreakpointTarget target = BulletBreakpointTarget::BulletSpreadFuncEntry;
    {
        std::lock_guard<std::mutex> lock(gBulletSpreadConfigMutex);
        pid = gBulletSpreadPid;
        libUE4Base = gBulletSpreadLibUE4;
        target = gBulletBreakpointTarget;
    }

    if (pid <= 0 || libUE4Base == 0) {
        BulletSpreadDebugState state = GetBulletSpreadDebugState();
        state.requestedEnabled = true;
        state.pid = pid;
        state.target = target;
        state.breakpointAddr = libUE4Base ? libUE4Base + ResolveBreakpointOffset(target) : 0;
        state.lastError = EINVAL;
        UpdateState(state);
        return;
    }

    gBulletSpreadThreadRunning.store(true, std::memory_order_release);
    gBulletSpreadThread = std::thread(BulletSpreadThreadMain);
}

void StopBulletSpreadThread() {
    gBulletSpreadThreadRunning.store(false, std::memory_order_release);
    if (gBulletSpreadThread.joinable()) {
        gBulletSpreadThread.join();
    }
}

}  // namespace

void ConfigureBulletSpreadMonitor(int pid, uint64_t libUE4Base) {
    {
        std::lock_guard<std::mutex> lock(gBulletSpreadConfigMutex);
        gBulletSpreadPid = pid;
        gBulletSpreadLibUE4 = libUE4Base;
    }

    {
        std::lock_guard<std::mutex> lock(gBulletSpreadStateMutex);
        gBulletSpreadState.pid = pid;
        gBulletSpreadState.target = gBulletBreakpointTarget;
        gBulletSpreadState.breakpointAddr = libUE4Base ? libUE4Base + ResolveBreakpointOffset(gBulletBreakpointTarget) : 0;
    }

    if (gBulletSpreadEnabled.load(std::memory_order_acquire)) {
        StopBulletSpreadThread();
        StartBulletSpreadThreadIfNeeded();
    }
}

void SetBulletSpreadMonitorEnabled(bool enabled) {
    gBulletSpreadEnabled.store(enabled, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(gBulletSpreadStateMutex);
        gBulletSpreadState.requestedEnabled = enabled;
    }

    if (enabled) {
        StartBulletSpreadThreadIfNeeded();
    } else {
        StopBulletSpreadThread();
        std::lock_guard<std::mutex> lock(gBulletSpreadStateMutex);
        gBulletSpreadState.threadRunning = false;
        gBulletSpreadState.sessionOpen = false;
        gBulletSpreadState.breakpointArmed = false;
    }
}

void SetBulletBreakpointTarget(BulletBreakpointTarget target) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(gBulletSpreadConfigMutex);
        if (gBulletBreakpointTarget == target) {
            return;
        }
        gBulletBreakpointTarget = target;
        changed = true;
    }

    {
        std::lock_guard<std::mutex> lock(gBulletSpreadStateMutex);
        gBulletSpreadState.target = target;
        gBulletSpreadState.breakpointAddr = gBulletSpreadLibUE4
            ? gBulletSpreadLibUE4 + ResolveBreakpointOffset(target)
            : 0;
    }

    if (changed && gBulletSpreadEnabled.load(std::memory_order_acquire)) {
        StopBulletSpreadThread();
        StartBulletSpreadThreadIfNeeded();
    }
}


BulletSpreadDebugState GetBulletSpreadDebugState() {
    std::lock_guard<std::mutex> lock(gBulletSpreadStateMutex);
    return gBulletSpreadState;
}

void ShutdownBulletSpreadMonitor() {
    gBulletSpreadEnabled.store(false, std::memory_order_release);
    StopBulletSpreadThread();
    std::lock_guard<std::mutex> lock(gBulletSpreadStateMutex);
    gBulletSpreadState.requestedEnabled = false;
    gBulletSpreadState.threadRunning = false;
    gBulletSpreadState.sessionOpen = false;
    gBulletSpreadState.breakpointArmed = false;
}
