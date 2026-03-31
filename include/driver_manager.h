#pragma once
#include "kernel.h"
#include "driver.h"
#include <mutex>

enum DriverType { DRIVER_RT_HOOK = 0, DRIVER_PARADISE = 1 };

class DriverManager {
    c_driver* rtHook;
    Paradise_hook_driver* paradiseHook;
    DriverType currentType;
    std::mutex paradiseLock;  // Paradise Hook 全局互斥锁（严格单线程访问）

public:
    DriverManager() : rtHook(new c_driver()), paradiseHook(nullptr),
                      currentType(DRIVER_RT_HOOK) {}
    ~DriverManager() { delete rtHook; delete paradiseHook; }

    DriverType getType() const { return currentType; }

    void switchDriver(DriverType type) {
        if (type == currentType) return;
        currentType = type;
        if (type == DRIVER_PARADISE) {
            if (!paradiseHook) {
                paradiseHook = new Paradise_hook_driver();
            }
        }
    }

    void initialize(pid_t pid) {
        if (currentType == DRIVER_RT_HOOK) {
            rtHook->initialize(pid);
        } else {
            if (!paradiseHook) {
                paradiseHook = new Paradise_hook_driver();
            }
            paradiseHook->initialize(pid);
        }
    }

    pid_t get_pid(const char* name) {
        pid_t pid = 0;
        if (currentType == DRIVER_PARADISE) {
            // Paradise Hook: 全局加锁
            std::lock_guard<std::mutex> lock(paradiseLock);
            if (!paradiseHook) {
                paradiseHook = new Paradise_hook_driver();
            }
            pid = paradiseHook->get_pid(name);
            // 在锁内调用 initialize，避免重复加锁
            if (pid > 0) {
                paradiseHook->initialize(pid);
            }
        } else {
            // RT Hook: use pidof command (same as old free function)
            char cmd[0x100] = "pidof ";
            strcat(cmd, name);
            FILE* fp = popen(cmd, "r");
            if (!fp) return -1;
            fscanf(fp, "%d", &pid);
            pclose(fp);
            if (pid > 0) {
                rtHook->initialize(pid);
            }
        }
        return pid;
    }
    bool read(uintptr_t addr, void* buf, size_t size) {
        // 地址验证：排除明显无效的地址
        if (addr == 0 || addr < 0x10000 || addr >= 0x800000000000ULL || buf == nullptr || size == 0) {
            return false;
        }

        if (currentType == DRIVER_RT_HOOK) return rtHook->read(addr, buf, size);

        // Paradise Hook: 全局加锁，严格单线程访问
        std::lock_guard<std::mutex> lock(paradiseLock);
        if (!paradiseHook) return false;
        return paradiseHook->read(addr, buf, size);
    }

    bool read_safe(uintptr_t addr, void* buf, size_t size) {
        // 地址验证：排除明显无效的地址
        if (addr == 0 || addr < 0x10000 || addr >= 0x800000000000ULL || buf == nullptr || size == 0) {
            return false;
        }

        if (currentType == DRIVER_RT_HOOK) return rtHook->read(addr, buf, size);

        // Paradise Hook: 全局加锁，严格单线程访问
        std::lock_guard<std::mutex> lock(paradiseLock);
        if (!paradiseHook) return false;
        return paradiseHook->read_safe(addr, buf, size);
    }

    template<typename T> T read(uintptr_t addr) {
        T res{};
        read(addr, &res, sizeof(T));
        return res;
    }

    bool write(uintptr_t addr, void* buf, size_t size) {
        // 地址验证：排除明显无效的地址
        if (addr == 0 || addr < 0x10000 || addr >= 0x800000000000ULL || buf == nullptr || size == 0) {
            return false;
        }

        if (currentType == DRIVER_RT_HOOK) return rtHook->write(addr, buf, size);

        // Paradise Hook: 全局加锁，严格单线程访问
        std::lock_guard<std::mutex> lock(paradiseLock);
        if (!paradiseHook) return false;
        return paradiseHook->write(addr, buf, size);
    }

    template<typename T> bool write(uintptr_t addr, T value) {
        return write(addr, &value, sizeof(T));
    }

    uintptr_t get_module_base(const char* name) {
        if (currentType == DRIVER_RT_HOOK)
            return rtHook->get_module_base(const_cast<char*>(name), 0);

        // Paradise Hook: 全局加锁，严格单线程访问
        std::lock_guard<std::mutex> lock(paradiseLock);
        if (!paradiseHook) return 0;
        return paradiseHook->get_module_base(name);
    }

    bool gyro_update(float x, float y, uint32_t mask = 0, bool enable = true) {
        if (currentType == DRIVER_PARADISE && paradiseHook)
            return paradiseHook->gyro_update(x, y, mask, enable);
        return false;
    }

    int add_breakpoint(uintptr_t addr, int type, int len) {
        if (currentType != DRIVER_RT_HOOK) return ENOTSUP;
        return rtHook->add_breakpoint(addr, type, len);
    }

    bool remove_breakpoint(uintptr_t addr) {
        if (currentType != DRIVER_RT_HOOK) return false;
        return rtHook->remove_breakpoint(addr);
    }

    int get_breakpoint_hits(HW_BREAKPOINT_HIT_INFO* buffer, size_t max_count) {
        if (currentType != DRIVER_RT_HOOK) return -1;
        return rtHook->get_breakpoint_hits(buffer, max_count);
    }
};

inline DriverManager* Paradise_hook = new DriverManager();
