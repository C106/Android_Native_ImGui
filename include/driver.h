#ifndef PARADISE_DRIVER_USER_H
#define PARADISE_DRIVER_USER_H

#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <stdint.h>
#include <stddef.h>
#include <mutex>

// define __NR_reboot if not defined
#ifndef __NR_reboot
#if defined(__aarch64__) || defined(__arm__)
#define __NR_reboot 142
#elif defined(__x86_64__)
#define __NR_reboot 169
#elif defined(__i386__)
#define __NR_reboot 88
#elif defined(SYS_reboot)
#define __NR_reboot SYS_reboot
#else
#error "__NR_reboot not defined and cannot be determined for this architecture"
#endif
#endif

// Magic numbers for reboot syscall to install fd
#define PARADISE_INSTALL_MAGIC1 0xDEADBEEF
#define PARADISE_INSTALL_MAGIC2 0xF00DCAFE

// Driver name in /proc/self/fd
#define PARADISE_DRIVER_NAME "[paradise_driver]"

// IOCTL command structures
struct paradise_get_pid_cmd
{
    pid_t pid;
    char name[256];
};

struct paradise_get_module_base_cmd
{
    pid_t pid;
    char name[256];
    uintptr_t base;
    int vm_flag;
};

struct paradise_memory_cmd
{
    pid_t pid;
    uintptr_t src_va;
    uintptr_t dst_va;
    size_t size;
    uintptr_t phy_addr;
};

struct paradise_memory_fast_cmd
{
    pid_t pid;
    uintptr_t src_va;
    uintptr_t dst_va;
    size_t size;
    uintptr_t phy_addr;
    int prot;
};

struct paradise_gyro_config_cmd
{
    int enable;
    uint32_t type_mask;
    float x;
    float y;
};

#define PARADISE_GYRO_MASK_GYRO (1u << 0)
#define PARADISE_GYRO_MASK_UNCAL (1u << 1)
#define PARADISE_GYRO_MASK_ALL (PARADISE_GYRO_MASK_GYRO | PARADISE_GYRO_MASK_UNCAL)

// IOCTL commands
#define PARADISE_IOCTL_GET_PID _IOWR('W', 11, struct paradise_get_pid_cmd)                               // 查找进程
#define PARADISE_IOCTL_GET_MODULE_BASE _IOWR('W', 10, struct paradise_get_module_base_cmd)               // 获取模块基地址
#define PARADISE_IOCTL_READ_MEMORY _IOWR('W', 9, struct paradise_memory_cmd)                             // 硬件读
#define PARADISE_IOCTL_WRITE_MEMORY _IOWR('W', 12, struct paradise_memory_cmd)                           // 硬件写
#define PARADISE_IOCTL_READ_MEMORY_FAST _IOWR('W', 16, struct paradise_memory_fast_cmd)            // 内核映射读
#define PARADISE_IOCTL_WRITE_MEMORY_FAST _IOWR('W', 17, struct paradise_memory_fast_cmd)           // 内核映射写
#define PARADISE_IOCTL_GYRO_CONFIG _IOWR('W', 21, struct paradise_gyro_config_cmd)                       // 陀螺仪

class Paradise_hook_driver
{
private:
    pid_t pid;
    int fd;

    std::mutex driver_lock;

    int scan_driver_fd()
    {
        DIR *dir = opendir("/proc/self/fd");
        if (!dir)
        {
            return -1;
        }

        struct dirent *entry;
        char link_path[256];
        char target[256];
        ssize_t len;

        while ((entry = readdir(dir)) != NULL)
        {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            {
                continue;
            }

            int fd_num = atoi(entry->d_name);
            if (fd_num < 0)
            {
                continue;
            }

            snprintf(link_path, sizeof(link_path), "/proc/self/fd/%d", fd_num);
            len = readlink(link_path, target, sizeof(target) - 1);
            if (len < 0)
            {
                continue;
            }
            target[len] = '\0';

            // Check if this is our driver
            if (strstr(target, PARADISE_DRIVER_NAME) != NULL)
            {
                closedir(dir);
                return fd_num;
            }
        }

        closedir(dir);
        return -1;
    }

    int install_driver_fd()
    {
        int fd = -1;

        long ret = syscall(__NR_reboot, PARADISE_INSTALL_MAGIC1, PARADISE_INSTALL_MAGIC2, 0, &fd);

        if (fd < 0)
        {
            printf("install_driver_fd: fd not installed, ret=%ld, errno=%d\n", ret, errno);
            return -1;
        }

        printf("install_driver_fd: fd=%d installed successfully\n", fd);
        return fd;
    }

    int get_driver_fd()
    {
        int fd = scan_driver_fd();
        if (fd >= 0)
        {
            return fd;
        }

        fd = install_driver_fd();
        if (fd >= 0)
        {
            return fd;
        }

        return -1;
    }

public:
    Paradise_hook_driver()
    {
        fd = get_driver_fd();

        if (fd < 0)
        {
            printf("无法找到驱动\n");
            exit(1);
        }

        printf("识别到Paradise Driver | fd %d\n", fd);
    }

    ~Paradise_hook_driver()
    {
        if (fd >= 0)
        {
            close(fd);
            fd = -1;
        }
    }

    pid_t get_pid(const char *name)
    {
        if (fd < 0)
        {
            return false;
        }

        struct paradise_get_pid_cmd cmd = {0, ""};

        strncpy(cmd.name, name, sizeof(cmd.name) - 1);

        if (ioctl(fd, PARADISE_IOCTL_GET_PID, &cmd) != 0)
        {
            return 0;
        }

        return cmd.pid;
    }

    void initialize(pid_t target_pid)
    {
        this->pid = target_pid;
    }

    uintptr_t get_module_base(const char *name)
    {
        if (this->pid <= 0)
        {
            return 0;
        }

        struct paradise_get_module_base_cmd cmd = {this->pid, "", 0, 0};

        strncpy(cmd.name, name, sizeof(cmd.name) - 1);
        if (ioctl(fd, PARADISE_IOCTL_GET_MODULE_BASE, &cmd) != 0)
            return 0;
        return cmd.base;
    }

    bool gyro_update(float x, float y, uint32_t type_mask = PARADISE_GYRO_MASK_ALL, bool enable = true)
    {
        if (fd < 0)
        {
            return false;
        }

        struct paradise_gyro_config_cmd cmd{};

        cmd.enable = enable ? 1 : 0;
        cmd.type_mask = type_mask ? type_mask : PARADISE_GYRO_MASK_ALL;
        cmd.x = x;
        cmd.y = y;
        return ioctl(fd, PARADISE_IOCTL_GYRO_CONFIG, &cmd) == 0;
    }

    bool read(uintptr_t addr, void *buffer, size_t size)
    {
        if (fd < 0)
        {
            return false;
        }

        if (buffer == nullptr || this->pid <= 0)
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(this->driver_lock);

        struct paradise_memory_fast_cmd cmd = {};

        cmd.pid = this->pid;
        cmd.src_va = addr;
        cmd.dst_va = (uintptr_t)buffer;
        cmd.size = size;
        cmd.prot = 0;

        return ioctl(fd, PARADISE_IOCTL_READ_MEMORY_FAST, &cmd) == 0;
    }

    template <typename T>
    T read(uintptr_t addr)
    {
        T res{};
        if (this->read(addr, &res, sizeof(T)))
            return res;
        return {};
    }

    bool write(uintptr_t addr, void *buffer, size_t size)
    {
        if (fd < 0)
        {
            return false;
        }

        if (buffer == nullptr || this->pid <= 0)
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(this->driver_lock);

        struct paradise_memory_fast_cmd cmd = {};

        cmd.pid = this->pid;
        cmd.src_va = addr;
        cmd.dst_va = (uintptr_t)buffer;
        cmd.size = size;
        cmd.prot = 0;

        return ioctl(fd, PARADISE_IOCTL_WRITE_MEMORY_FAST, &cmd) == 0;
    }

    template <typename T>
    T write(uintptr_t addr, T value)
    {
        return this->write(addr, &value, sizeof(T));
    }

    bool read_safe(uintptr_t addr, void *buffer, size_t size)
    {
        if (fd < 0)
        {
            return false;
        }

        if (buffer == nullptr || this->pid <= 0)
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(this->driver_lock);

        struct paradise_memory_cmd cmd = {};

        cmd.pid = this->pid;
        cmd.src_va = addr;
        cmd.dst_va = (uintptr_t)buffer;
        cmd.size = size;

        return ioctl(fd, PARADISE_IOCTL_READ_MEMORY, &cmd) == 0;
    }

    template <typename T>
    T read_safe(uintptr_t addr)
    {
        T res{};
        if (this->read_safe(addr, &res, sizeof(T)))
            return res;
        return {};
    }

    bool write_safe(uintptr_t addr, void *buffer, size_t size)
    {
        if (fd < 0)
        {
            return false;
        }

        if (buffer == nullptr || this->pid <= 0)
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(this->driver_lock);

        struct paradise_memory_cmd cmd = {};

        cmd.pid = this->pid;
        cmd.src_va = addr;
        cmd.dst_va = (uintptr_t)buffer;
        cmd.size = size;

        return ioctl(fd, PARADISE_IOCTL_WRITE_MEMORY, &cmd) == 0;
    }

    template <typename T>
    T write_safe(uintptr_t addr, T value)
    {
        return this->write_safe(addr, &value, sizeof(T));
    }
};

 // 全局声明，初始化为空指针

#endif // PARADISE_DRIVER_USER_H