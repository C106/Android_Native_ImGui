
#include "VulkanApp.h"
#include "ImGuiLayer.h"
#include "ANativeWindowCreator.h"

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



    while (gRunning) {
        int w = native_window_screen_x;
        int h = native_window_screen_y;

        gImGui.beginFrame(gWindow, w, h);

        // 示例窗口
        ImGui::Begin("Hello");
        ImGui::Text("Hello Vulkan + ImGui!");
        if (ImGui::Button("Exit")) gRunning = false;
        ImGui::End();

        gImGui.endFrame(gApp);
    }

    gImGui.shutdown(gApp);
    gApp.cleanup();
    ANativeWindow_release(gWindow);
    gWindow = nullptr;
}