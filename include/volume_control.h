#include <linux/input.h>
extern std::atomic<bool> IsMenuOpen;
int getData() {
    DIR *dir = opendir("/dev/input/");
    dirent *ptr = NULL;
    int count = 0;
    while ((ptr = readdir(dir)) != NULL) {
        if (strstr(ptr->d_name, "event"))
            count++;
    }
    return count ? count : -1;
}


int volume() {
    int EventCount = getData();
    if (EventCount < 0) {
        printf("未找到输入设备\n");
        return -1;
    }

    int *fdArray = (int *)malloc(EventCount * sizeof(int));

    for (int i = 0; i < EventCount; i++) {
        char temp[128];
        sprintf(temp, "/dev/input/event%d", i);
        fdArray[i] = open(temp, O_RDWR | O_NONBLOCK);
    }


    input_event ev;
    int count = 0; // 记录按下音量键的次数

    while (1) {
        for (int i = 0; i < EventCount; i++) {
            memset(&ev, 0, sizeof(ev));
            read(fdArray[i], &ev, sizeof(ev));
            if (ev.type == EV_KEY && (ev.code == KEY_VOLUMEUP || ev.code == KEY_VOLUMEDOWN)) {
          
                if (ev.code == 115&&ev.value==1) {
                    IsMenuOpen.store(true,std::memory_order_relaxed);
                } else if (ev.code == 114&&ev.value==1) {
                    IsMenuOpen.store(false,std::memory_order_relaxed);
                }
            }
            usleep(1000);
        }
        usleep(500);
    }
       usleep(1500);
    return 0;
}