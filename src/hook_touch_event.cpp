#include "hook_touch_event.h"
#include "imgui.h"
#include "ANativeWindowCreator.h"
extern android::ANativeWindowCreator::DisplayInfo displayInfo;
int orientation= 0;
void update_info(){
    displayInfo = android::ANativeWindowCreator::GetDisplayInfo();
    orientation = displayInfo.orientation;
    printf("orientation %d\n",displayInfo.orientation);
}

int find_touch_device() {
    DIR* dir = opendir("/dev/input");
    if (!dir) {
        perror("opendir");
        return -1;
    }

    struct dirent* ent;
    char path[256];
    int fd = -1;

    while ((ent = readdir(dir))) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;

        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int tmp_fd = open(path, O_RDONLY | O_NONBLOCK);
        if (tmp_fd < 0) continue;

        // 读取设备名字
        char name[256] = "unknown";
        ioctl(tmp_fd, EVIOCGNAME(sizeof(name)), name);

        unsigned long evbit[EV_MAX/sizeof(long)+1];
        memset(evbit, 0, sizeof(evbit));
        ioctl(tmp_fd, EVIOCGBIT(0, sizeof(evbit)), evbit);

        // 必须支持 EV_ABS 和 EV_KEY
        bool has_abs = evbit[EV_ABS / (8*sizeof(long))] & (1UL << (EV_ABS % (8*sizeof(long))));
        bool has_key = evbit[EV_KEY / (8*sizeof(long))] & (1UL << (EV_KEY % (8*sizeof(long))));

        if (has_abs && has_key) {
            printf("Found touch device: %s (%s)\n", path, name);
            fd = tmp_fd;
            break;
        }

        close(tmp_fd);
    }

    closedir(dir);
    return fd;
}

void process_input_event(int fd) {
    static int x = 0, y = 0;
    static bool pressed = false;

    struct input_event ev;
    while (read(fd, &ev, sizeof(ev)) > 0) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X) {
                x = ev.value;
            } else if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y) {
                y = ev.value;
            }
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            pressed = (ev.value > 0);
        } else if (ev.type == EV_SYN) {
            ImGuiIO& io = ImGui::GetIO();
            if(orientation==0)
                io.MousePos = ImVec2((float)x, (float)y);
            else if(orientation==1)
                io.MousePos = ImVec2((float)y, displayInfo.height-(float)x);
            else if(orientation==2)
                io.MousePos = ImVec2(displayInfo.width-(float)y, displayInfo.height-(float)x);
            else
                io.MousePos = ImVec2(displayInfo.width-(float)y, (float)x);
            io.MouseDown[0] = pressed;
        }
    }
}