#pragma once

#include "kernel.h"
#include "driver.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

enum DriverType { DRIVER_RT_HOOK = 0, DRIVER_PARADISE = 1 };

struct DriverSharedState {
    std::atomic<int> desiredType{DRIVER_RT_HOOK};
    std::atomic<pid_t> targetPid{0};
    std::atomic<uint64_t> generation{0};
};

inline DriverSharedState& GetDriverSharedState() {
    static DriverSharedState state;
    return state;
}

// 全局单例 driver 实例，所有线程共享
inline c_driver& GetGlobalRtHook() {
    static c_driver instance;
    return instance;
}

inline paradise_driver& GetGlobalParadiseHook() {
    static paradise_driver instance;
    static std::mutex initMutex;
    return instance;
}

inline std::mutex& GetParadiseMutex() {
    static std::mutex mutex;
    return mutex;
}

class DriverManager {
    DriverType currentType_;
    uint64_t appliedGeneration_;
    pid_t initializedPid_;

    void initializeCurrentDriver(pid_t pid) {
        if (pid <= 0 || initializedPid_ == pid) {
            return;
        }

        if (currentType_ == DRIVER_RT_HOOK) {
            GetGlobalRtHook().initialize(pid);
        } else {
            std::lock_guard<std::mutex> lock(GetParadiseMutex());
            GetGlobalParadiseHook().initialize(pid);
        }
        initializedPid_ = pid;
    }

    void applySharedState() {
        DriverSharedState& shared = GetDriverSharedState();
        const DriverType desiredType = static_cast<DriverType>(
            shared.desiredType.load(std::memory_order_acquire));
        const uint64_t generation = shared.generation.load(std::memory_order_acquire);

        if (desiredType != currentType_) {
            currentType_ = desiredType;
            initializedPid_ = 0;
        }

        initializeCurrentDriver(shared.targetPid.load(std::memory_order_acquire));
        appliedGeneration_ = generation;
    }

    void ensureSynchronized() {
        DriverSharedState& shared = GetDriverSharedState();
        const uint64_t generation = shared.generation.load(std::memory_order_acquire);
        const DriverType desiredType = static_cast<DriverType>(
            shared.desiredType.load(std::memory_order_acquire));

        if (generation != appliedGeneration_ || desiredType != currentType_) {
            applySharedState();
        }
    }

    void publishTargetPid(pid_t pid) {
        if (pid <= 0) {
            return;
        }

        DriverSharedState& shared = GetDriverSharedState();
        shared.targetPid.store(pid, std::memory_order_release);
        shared.generation.fetch_add(1, std::memory_order_acq_rel);
    }

public:
    DriverManager()
        : currentType_(static_cast<DriverType>(GetDriverSharedState().desiredType.load(std::memory_order_acquire))),
          appliedGeneration_(GetDriverSharedState().generation.load(std::memory_order_acquire)),
          initializedPid_(GetDriverSharedState().targetPid.load(std::memory_order_acquire)) {
        applySharedState();
    }

    DriverType getType() {
        ensureSynchronized();
        return currentType_;
    }

    DriverType getDesiredType() const {
        return static_cast<DriverType>(
            GetDriverSharedState().desiredType.load(std::memory_order_acquire));
    }

    void switchDriver(DriverType type) {
        DriverSharedState& shared = GetDriverSharedState();
        const DriverType currentDesired = static_cast<DriverType>(
            shared.desiredType.load(std::memory_order_acquire));
        if (currentDesired == type) {
            return;
        }

        shared.desiredType.store(static_cast<int>(type), std::memory_order_release);
        shared.generation.fetch_add(1, std::memory_order_acq_rel);
        ensureSynchronized();
    }

    void initialize(pid_t pid) {
        publishTargetPid(pid);
        initializeCurrentDriver(pid);
        appliedGeneration_ = GetDriverSharedState().generation.load(std::memory_order_acquire);
    }

    pid_t get_pid(const char* name) {
        ensureSynchronized();

        pid_t pid = 0;
        if (currentType_ == DRIVER_PARADISE) {
            std::lock_guard<std::mutex> lock(GetParadiseMutex());
            pid = GetGlobalParadiseHook().get_pid(name);
            if (pid > 0) {
                GetGlobalParadiseHook().initialize(pid);
                initializedPid_ = pid;
                publishTargetPid(pid);
                appliedGeneration_ = GetDriverSharedState().generation.load(std::memory_order_acquire);
            }
        } else {
            char cmd[0x100] = "pidof ";
            std::strcat(cmd, name);
            FILE* fp = popen(cmd, "r");
            if (!fp) {
                return -1;
            }
            std::fscanf(fp, "%d", &pid);
            pclose(fp);
            if (pid > 0) {
                GetGlobalRtHook().initialize(pid);
                initializedPid_ = pid;
                publishTargetPid(pid);
                appliedGeneration_ = GetDriverSharedState().generation.load(std::memory_order_acquire);
            }
        }
        return pid;
    }

    bool read(uintptr_t addr, void* buf, size_t size) {
        ensureSynchronized();
        if (addr == 0 || addr < 0x10000 || addr >= 0x800000000000ULL || buf == nullptr || size == 0) {
            return false;
        }
        if (currentType_ == DRIVER_RT_HOOK) {
            return GetGlobalRtHook().read(addr, buf, size);
        }
        std::lock_guard<std::mutex> lock(GetParadiseMutex());
        return GetGlobalParadiseHook().read(addr, buf, size);
    }

    bool read_safe(uintptr_t addr, void* buf, size_t size) {
        return read(addr, buf, size);
    }

    template<typename T> T read(uintptr_t addr) {
        T res{};
        read(addr, &res, sizeof(T));
        return res;
    }

    bool write(uintptr_t addr, void* buf, size_t size) {
        ensureSynchronized();
        if (addr == 0 || addr < 0x10000 || addr >= 0x800000000000ULL || buf == nullptr || size == 0) {
            return false;
        }
        if (currentType_ == DRIVER_RT_HOOK) {
            return GetGlobalRtHook().write(addr, buf, size);
        }
        std::lock_guard<std::mutex> lock(GetParadiseMutex());
        return GetGlobalParadiseHook().write(addr, buf, size);
    }

    template<typename T> bool write(uintptr_t addr, T value) {
        return write(addr, &value, sizeof(T));
    }

    uintptr_t get_module_base(const char* name) {
        ensureSynchronized();
        if (currentType_ == DRIVER_RT_HOOK) {
            return GetGlobalRtHook().get_module_base(const_cast<char*>(name), 0);
        }
        std::lock_guard<std::mutex> lock(GetParadiseMutex());
        return GetGlobalParadiseHook().get_module_base(name);
    }

    bool gyro_update(float x, float y, uint32_t mask = PARADISE_GYRO_MASK_ALL, bool enable = true) {
        ensureSynchronized();
        if (currentType_ != DRIVER_PARADISE) return false;
        std::lock_guard<std::mutex> lock(GetParadiseMutex());
        return GetGlobalParadiseHook().gyro_update(x, y, mask, enable);
    }

    bool touch_init(int* max_x, int* max_y) {
        ensureSynchronized();
        if (currentType_ != DRIVER_PARADISE) return false;
        std::lock_guard<std::mutex> lock(GetParadiseMutex());
        return GetGlobalParadiseHook().touch_init(max_x, max_y);
    }

    bool touch_down(int slot, int x, int y) {
        ensureSynchronized();
        if (currentType_ != DRIVER_PARADISE) return false;
        std::lock_guard<std::mutex> lock(GetParadiseMutex());
        return GetGlobalParadiseHook().touch_down(slot, x, y);
    }

    bool touch_move(int slot, int x, int y) {
        ensureSynchronized();
        if (currentType_ != DRIVER_PARADISE) return false;
        std::lock_guard<std::mutex> lock(GetParadiseMutex());
        return GetGlobalParadiseHook().touch_move(slot, x, y);
    }

    bool touch_up(int slot) {
        ensureSynchronized();
        if (currentType_ != DRIVER_PARADISE) return false;
        std::lock_guard<std::mutex> lock(GetParadiseMutex());
        return GetGlobalParadiseHook().touch_up(slot);
    }

    bool touch_destroy() {
        ensureSynchronized();
        if (currentType_ != DRIVER_PARADISE) return false;
        std::lock_guard<std::mutex> lock(GetParadiseMutex());
        return GetGlobalParadiseHook().touch_destroy();
    }

    bool supports_touch() {
        ensureSynchronized();
        return currentType_ == DRIVER_PARADISE;
    }

    int add_breakpoint(uintptr_t addr, int type, int len) {
        ensureSynchronized();
        if (currentType_ != DRIVER_RT_HOOK) {
            return ENOTSUP;
        }
        return GetGlobalRtHook().add_breakpoint(addr, type, len);
    }

    bool remove_breakpoint(uintptr_t addr) {
        ensureSynchronized();
        if (currentType_ != DRIVER_RT_HOOK) {
            return false;
        }
        return GetGlobalRtHook().remove_breakpoint(addr);
    }

    int get_breakpoint_hits(HW_BREAKPOINT_HIT_INFO* buffer, size_t max_count) {
        ensureSynchronized();
        if (currentType_ != DRIVER_RT_HOOK) {
            return -1;
        }
        return GetGlobalRtHook().get_breakpoint_hits(buffer, max_count);
    }
};

inline DriverManager& GetDriverManager() {
    thread_local DriverManager manager;
    return manager;
}
