# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

##
注意！本项目仅用于内部调试测试，非引擎开发人员不要clone本项目。仅在开发组内部测试机器上运行，切忌向外部发送。
测试人员切勿开启远程访问该路径的权限。

## Project Overview

This is an Android native application that renders an ImGui overlay using Vulkan graphics API. It creates an Android overlay window (trusted overlay) for displaying UI on top of other applications.

## Build Commands

```bash
# Build the project (requires Android NDK at ~/android-ndk-r25c)
./build.sh

# Or manually with ndk-build
$NDK_PATH/ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk APP_ABI="arm64-v8a" APP_PLATFORM=android-24

# Output binary: libs/arm64-v8a/Debugger
```

## Architecture

```
main.cpp
├── ANativeWindowCreator   → Creates Android overlay Surface (hooks SurfaceFlinger)
├── VulkanApp              → Vulkan init, swapchain, render passes (uses vk-bootstrap)
├── ImGuiLayer             → ImGui init, font loading, texture loading, rendering
├── hook_touch_event       → Reads /dev/input/event* for touch input
├── Gyro (TCP client)      → Connects to 127.0.0.1:12345 for gyro data
└── draw_menu              → ImGui UI (Camera, Obj View, Config tabs)
```

## Key Components

- **VulkanApp** (`src/VulkanApp.cpp`): Wraps vk-bootstrap, manages swapchain, handles resize/orientation changes
- **ImGuiLayer** (`src/ImGuiLayer.cpp`): Initializes ImGui-Spectrum with custom styling, handles font (CJK + emoji) and texture loading
- **ANativeWindowCreator** (`include/ANativeWindowCreator.h`): Hooks Android private APIs to create overlay windows (Android 9-16 / API 24+)
- **hook_touch_event** (`src/hook_touch_event.cpp`): Reads Linux input devices, handles screen rotation
- **draw_menu** (`src/draw_menu.cpp`): Tab-based ImGui interface

## Dependencies (Git Submodules)

- `imgui`: ImGui-Spectrum (Adobe-style GUI)
- `vk-bootstrap`: Vulkan bootstrap library
- `freetype`: Font rendering
- `glm`: Math library
- `Vulkan-Headers`: Vulkan API headers

## Important Notes

- ImGui version: 1.90+
- Target architecture: `arm64-v8a`
- Minimum Android API: 24 (Android 9)
- Uses C++ static STL (`c++_static`)
- Double-buffered Vulkan rendering (maxFramesInFlight = 2)
- Touch input reads from `/dev/input/event*` devices
- Gyro connects via TCP socket to localhost:12345
