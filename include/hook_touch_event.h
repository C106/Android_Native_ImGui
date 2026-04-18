#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/ioctl.h>

struct TouchPoint {
    int id;
    float x;
    float y;
    bool active;
};

void process_input_event(int fd);
int find_touch_device();
void update_info();
void refresh_touch_device_range();
int get_active_touch_points(TouchPoint* out_points, int max_points);
bool has_active_touch_points();
bool MapScreenToTouch(float screen_x, float screen_y, int& out_raw_x, int& out_raw_y);

