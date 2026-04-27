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

struct TouchDeviceProbeResult {
    bool valid = false;
    bool has_mt_position = false;
    bool has_single_position = false;
    bool has_touch_key = false;
    int max_x = 0;
    int max_y = 0;
    int score = -1;
};

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

static bool IsBitSet(const unsigned long* bits, int bit) {
    const int bits_per_long = static_cast<int>(sizeof(unsigned long) * 8);
    return (bits[bit / bits_per_long] & (1UL << (bit % bits_per_long))) != 0;
}

static TouchDeviceProbeResult ProbeTouchDeviceCapabilities(int fd) {
    TouchDeviceProbeResult result;
    if (fd < 0) return result;

    constexpr int kBitsPerLong = static_cast<int>(sizeof(unsigned long) * 8);
    unsigned long ev_bits[(EV_MAX / kBitsPerLong) + 1] = {};
    unsigned long abs_bits[(ABS_MAX / kBitsPerLong) + 1] = {};
    unsigned long key_bits[(KEY_MAX / kBitsPerLong) + 1] = {};

    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) return result;

    const bool has_abs = IsBitSet(ev_bits, EV_ABS);
    const bool has_key = IsBitSet(ev_bits, EV_KEY);
    if (!has_abs) return result;

    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) {
        memset(abs_bits, 0, sizeof(abs_bits));
    }
    if (has_key && ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
        memset(key_bits, 0, sizeof(key_bits));
    }

    result.has_mt_position =
        IsBitSet(abs_bits, ABS_MT_POSITION_X) && IsBitSet(abs_bits, ABS_MT_POSITION_Y);
    result.has_single_position =
        IsBitSet(abs_bits, ABS_X) && IsBitSet(abs_bits, ABS_Y);
    result.has_touch_key =
        has_key && (IsBitSet(key_bits, BTN_TOUCH) || IsBitSet(key_bits, BTN_TOOL_FINGER));

    if (!result.has_mt_position && !result.has_single_position) return result;
    if (!result.has_touch_key && !result.has_mt_position) return result;

    input_absinfo abs_x{};
    input_absinfo abs_y{};
    if (result.has_single_position) {
        if (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) == 0) result.max_x = abs_x.maximum;
        if (ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) == 0) result.max_y = abs_y.maximum;
    }
    if ((result.max_x <= 0 || result.max_y <= 0) && result.has_mt_position) {
        input_absinfo abs_mt_x{};
        input_absinfo abs_mt_y{};
        if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_mt_x) == 0) result.max_x = abs_mt_x.maximum;
        if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_mt_y) == 0) result.max_y = abs_mt_y.maximum;
    }
    if (result.max_x <= 0 || result.max_y <= 0) return result;

    result.valid = true;
    result.score = 0;
    if (result.has_mt_position) result.score += 4;
    if (result.has_touch_key) result.score += 2;
    if (result.has_single_position) result.score += 1;
    return result;
}

static void MapTouchToScreen(int raw_x, int raw_y, float& out_x, float& out_y) {
    const int screen_width = displayInfo.width;
    const int screen_height = displayInfo.height;
    out_x = 0.0f;
    out_y = 0.0f;

    if (touch_max_x <= 0 || touch_max_y <= 0 || screen_width <= 0 || screen_height <= 0) {
        return;
    }

    const float normalized_x = static_cast<float>(raw_x) / static_cast<float>(touch_max_x);
    const float normalized_y = static_cast<float>(raw_y) / static_cast<float>(touch_max_y);
    float screen_u = normalized_x;
    float screen_v = normalized_y;

    switch (displayInfo.orientation) {
        case 1:  // Rotation 90
            screen_u = normalized_y;
            screen_v = 1.0f - normalized_x;
            break;
        case 2:  // Rotation 180
            screen_u = 1.0f - normalized_x;
            screen_v = 1.0f - normalized_y;
            break;
        case 3:  // Rotation 270
            screen_u = 1.0f - normalized_y;
            screen_v = normalized_x;
            break;
        case 0:  // Rotation 0
        default:
            break;
    }

    out_x = std::clamp(screen_u, 0.0f, 1.0f) * static_cast<float>(screen_width);
    out_y = std::clamp(screen_v, 0.0f, 1.0f) * static_cast<float>(screen_height);
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
    const float screen_u = (screen_width > 0) ? (clamped_x / static_cast<float>(screen_width)) : 0.0f;
    const float screen_v = (screen_height > 0) ? (clamped_y / static_cast<float>(screen_height)) : 0.0f;
    float normalized_x = screen_u;
    float normalized_y = screen_v;

    switch (displayInfo.orientation) {
        case 1:  // Rotation 90
            normalized_x = 1.0f - screen_v;
            normalized_y = screen_u;
            break;
        case 2:  // Rotation 180
            normalized_x = 1.0f - screen_u;
            normalized_y = 1.0f - screen_v;
            break;
        case 3:  // Rotation 270
            normalized_x = screen_v;
            normalized_y = 1.0f - screen_u;
            break;
        case 0:  // Rotation 0
        default:
            break;
    }

    out_raw_x = static_cast<int>(std::lround(
        std::clamp(normalized_x, 0.0f, 1.0f) * static_cast<float>(touch_max_x)));
    out_raw_y = static_cast<int>(std::lround(
        std::clamp(normalized_y, 0.0f, 1.0f) * static_cast<float>(touch_max_y)));

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
    int best_score = -1;
    int best_max_x = 0;
    int best_max_y = 0;

    while ((ent = readdir(dir))) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;

        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int tmp_fd = open(path, O_RDONLY | O_NONBLOCK);
        if (tmp_fd < 0) continue;

        const TouchDeviceProbeResult probe = ProbeTouchDeviceCapabilities(tmp_fd);
        if (probe.valid && probe.score > best_score) {
            best_score = probe.score;
            best_max_x = probe.max_x;
            best_max_y = probe.max_y;
        }

        close(tmp_fd);
    }

    closedir(dir);
    if (best_score >= 0) {
        touch_max_x = best_max_x;
        touch_max_y = best_max_y;
        LOGI("Refreshed touch device range after rotation: %d x %d", touch_max_x, touch_max_y);
    }
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
    int best_score = -1;

    while ((ent = readdir(dir))) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;

        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int tmp_fd = open(path, O_RDONLY | O_NONBLOCK);
        if (tmp_fd < 0) continue;

        // 读取设备名字
        char name[256] = "unknown";
        ioctl(tmp_fd, EVIOCGNAME(sizeof(name)), name);

        const TouchDeviceProbeResult probe = ProbeTouchDeviceCapabilities(tmp_fd);
        if (!probe.valid) {
            close(tmp_fd);
            continue;
        }

        if (probe.score > best_score) {
            if (fd >= 0) close(fd);
            fd = tmp_fd;
            best_score = probe.score;
            touch_max_x = probe.max_x;
            touch_max_y = probe.max_y;
            LOGI("Found touch device candidate: %s (%s), score=%d, range=%d x %d",
                 path, name, probe.score, touch_max_x, touch_max_y);
        } else {
            close(tmp_fd);
        }
    }

    closedir(dir);
    if (fd >= 0) {
        LOGI("Using touch device range: %d x %d", touch_max_x, touch_max_y);
    }
    return fd;
}

void process_input_event(int fd) {
    static int x = 0, y = 0;
    static bool pressed = false;

    if (fd < 0) return;

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
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
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

bool IsTouchSlotActive(int slot) {
    if (slot < 0 || slot >= kMaxTouchSlots) return false;
    return g_touch_slots[slot].active;
}
