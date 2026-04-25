#include <linux/input.h>
#include <poll.h>
extern std::atomic<bool> IsMenuOpen;
int getData() {
    DIR *dir = opendir("/dev/input/");
    dirent *ptr = NULL;
    int count = 0;
    while ((ptr = readdir(dir)) != NULL) {
        if (strstr(ptr->d_name, "event"))
            count++;
    }
    closedir(dir);
    return count ? count : -1;
}


int volume() {
    int EventCount = getData();
    if (EventCount < 0) {
        printf("未找到输入设备\n");
        return -1;
    }

    struct pollfd *pfds = (struct pollfd *)malloc(EventCount * sizeof(struct pollfd));

    for (int i = 0; i < EventCount; i++) {
        char temp[128];
        sprintf(temp, "/dev/input/event%d", i);
        pfds[i].fd = open(temp, O_RDONLY | O_NONBLOCK);
        pfds[i].events = POLLIN;
    }

    input_event ev;

    while (1) {
        int ret = poll(pfds, EventCount, -1); // 阻塞等待事件
        if (ret <= 0) continue;

        for (int i = 0; i < EventCount; i++) {
            if (!(pfds[i].revents & POLLIN)) continue;

            while (read(pfds[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_KEY && ev.value == 1) {
                    if (ev.code == KEY_VOLUMEUP) {
                        IsMenuOpen.store(true, std::memory_order_relaxed);
                    } else if (ev.code == KEY_VOLUMEDOWN) {
                        IsMenuOpen.store(false, std::memory_order_relaxed);
                    }
                }
            }
        }
    }
    return 0;
}
