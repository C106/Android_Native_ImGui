#include "bullet_spread_monitor.h"

#include "arm64_hwbp_debugger_compat.h"
#include "cpu_affinity.h"
#include "driver_manager.h"
#include "read_mem.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
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
constexpr int kPollTimeoutMs = 250;
constexpr int kStartRetryDelayMs = 150;

class Arm64HwbpSession {
public:
    bool Start(int pid, uintptr_t addr, int& errorOut) {
        Stop();

        fd_ = open("/dev/arm64_hwbp_debugger", O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            errorOut = errno;
            return false;
        }

        arm64_hwbp_request req{};
        req.pid = pid;
        req.addr = static_cast<uint64_t>(addr);
        if (ioctl(fd_, ARM64_HWBP_IOC_SET_BREAKPOINT, &req) == 0) {
            errorOut = 0;
            return true;
        }

        errorOut = errno;

        close(fd_);
        fd_ = -1;
        return false;
    }

    void Stop() {
        if (fd_ >= 0) {
            ioctl(fd_, ARM64_HWBP_IOC_CLEAR_BREAKPOINT);
            close(fd_);
            fd_ = -1;
        }
    }

    bool IsOpen() const {
        return fd_ >= 0;
    }

    bool WaitAndRead(arm64_hwbp_regs_snapshot& snapshot, int& errorOut) {
        if (fd_ < 0) {
            errorOut = ENODEV;
            return false;
        }

        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN | POLLRDNORM;

        const int pollRet = poll(&pfd, 1, kPollTimeoutMs);
        if (pollRet == 0) {
            errorOut = 0;
            return false;
        }
        if (pollRet < 0) {
            errorOut = (errno == EINTR) ? 0 : errno;
            return false;
        }
        if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
            errorOut = EIO;
            return false;
        }

        if ((pfd.revents & (POLLIN | POLLRDNORM)) == 0) {
            errorOut = 0;
            return false;
        }

        ssize_t n = read(fd_, &snapshot, sizeof(snapshot));
        if (n == static_cast<ssize_t>(sizeof(snapshot))) {
            errorOut = 0;
            return true;
        }
        if (n < 0 && errno == EAGAIN) {
            errorOut = 0;
            return false;
        }

        errorOut = (n < 0) ? errno : EIO;
        return false;
    }

    bool GetStatus(arm64_hwbp_status& status, int& errorOut) {
        if (fd_ < 0) {
            errorOut = ENODEV;
            return false;
        }

        if (ioctl(fd_, ARM64_HWBP_IOC_GET_STATUS, &status) == 0) {
            errorOut = 0;
            return true;
        }

        errorOut = errno;
        return false;
    }

private:
    int fd_ = -1;
};

std::mutex gBulletSpreadStateMutex;
BulletSpreadDebugState gBulletSpreadState;
std::thread gBulletSpreadThread;
std::atomic<bool> gBulletSpreadThreadRunning{false};
std::atomic<bool> gBulletSpreadEnabled{false};
std::atomic<bool> gBulletSpreadStatusRequest{false};
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
    if (!sessionOpen) {
        gBulletSpreadState.statusValid = false;
        gBulletSpreadState.driverStatusAddr = 0;
        gBulletSpreadState.driverStatusHitCount = 0;
        gBulletSpreadState.driverStatusThreadCount = 0;
        gBulletSpreadState.driverStatusFlags = 0;
    }
}

void UpdateStateError(int errorCode) {
    std::lock_guard<std::mutex> lock(gBulletSpreadStateMutex);
    if (errorCode == EAGAIN) {
        gBulletSpreadState.lastError = 0;
        return;
    }
    gBulletSpreadState.lastError = errorCode;
}

void UpdateStateDriverStatus(const arm64_hwbp_status& status, int errorCode) {
    std::lock_guard<std::mutex> lock(gBulletSpreadStateMutex);
    gBulletSpreadState.lastStatusMonotonicMs = GetMonotonicMs();
    gBulletSpreadState.lastStatusError = errorCode;
    gBulletSpreadState.statusValid = (errorCode == 0);
    if (errorCode != 0) {
        return;
    }

    gBulletSpreadState.driverStatusAddr = static_cast<uintptr_t>(status.addr);
    gBulletSpreadState.driverStatusHitCount = status.hit_count;
    gBulletSpreadState.driverStatusThreadCount = status.thread_count;
    gBulletSpreadState.driverStatusFlags = status.flags;
}

void UpdateStateHit(const arm64_hwbp_regs_snapshot& snap,
                    uintptr_t thisPtr,
                    const Vec3& spread,
                    float deviation,
                    float deviationYaw,
                    float deviationPitch,
                    bool valid) {
    std::lock_guard<std::mutex> lock(gBulletSpreadStateMutex);
    gBulletSpreadState.valid = valid;
    gBulletSpreadState.pid = snap.tgid;
    gBulletSpreadState.tid = snap.tid;
    gBulletSpreadState.hitCount = snap.hit_count;
    gBulletSpreadState.lastHitMonotonicMs = GetMonotonicMs();
    gBulletSpreadState.breakpointAddr = static_cast<uintptr_t>(snap.bp_addr);
    gBulletSpreadState.pc = static_cast<uintptr_t>(snap.pc);
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
    if (!Paradise_hook || thisPtr < 0x10000ULL) {
        return false;
    }

    if (!Paradise_hook->read(thisPtr + kSpreadVecOffset, &spread, sizeof(spread))) {
        return false;
    }

    if (!Paradise_hook->read(thisPtr + kDeviationOffset, &deviation, sizeof(deviation))) {
        return false;
    }

    if (!Paradise_hook->read(thisPtr + kDeviationYawOffset, &deviationYaw, sizeof(deviationYaw))) {
        return false;
    }

    if (!Paradise_hook->read(thisPtr + kDeviationPitchOffset, &deviationPitch, sizeof(deviationPitch))) {
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

    Arm64HwbpSession session;
    int sessionError = 0;
    const uintptr_t breakpointAddr = libUE4Base + ResolveBreakpointOffset(target);
    if (!session.Start(pid, breakpointAddr, sessionError)) {
        while (gBulletSpreadThreadRunning.load(std::memory_order_acquire)) {
            UpdateStateFlags(true, false, false);
            UpdateStateError(sessionError);
            if (sessionError != EAGAIN && sessionError != EBUSY) {
                gBulletSpreadThreadRunning.store(false, std::memory_order_release);
                UpdateStateFlags(false, false, false);
                return;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(kStartRetryDelayMs));
            if (session.Start(pid, breakpointAddr, sessionError)) {
                break;
            }
        }

        if (!gBulletSpreadThreadRunning.load(std::memory_order_acquire)) {
            UpdateStateFlags(false, false, false);
            return;
        }
    }

    UpdateStateFlags(true, true, true);
    gBulletSpreadStatusRequest.store(true, std::memory_order_release);

    while (gBulletSpreadThreadRunning.load(std::memory_order_acquire)) {
        if (gBulletSpreadStatusRequest.exchange(false, std::memory_order_acq_rel)) {
            arm64_hwbp_status status{};
            int statusError = 0;
            if (session.GetStatus(status, statusError)) {
                UpdateStateDriverStatus(status, 0);
            } else {
                UpdateStateDriverStatus(status, statusError);
            }
        }

        arm64_hwbp_regs_snapshot snap{};
        int waitError = 0;
        const bool hasSnapshot = session.WaitAndRead(snap, waitError);
        if (!gBulletSpreadThreadRunning.load(std::memory_order_acquire)) {
            break;
        }

        if (!hasSnapshot) {
            if (waitError != 0) {
                UpdateStateError(waitError);
            }
            continue;
        }

        uintptr_t thisPtr = static_cast<uintptr_t>(snap.regs[0]);
        if (target == BulletBreakpointTarget::BulletSpreadFuncEntry && thisPtr < 0x10000ULL) {
            thisPtr = static_cast<uintptr_t>(snap.regs[19]);
        }
        Vec3 spread = Vec3::Zero();
        float deviation = 0.0f;
        float deviationYaw = 0.0f;
        float deviationPitch = 0.0f;
        bool valid = static_cast<uintptr_t>(snap.pc) == breakpointAddr;
        if (valid && target == BulletBreakpointTarget::BulletSpreadFuncEntry) {
            valid = ReadSpreadState(thisPtr, spread, deviation, deviationYaw, deviationPitch);
        }
        UpdateStateHit(snap, thisPtr, spread, deviation, deviationYaw, deviationPitch, valid);
        gBulletSpreadStatusRequest.store(true, std::memory_order_release);
    }

    session.Stop();
    gBulletSpreadThreadRunning.store(false, std::memory_order_release);
    UpdateStateFlags(false, false, false);
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

void RequestBulletSpreadDriverStatus() {
    gBulletSpreadStatusRequest.store(true, std::memory_order_release);
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
