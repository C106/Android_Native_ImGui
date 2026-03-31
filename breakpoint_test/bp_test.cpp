#include "kernel.h"
int main(int argc, char **argv)
{
    PACKAGENAME* bm = "com.tencent.tmgp.pubgmhd";
    if (getPID(bm) <= 0) {
        printf("[-] 未找到进程: %s\n", bm);
        return -1;
    }
    printf("[+] pid = %d\n", pid);

   
    uintptr_t base = driver->get_module_base("libUE4.so");
    
    printf("[+] libUE4.so base = %lX\n", base);

    uintptr_t target_addr = base + 0x8088F9C;
    printf("断点地址: %lX (Base + 0x8088F9C)\n", target_addr);
    int ret = driver->add_breakpoint(target_addr, HW_BP_TYPE_EXECUTE, 4);
if (ret == 0) {
    printf("下成功了\n");
} else {
    printf("下断点失败\n", ret, strerror(ret));

    return -1;
}
}
