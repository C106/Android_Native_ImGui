#include "hook_touch_event.h"
#include "imgui.h"
#include "ANativeWindowCreator.h"
#include <android/log.h>
#include <poll.h>
#include <algorithm>

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

namespace {
constexpr int kMaxTouchSlots = 10;

struct TouchSlotState {
    bool active = false;
    int tracking_id = -1;
    int raw_x = 0;
    int raw_y = 0;
    bool has_x = false;
    bool has_y = false;
};

TouchSlotState g_touch_slots[kMaxTouchSlots];
TouchPoint g_active_touches[kMaxTouchSlots];
int g_active_touch_count = 0;
int g_current_mt_slot = 0;
int g_primary_touch_slot = -1;

static void MapTouchToScreen(int raw_x, int raw_y, float& out_x, float& out_y) {
    const int screen_width = displayInfo.width;
    const int screen_height = displayInfo.height;
    out_x = 0.0f;
    out_y = 0.0f;

    if (touch_max_x <= 0 || touch_max_y <= 0 || screen_width <= 0 || screen_height <= 0) {
        return;
    }

    const bool screen_is_landscape = (screen_width > screen_height);
    if (screen_is_landscape) {
        out_x = static_cast<float>(raw_y) / static_cast<float>(touch_max_y) * screen_width;
        out_y = screen_height - static_cast<float>(raw_x) / static_cast<float>(touch_max_x) * screen_height;
    } else {
        out_x = static_cast<float>(raw_x) / static_cast<float>(touch_max_x) * screen_width;
        out_y = static_cast<float>(raw_y) / static_cast<float>(touch_max_y) * screen_height;
    }
}

static void RebuildActiveTouchesFromSlots() {
    g_active_touch_count = 0;
    for (int slot = 0; slot < kMaxTouchSlots; ++slot) {
        const TouchSlotState& state = g_touch_slots[slot];
        if (!state.active || !state.has_x || !state.has_y) continue;
        if (g_active_touch_count >= kMaxTouchSlots) break;

        TouchPoint& point = g_active_touches[g_active_touch_count++];
        point.id = state.tracking_id >= 0 ? state.tracking_id : slot;
        point.active = true;
        MapTouchToScreen(state.raw_x, state.raw_y, point.x, point.y);
    }
}

static bool TryMapSlotToScreenPoint(int slot, float& out_x, float& out_y) {
    if (slot < 0 || slot >= kMaxTouchSlots) return false;
    const TouchSlotState& state = g_touch_slots[slot];
    if (!state.active || !state.has_x || !state.has_y) return false;
    MapTouchToScreen(state.raw_x, state.raw_y, out_x, out_y);
    return true;
}
}  // namespace

bool MapScreenToTouch(float screen_x, float screen_y, int& out_raw_x, int& out_raw_y) {
    update_info();

    const int screen_width = displayInfo.width;
    const int screen_height = displayInfo.height;
    out_raw_x = 0;
    out_raw_y = 0;

    if (touch_max_x <= 0 || touch_max_y <= 0 || screen_width <= 0 || screen_height <= 0) {
        return false;
    }

    const float clamped_x = std::clamp(screen_x, 0.0f, static_cast<float>(screen_width));
    const float clamped_y = std::clamp(screen_y, 0.0f, static_cast<float>(screen_height));
    const bool screen_is_landscape = (screen_width > screen_height);
    if (screen_is_landscape) {
        out_raw_x = static_cast<int>(std::lround(
            (1.0f - clamped_y / static_cast<float>(screen_height)) * static_cast<float>(touch_max_x)));
        out_raw_y = static_cast<int>(std::lround(
            (clamped_x / static_cast<float>(screen_width)) * static_cast<float>(touch_max_y)));
    } else {
        out_raw_x = static_cast<int>(std::lround(
            (clamped_x / static_cast<float>(screen_width)) * static_cast<float>(touch_max_x)));
        out_raw_y = static_cast<int>(std::lround(
            (clamped_y / static_cast<float>(screen_height)) * static_cast<float>(touch_max_y)));
    }

    out_raw_x = std::clamp(out_raw_x, 0, touch_max_x);
    out_raw_y = std::clamp(out_raw_y, 0, touch_max_y);
    return true;
}

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
            if (ev.code == ABS_MT_SLOT) {
                g_current_mt_slot = std::clamp(ev.value, 0, kMaxTouchSlots - 1);
            } else if (ev.code == ABS_MT_TRACKING_ID) {
                TouchSlotState& slot = g_touch_slots[g_current_mt_slot];
                if (ev.value < 0) {
                    if (g_primary_touch_slot == g_current_mt_slot) {
                        g_primary_touch_slot = -1;
                    }
                    slot = TouchSlotState{};
                } else {
                    slot.active = true;
                    slot.tracking_id = ev.value;
                    g_primary_touch_slot = g_current_mt_slot;
                }
            } else if (ev.code == ABS_MT_POSITION_X) {
                TouchSlotState& slot = g_touch_slots[g_current_mt_slot];
                slot.raw_x = ev.value;
                slot.has_x = true;
                if (slot.active) g_primary_touch_slot = g_current_mt_slot;
            } else if (ev.code == ABS_MT_POSITION_Y) {
                TouchSlotState& slot = g_touch_slots[g_current_mt_slot];
                slot.raw_y = ev.value;
                slot.has_y = true;
                if (slot.active) g_primary_touch_slot = g_current_mt_slot;
            } else if (ev.code == ABS_X) {
                x = ev.value;
                TouchSlotState& slot = g_touch_slots[0];
                slot.raw_x = ev.value;
                slot.has_x = true;
                if (slot.active) g_primary_touch_slot = 0;
            } else if (ev.code == ABS_Y) {
                y = ev.value;
                TouchSlotState& slot = g_touch_slots[0];
                slot.raw_y = ev.value;
                slot.has_y = true;
                if (slot.active) g_primary_touch_slot = 0;
            }
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            pressed = (ev.value > 0);
            if (!pressed) {
                g_touch_slots[0].active = false;
                g_touch_slots[0].tracking_id = -1;
                if (g_primary_touch_slot == 0) {
                    g_primary_touch_slot = -1;
                }
            } else if (!g_touch_slots[0].active) {
                g_touch_slots[0].active = true;
                if (g_touch_slots[0].tracking_id < 0) {
                    g_touch_slots[0].tracking_id = 0;
                }
                g_primary_touch_slot = 0;
            }
        } else if (ev.type == EV_SYN) {
            ImGuiIO& io = ImGui::GetIO();
            RebuildActiveTouchesFromSlots();

            float final_x = 0.0f;
            float final_y = 0.0f;
            bool has_primary_touch = false;
            if (TryMapSlotToScreenPoint(g_primary_touch_slot, final_x, final_y)) {
                has_primary_touch = true;
            } else if (g_active_touch_count > 0) {
                final_x = g_active_touches[g_active_touch_count - 1].x;
                final_y = g_active_touches[g_active_touch_count - 1].y;
                has_primary_touch = true;
            } else if (pressed) {
                MapTouchToScreen(x, y, final_x, final_y);
                has_primary_touch = true;
            }

            if (has_primary_touch) {
                io.MousePos = ImVec2(final_x, final_y);
            }
            io.MouseDown[0] = has_primary_touch;
        }
    }
}

int get_active_touch_points(TouchPoint* out_points, int max_points) {
    if (!out_points || max_points <= 0) return 0;
    const int count = std::min(g_active_touch_count, max_points);
    for (int i = 0; i < count; ++i) {
        out_points[i] = g_active_touches[i];
    }
    return count;
}

bool has_active_touch_points() {
    return g_active_touch_count > 0;
}
