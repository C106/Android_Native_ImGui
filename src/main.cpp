#include "hook_touch_event.h"
#include "VulkanApp.h"
#include "ImGuiLayer.h"
#include "ANativeWindowCreator.h"
#include "draw_menu.h"

static VulkanApp gApp;
static ImGuiLayer gImGui;
static ANativeWindow* gWindow = nullptr;
static bool gRunning = false;
android::ANativeWindowCreator::DisplayInfo displayInfo;
int secure_flag = 0;

int main() {
    displayInfo = android::ANativeWindowCreator::GetDisplayInfo();
    int native_window_screen_x = (displayInfo.height > displayInfo.width ? displayInfo.height : displayInfo.width);
    int native_window_screen_y = (displayInfo.height > displayInfo.width ? displayInfo.height : displayInfo.width);


    gWindow = android::ANativeWindowCreator::Create("a", native_window_screen_x, native_window_screen_y, false);
    printf("Native Surface\n");
    if(gApp.init(gWindow))
    printf("Vulkan\n");
    else
    printf("Vulkan Failed\n");
    gImGui.init(gWindow, gApp);
    printf("ImGui");
    gRunning = true;

    int touch_fd = find_touch_device();
    if (touch_fd < 0) {
        printf("No touch device found!\n");
        return -1;
    }
    bool my_tool_active=1;
    while (my_tool_active) {
        int w = native_window_screen_x;
        int h = native_window_screen_y;

        process_input_event(touch_fd);

        gImGui.beginFrame(gWindow, w, h);
        
        
        Draw_Menu(my_tool_active);
        
        
        gImGui.endFrame();
        gImGui.frame_render(gApp);
        //gFrameIndex = (gFrameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
        
    }

    gImGui.shutdown(gApp);
    gApp.cleanup();
    ANativeWindow_release(gWindow);
    gWindow = nullptr;
}