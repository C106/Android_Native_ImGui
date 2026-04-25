#ifndef PARADISE_API_H
#define PARADISE_API_H

#include <stdint.h>
#include <sys/types.h>
#include <stddef.h>

#define PARADISE_GYRO_MASK_GYRO (1u << 0)
#define PARADISE_GYRO_MASK_UNCAL (1u << 1)
#define PARADISE_GYRO_MASK_ALL (PARADISE_GYRO_MASK_GYRO | PARADISE_GYRO_MASK_UNCAL)

class paradise_driver {
private:
    pid_t pid;
    int fd;

    int install_driver_fd();
    void ensure_connected();

public:
    // 构造时连接 Paradise 驱动
    paradise_driver();
    
    // 构析方法
    ~paradise_driver();

    // 初始化目标 pid，读写前务必调用一次
    void initialize(pid_t target_pid);
    
    // 获取进程pid，传入进程名称，从内核层安全获取pid
    pid_t get_pid(const char *name);
    
    // 获取模块基址，传入模块名，从内核层安全获取模块基址
    uintptr_t get_module_base(const char *name);

    // 获取模块映射范围 [base, end)，end 为最高一段 VMA 的 vm_end，便于一次覆盖整个 so，如 libc 多段
    bool get_module_range(const char *name, uintptr_t *base_out, uintptr_t *end_out);

    // 获取模块结束地址，传入模块名，从内核层安全获取模块结束地址
    uintptr_t get_module_end(const char *name);

    /*
    usage:
        uintptr_t lo, hi;
        if (get_module_range("libc.so", &lo, &hi)) {
            // 映射包络为 [lo, hi)，按需分段 read
        }
        // 或仅要结束地址：
        uintptr_t end = get_module_end("libc.so");
    */
    
    // 更新陀螺仪数据
    bool gyro_update(float x, float y, uint32_t type_mask = PARADISE_GYRO_MASK_ALL, bool enable = true);
    
    // 检查进程是否存活 (alive_out: 1为存活，0为未存活)
    bool is_process_alive(pid_t check_pid, int *alive_out);
    
    // 隐藏或取消隐藏指定进程
    bool hide_process(pid_t target_pid, bool hide);
    
    // 隐藏或取消隐藏指定路径
    bool hide_path(const char *path, bool hide);
    
    // 获取进程列表位图
    bool list_processes(uint8_t *bitmap, size_t bitmap_size, size_t *process_count_out);
    
    // 内核层读取数据，传入地址、接收指针、类型大小
    bool read(uintptr_t addr, void *buffer, size_t size);
    
    // 内核层修改数据，传入地址、数据指针、类型大小
    bool write(uintptr_t addr, void *buffer, size_t size);

    // 初始化触摸注入，自动查找触摸设备
    bool touch_init(int *max_x, int *max_y);

    // 手指按下
    bool touch_down(int slot, int x, int y);

    // 手指移动
    bool touch_move(int slot, int x, int y);

    // 手指抬起
    bool touch_up(int slot);

    // 销毁触摸注入
    bool touch_destroy();

    // 模板方法，传入地址，返回地址上的值
    template <typename T>
    T read(uintptr_t addr)
    {
        T res{};
        if (this->read(addr, &res, sizeof(T)))
            return res;
        return {};
    }

    // 模板方法，传入地址，修改后的值
    template <typename T>
    bool write(uintptr_t addr, T value)
    {
        return this->write(addr, &value, sizeof(T));
    }
};

#endif
