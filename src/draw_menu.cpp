#include "draw_menu.h"
float colors[4]={0};
void Draw_Menu(bool& MenuFlag){
    ImGui::Begin("My First Tool", &MenuFlag, ImGuiWindowFlags_MenuBar);
        float my_color[4];
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open..", "Ctrl+O")) { /* Do stuff */ }
                if (ImGui::MenuItem("Save", "Ctrl+S"))   { /* Do stuff */ }
                if (ImGui::MenuItem("Close", "Ctrl+W"))  { MenuFlag = false; }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        
        // Edit a color stored as 4 floats
        ImGui::ColorEdit4("Color", colors);

        // Generate samples and plot them
        float samples[100];
        for (int n = 0; n < 100; n++)
            samples[n] = sinf(n * 0.2f + ImGui::GetTime() * 1.5f);
        ImGui::PlotLines("Samples", samples, 100);
        ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("屏幕坐标: (%.1f, %.1f)", io.MousePos.x, io.MousePos.y);
        ImGui::Text(u8"こんにちは！テスト👻 %d", 123);
        // Display contents in a scrolling region
        ImGui::TextColored(ImVec4(1,1,0,1), u8"哈基米👻");
        ImGui::BeginChild("Scrolling");
        for (int n = 0; n < 50; n++)
            ImGui::Text(u8"%04d: Some text👻", n);
        ImGui::EndChild();
        ImGui::End();
}