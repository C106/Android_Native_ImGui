#ifndef KERNEL_H
#define KERNEL_H

#include <iostream>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <sys/ioctl.h>
#include <vector>
#include <string>
#include <regex>
#include <sstream>



// 内存类型 标志
enum MEMTYPR {
    MEMREAD = 1UL << 1, //读
    MEMWRITE = 1UL << 2, //写
    MEMSYSTEM = 1UL << 3, //使用系统API (读/写)
    MEMASM = 1UL << 4, //使用汇编指令 (读写)
    MEMWRITEUSER = 1UL << 5, //(写) 只写用户页面自身可写的，不加默认全部可写
    MEMCPU = 1UL << 7, //获取经过CPU的地址 (读写)
    MEMUNINSTALL = 1UL << 9, //释放内存
};

// 其他 标志
enum OTHERTYPR {
    PROCESSPID = 1UL << 10, //获取PID
    MODULEBASE = 1UL << 11, //获取模块
    HIDEPID = 1UL << 12, //隐藏进程
    UNHIDEPID = 1UL << 13, //恢复隐藏的进程
    HIDEEVENT = 1UL << 14, //抹除创建的触摸驱动
    UNHIDEEVENT = 1UL << 15, //恢复抹除的触摸驱动
};

//标志结合 预设组合
#define __NR_syscall_ 18 //调用号
#define __FLAGS 1UL << 0 //标志
#define __CHECKSUCCESS (1UL << 30)
enum {
    DRIVERSUCCESS = 616,
};

#define __READSYSTEM (__FLAGS | MEMREAD | MEMSYSTEM) //读使用系统API
#define __READASM (__FLAGS | MEMREAD | MEMASM) //读使用汇编
#define __WRITESYSTEM (__FLAGS | MEMWRITE | MEMSYSTEM) //写使用系统API
#define __WRITEASM (__FLAGS | MEMWRITE | MEMASM) //写使用汇编
#define __MEMUNINSTALL (__FLAGS | MEMUNINSTALL) //释放内存

struct Ditpro_uct_base {
    pid_t pid;
    const char* name;
    uint64_t base;
};

struct Ditpro_uct_kpm
{
	int32_t pid;
	uint64_t addr;
	void *buffer;
	uint32_t size;
}__attribute__((aligned(8)));

struct Dit_Device_Uct {
	char name[32];
	unsigned long addr;
};


class c_driver
{
private:
	
	pid_t pid = -1;
	int fd;
	
public:
    float *mode = nullptr;
	bool *cpuaddr = nullptr;
	
	int DriverInput = -1;
	void start_drivers(int Continue)
	{
		fd = -1;
		printf("[0] ditPro (1.0*) - kpm\n");
		printf("[1] ditPro (1.0*) - ko\n");	
		
		printf("\n");
		printf("请选择 (0~1):");
		if(Continue <= 1)
		    scanf("%d", &DriverInput);
			
		if (DriverInput == 1)
		{
			int check = (__FLAGS | __CHECKSUCCESS);
			if(syscall(__NR_syscall_, &check) == DRIVERSUCCESS){
			    int va = DRIVERSUCCESS;   
                fd = syscall(__NR_syscall_, &va);
	            if (fd < 0){
		            printf("未刷入驱动程序或者不成功");
		            exit(1);
	            }
			}else{
				printf("未刷入驱动程序或者不成功");
		        exit(1);
			}
			printf("ditpro_ko 驱动\n\n");
		}
		else if (DriverInput == 0)
		{
			int check = syscall(__NR_syscall_, (__FLAGS | __CHECKSUCCESS));
			if(check != DRIVERSUCCESS){
				printf("未刷入驱动程序或者不成功");
		        exit(1);
			}
			printf("ditpro_kpm 驱动\n");
		}
		else
		{
			printf("输入错误,请重新输入\n");
			exit(1);
		}
	}

	void init_pid(pid_t pid)
	{
		{
			this->pid = pid;
		}
	}
	
	bool read(uintptr_t addr, void *buffer, uint32_t size)
    {
		if (this->DriverInput == 0)
        {
            struct Ditpro_uct_kpm ptr = {this->pid, addr, buffer, size};
            uint64_t flags = __READSYSTEM;
            if((uint32_t)*this->mode != 1)
                flags = __READASM;
               
			if(*this->cpuaddr == true){
				flags |= MEMCPU;
			}
            if(!syscall(__NR_syscall_, flags, &ptr))
                return true;
        }
        else if (this->DriverInput == 1)
        {
			struct Ditpro_uct_kpm ptr = {this->pid, addr, buffer, size};
            uint64_t flags = __READASM;        
               
			if(*this->cpuaddr == true)
				flags |= MEMCPU;
            if(!ioctl(fd, flags, &ptr))
				return true;
        }
        return false;
	}
	
	template <typename T>
	T read(uintptr_t addr)
	{
		T res __attribute__((aligned(8))){};
		if (this->read(addr, &res, sizeof(T))){
			return res;
	    }
		return {};
	}

	~c_driver()
	{		
        fd = -1;		
	}

	signed int getPid(const char *name){
	    if(this->DriverInput == 0)
	    {
		    return syscall(__NR_syscall_, (__FLAGS | PROCESSPID), name);
		}
		else if(this->DriverInput == 1)
		{
		    return ioctl(fd, (__FLAGS | PROCESSPID), name);
		}
		return -1;
	}
	
	signed long getBase(pid_t pid, char *name){
	    if(this->DriverInput == 0)
	    {
		    Ditpro_uct_base ptr = {this->pid, name, 0};
            syscall(__NR_syscall_, (__FLAGS | MODULEBASE), &ptr);
            return ptr.base;
		}
		else if(this->DriverInput == 1)
		{
		    Ditpro_uct_base ptr = {this->pid, name, 0};
            ioctl(fd, (__FLAGS | MODULEBASE), &ptr);
            return ptr.base;
		}
		return 0;
	}

    void mem_uninstall(){
		if(this->DriverInput == 0)
            syscall(__NR_syscall_, (__FLAGS | MEMUNINSTALL));
		else if(this->DriverInput == 1)
			ioctl(fd, (__FLAGS | MEMUNINSTALL));
    }
	

    int hideproc(){
		if(this->DriverInput == 0)
            return syscall(__NR_syscall_, (__FLAGS | HIDEPID), gettid());
		else if(this->DriverInput == 1)
			return ioctl(fd, (__FLAGS | HIDEPID), gettid());
		else 
			return -1;
    }

    void unhideproc(){
		if(this->DriverInput == 0)
            syscall(__NR_syscall_, (__FLAGS | UNHIDEPID), gettid());
		else if(this->DriverInput == 1)
			ioctl(fd, (__FLAGS | UNHIDEPID), gettid());
    }
	
	
static int setts(int value) {
    FILE *file;
    loff_t pos = 0;
    char buf[2];

    file = fopen("/proc/sys/kernel/kptr_restrict", "r+");
    if (!file) {
        printf("Failed to open /proc/sys/kernel/kptr_restrict\n");
        return -2;
    }

    snprintf(buf, sizeof(buf), "%d", value);

    fwrite(buf, strlen(buf), 1, file);

    fclose(file);

    return 0;
}

unsigned long get_symbol_from_proc(const char *name)
{
	if(setts(0))
	    return 0;
	    
    FILE *fp;
    char buf[256];
    unsigned long addr = 0;
    
    fp = fopen("/proc/kallsyms", "r");
    if (!fp){
    	printf("kallsyms open frined\n");
        return 0;
    }
    
    while (fgets(buf, 255, fp)) {
        char sym_name[128] = {0};
        char type;
        
        if (sscanf(buf, "%lx %c %127s", &addr, &type, sym_name) == 3) {
            if (strcmp(sym_name, name) == 0) {
                break;
            }
        }
        addr = 0;
    }
    
    fclose(fp);
    
    if(setts(2))
	    return 0;
	    
    return addr;
}

	int hideevent(const char *name){
		if(this->DriverInput == 0){
			return syscall(__NR_syscall_, (__FLAGS | HIDEEVENT), name);
		}else if(this->DriverInput == 1){
		    struct Dit_Device_Uct cm;
		    cm.addr = get_symbol_from_proc("input_dev_list");
		    if(!cm.addr)
			    return 1;
		    memset(cm.name,0,32);	
		    strncpy(cm.name, name, 31);
		    return ioctl(fd, (__FLAGS | HIDEEVENT), &cm);
		}
		return 1;
	}
	
	int unhideevent(){
		if(this->DriverInput == 0){
			return syscall(__NR_syscall_, (__FLAGS | UNHIDEEVENT));
		}else if(this->DriverInput == 1){
		    return ioctl(fd, (__FLAGS | UNHIDEEVENT));
		}
		return 1;
	}
};

static c_driver Core;

#endif
