#pragma once

#include "mem_struct.h"
#include <cstdint>

enum class BulletBreakpointTarget : int {
    BulletSpreadFuncEntry = 0,
    EngineLoopProbe = 1,
};

struct BulletSpreadDebugState {
    bool requestedEnabled = false;
    bool threadRunning = false;
    bool sessionOpen = false;
    bool breakpointArmed = false;
    bool valid = false;
    bool statusValid = false;
    int pid = 0;
    int tid = 0;
    int lastError = 0;
    int lastStatusError = 0;
    uint64_t hitCount = 0;
    uint64_t driverStatusHitCount = 0;
    uint64_t lastHitMonotonicMs = 0;
    uint64_t lastStatusMonotonicMs = 0;
    uintptr_t breakpointAddr = 0;
    uintptr_t pc = 0;
    uintptr_t thisPtr = 0;
    uintptr_t driverStatusAddr = 0;
    uint32_t driverStatusThreadCount = 0;
    uint32_t driverStatusFlags = 0;
    Vec3 spread = Vec3::Zero();
    float deviation = 0.0f;
    float deviationYaw = 0.0f;
    float deviationPitch = 0.0f;
    BulletBreakpointTarget target = BulletBreakpointTarget::BulletSpreadFuncEntry;
};

void ConfigureBulletSpreadMonitor(int pid, uint64_t libUE4Base);
void SetBulletSpreadMonitorEnabled(bool enabled);
void SetBulletBreakpointTarget(BulletBreakpointTarget target);
BulletSpreadDebugState GetBulletSpreadDebugState();
void RequestBulletSpreadDriverStatus();
void ShutdownBulletSpreadMonitor();
