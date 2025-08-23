#include "hook_touch_event.h"
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

    int touch_fd = find_touch_device();
    if (touch_fd < 0) {
        printf("No touch device found!\n");
        return -1;
    }
    bool my_tool_active;
    while (gRunning) {
        int w = native_window_screen_x;
        int h = native_window_screen_y;

        process_input_event(touch_fd);

        gImGui.beginFrame(gWindow, w, h);

        // 示例窗口
        ImGui::Begin("My First Tool", &my_tool_active, ImGuiWindowFlags_MenuBar);
        float my_color[4];
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open..", "Ctrl+O")) { /* Do stuff */ }
                if (ImGui::MenuItem("Save", "Ctrl+S"))   { /* Do stuff */ }
                if (ImGui::MenuItem("Close", "Ctrl+W"))  { my_tool_active = false; }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // Edit a color stored as 4 floats
        ImGui::ColorEdit4("Color", my_color);

        // Generate samples and plot them
        float samples[100];
        for (int n = 0; n < 100; n++)
            samples[n] = sinf(n * 0.2f + ImGui::GetTime() * 1.5f);
        ImGui::PlotLines("Samples", samples, 100);

        // Display contents in a scrolling region
        ImGui::TextColored(ImVec4(1,1,0,1), "Important Stuff");
        ImGui::BeginChild("Scrolling");
        for (int n = 0; n < 50; n++)
            ImGui::Text("%04d: Some text", n);
        ImGui::EndChild();
        ImGui::End();
        
        gImGui.endFrame(gApp);
        //gFrameIndex = (gFrameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
        
    }

    gImGui.shutdown(gApp);
    gApp.cleanup();
    ANativeWindow_release(gWindow);
    gWindow = nullptr;
}