#ifndef KERNEL_H
#define KERNEL_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <linux/types.h>
#include <asm/ptrace.h>
#include <errno.h>


#define OP_CMD_READ 601
#define OP_CMD_WRITE 602
#define OP_CMD_BASE 603
#define OP_CMD_GETPID 604
#define OP_CMD_HIDE_PROCESS 605
#define OP_CMD_UN_HOOK 606
#define OP_CMD_RECOVER_PROCESS 607
#define OP_HW_BREAKPOINT_CTL 608
#define OP_HW_BREAKPOINT_GET_HITS 609






typedef struct _COPY_MEMORY {
    pid_t pid;
    uintptr_t addr;
    void* buffer;
    size_t size;
} COPY_MEMORY, *PCOPY_MEMORY;

typedef struct _MODULE_BASE {
    pid_t pid;
    char* name;
    uintptr_t base;
    short index;
} MODULE_BASE, *PMODULE_BASE;



enum HW_BREAKPOINT_ACTION {
    HW_BP_ADD = 1,
    HW_BP_REMOVE = 2,
};

enum HW_BREAKPOINT_TYPE {
    HW_BP_TYPE_EXECUTE = 0,
    HW_BP_TYPE_WRITE   = 1,
    HW_BP_TYPE_RW      = 2,
};


typedef struct _HW_BREAKPOINT_CTL {
    pid_t           pid;
    uintptr_t       addr;
    int             action;
    int             type;
    int             len;
} HW_BREAKPOINT_CTL, *PHW_BREAKPOINT_CTL;

typedef struct _HW_BREAKPOINT_HIT_INFO {
    pid_t           pid;
    uint64_t        timestamp;
    uintptr_t       addr;
    struct user_pt_regs regs;
} HW_BREAKPOINT_HIT_INFO, *PHW_BREAKPOINT_HIT_INFO;

typedef struct _HW_BREAKPOINT_GET_HITS_CTL {
    uintptr_t       buffer;
    size_t          count;
} HW_BREAKPOINT_GET_HITS_CTL, *PHW_BREAKPOINT_GET_HITS_CTL;



class c_driver {
private:
    int fd;
    pid_t pid;

public:
    c_driver() {

    }

    ~c_driver() {
        if (fd > 0)
            close(fd);
    }

    void initialize(pid_t pid) {
        this->pid = pid;
    }

   
    bool read(uintptr_t addr, void *buffer, size_t size) {
        COPY_MEMORY cm;
        cm.pid = this->pid;
        cm.addr = addr;
        cm.buffer = buffer;
        cm.size = size;
        return ioctl(fd, OP_CMD_READ, &cm) == 0;
    }

   
    bool write(uintptr_t addr, void *buffer, size_t size) {
        COPY_MEMORY cm;
        cm.pid = this->pid;
        cm.addr = addr;
        cm.buffer = buffer;
        cm.size = size;
        return ioctl(fd, OP_CMD_WRITE, &cm) == 0;
    }
    
    template <typename T>
    T read(uintptr_t addr) {
        T res;
        if (this->read(addr, &res, sizeof(T)))
            return res;
        return {};
    }

    template <typename T>
    bool write(uintptr_t addr, T value) {
        return this->write(addr, &value, sizeof(T));
    }

  
    uintptr_t get_module_base(char* module_name, short index = 0) {
        MODULE_BASE mb;
        mb.pid = this->pid;
        mb.name = module_name;
        mb.index = index;
        mb.base = 0;
        
        if (ioctl(fd, OP_CMD_BASE, &mb) == 0) {
            return mb.base;
        }
        return 0;
    }

    void hide_process() { 
        ioctl(fd, OP_CMD_HIDE_PROCESS); 
    }
  
    void recover_process() {
        ioctl(fd, OP_CMD_RECOVER_PROCESS);
    }


    

int add_breakpoint(uintptr_t addr, int type, int len) {
    HW_BREAKPOINT_CTL req;
    req.action = HW_BP_ADD;
    req.pid = this->pid;
    req.addr = addr;
    req.len = len;
    req.type = type;

    if (ioctl(fd, OP_HW_BREAKPOINT_CTL, &req) == 0) {
        return 0;
    }
    return errno;
}


    
    bool remove_breakpoint(uintptr_t addr) {
        HW_BREAKPOINT_CTL req;
        req.action = HW_BP_REMOVE;
        req.pid = this->pid;
        req.addr = addr;
        req.len = 0;
        req.type = 0;

        return ioctl(fd, OP_HW_BREAKPOINT_CTL, &req) == 0;
    }

  
    // buffer: 用户态分配的 HW_BREAKPOINT_HIT_INFO 数组
    // max_count: 数组大小
    // 返回值: 实际获取到的数量，-1 表示失败
        int get_breakpoint_hits(HW_BREAKPOINT_HIT_INFO *buffer, size_t max_count) {
        if (!buffer || max_count == 0) return 0;
        
        HW_BREAKPOINT_GET_HITS_CTL ctl;
        ctl.count = max_count;
        
        ctl.buffer = (uintptr_t)buffer; 

        // 注意：ioctl 成功返回 0，但我们需要返回 count
        // 这里假设驱动在成功时会把实际数量写回 ctl.count
        int ret = ioctl(fd, OP_HW_BREAKPOINT_GET_HITS, &ctl);
        if (ret >= 0) {
             // 如果内核返回的是处理的条数，直接返回 ret
             return ret; 
        }
        return -1;
    }
    
};


#endif
