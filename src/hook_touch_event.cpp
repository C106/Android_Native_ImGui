#include "hook_touch_event.h"
#include "imgui.h"
#include "ANativeWindowCreator.h"
#include <android/log.h>
#include <poll.h>

#ifdef NDEBUG
#define LOGI(...) ((void)0)
#else
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "TouchEvent", __VA_ARGS__)
#endif

extern android::ANativeWindowCreator::DisplayInfo displayInfo;

// 触摸设备的最大坐标范围
int touch_max_x = 0;
int touch_max_y = 0;
// 记录上一次的屏幕尺寸，用于检测屏幕旋转
static int last_screen_width = 0;
static int last_screen_height = 0;
// 标记是否需要重新检测触摸设备范围
static bool need_refresh_touch_range = false;

void update_info(){
    displayInfo = android::ANativeWindowCreator::GetDisplayInfo();

    // 检测屏幕旋转：当屏幕宽度和高度发生变化时，需要重新检测触摸设备范围
    if (last_screen_width != 0 && last_screen_height != 0) {
        if ((displayInfo.width != last_screen_width) || (displayInfo.height != last_screen_height)) {
            // 屏幕尺寸发生变化（可能是旋转），标记需要重新检测触摸设备范围
            LOGI("Screen rotated: %dx%d -> %dx%d, need refresh touch range",
                last_screen_width, last_screen_height, displayInfo.width, displayInfo.height);
            need_refresh_touch_range = true;
        }
    }
    last_screen_width = displayInfo.width;
    last_screen_height = displayInfo.height;
}

// 重新检测触摸设备范围（屏幕旋转后调用）
void refresh_touch_device_range() {
    if (!need_refresh_touch_range) return;

    // 重新遍历 /dev/input 查找触摸设备
    DIR* dir = opendir("/dev/input");
    if (!dir) return;

    struct dirent* ent;
    char path[256];
    int fd = -1;

    while ((ent = readdir(dir))) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;

        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int tmp_fd = open(path, O_RDONLY | O_NONBLOCK);
        if (tmp_fd < 0) continue;

        unsigned long evbit[EV_MAX/sizeof(long)+1];
        memset(evbit, 0, sizeof(evbit));
        ioctl(tmp_fd, EVIOCGBIT(0, sizeof(evbit)), evbit);

        bool has_abs = evbit[EV_ABS / (8*sizeof(long))] & (1UL << (EV_ABS % (8*sizeof(long))));
        bool has_key = evbit[EV_KEY / (8*sizeof(long))] & (1UL << (EV_KEY % (8*sizeof(long))));

        if (has_abs && has_key) {
            fd = tmp_fd;

            // 重新获取触摸设备的最大坐标范围
            input_absinfo abs_x, abs_y;
            if (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) == 0) {
                touch_max_x = abs_x.maximum;
            }
            if (ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) == 0) {
                touch_max_y = abs_y.maximum;
            }

            if (touch_max_x <= 0) {
                input_absinfo abs_mt_x;
                if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_mt_x) == 0) {
                    touch_max_x = abs_mt_x.maximum;
                }
            }
            if (touch_max_y <= 0) {
                input_absinfo abs_mt_y;
                if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_mt_y) == 0) {
                    touch_max_y = abs_mt_y.maximum;
                }
            }

            LOGI("Refreshed touch device range after rotation: %d x %d",
                touch_max_x, touch_max_y);

            close(fd);
            break;
        }

        close(tmp_fd);
    }

    closedir(dir);
    need_refresh_touch_range = false;
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
            LOGI("Found touch device: %s (%s)", path, name);
            fd = tmp_fd;

            // 如果触摸设备范围未初始化，则获取
            if (touch_max_x <= 0 || touch_max_y <= 0) {
                // 获取触摸设备的最大坐标范围
                // 优先使用单点触控 ABS_X/ABS_Y，如果没有则使用多点触控 ABS_MT_POSITION_X/Y
                input_absinfo abs_x, abs_y;
                if (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) == 0) {
                    touch_max_x = abs_x.maximum;
                }
                if (ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) == 0) {
                    touch_max_y = abs_y.maximum;
                }

                // 如果单点触控没有获取到，使用多点触控
                if (touch_max_x <= 0) {
                    input_absinfo abs_mt_x;
                    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_mt_x) == 0) {
                        touch_max_x = abs_mt_x.maximum;
                    }
                }
                if (touch_max_y <= 0) {
                    input_absinfo abs_mt_y;
                    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_mt_y) == 0) {
                        touch_max_y = abs_mt_y.maximum;
                    }
                }
                LOGI("Touch device range: %d x %d", touch_max_x, touch_max_y);
            }

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

    // 使用 poll() 检查是否有数据可读，避免 busy-wait
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    // 使用非阻塞 poll，避免不必要的内核唤醒
    int ret = poll(&pfd, 1, 0);
    if (ret <= 0) {
        // 无数据或错误，直接返回
        return;
    }

    // 有数据可读，批量处理所有可用事件
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

            // 使用设备显示屏幕的实际尺寸（从 displayInfo 获取）
            int screen_width = displayInfo.width;
            int screen_height = displayInfo.height;

            int final_x = 0;
            int final_y = 0;

            if (touch_max_x > 0 && touch_max_y > 0 && screen_width > 0 && screen_height > 0) {
                // 判断屏幕方向：横屏时交换坐标，竖屏时不交换
                bool screen_is_landscape = (screen_width > screen_height);

                if (screen_is_landscape) {
                    // 横屏：交换坐标并映射到屏幕尺寸
                    // 触摸设备的 x -> 屏幕的 y，触摸设备的 y -> 屏幕的 x
                    final_x = (int)((float)y / touch_max_y * screen_width);
                    final_y = (int)(screen_height - (float)x / touch_max_x * screen_height);
                } else {
                    // 竖屏：直接映射
                    final_x = (int)((float)x / touch_max_x * screen_width);
                    final_y = (int)((float)y / touch_max_y * screen_height);
                }
            }

            io.MousePos = ImVec2((float)final_x, (float)final_y);
            io.MouseDown[0] = pressed;
        }
    }
}