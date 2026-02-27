# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 注意

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
main.cpp (渲染主循环)
├── ANativeWindowCreator   → Creates Android overlay Surface (hooks SurfaceFlinger)
├── VulkanApp              → Vulkan init, swapchain (MAILBOX present), render passes
├── ImGuiLayer             → ImGui init, font loading, texture loading, rendering
├── hook_touch_event       → Reads /dev/input/event* for touch input
├── Gyro (TCP client)      → Connects to 127.0.0.1:12345 for gyro data
├── draw_menu              → ImGui UI (Camera, Obj View, Config tabs)
└── draw_objects           → 渲染线程即时读取 actor 位置 + VP 矩阵并绘制
```

### 数据流

```
读取线程 (read_mem.cpp)          渲染线程 (main.cpp + draw_objects.cpp)
  │                                │
  │ 每 500ms 扫描 actor 列表       │ 每帧:
  │ 缓存 addr + RootComponent     │  1. GetCachedActors() 获取地址列表 (shared_ptr)
  │ 通过 shared_ptr 共享           │  2. 即时读取 VP 矩阵 (1 ioctl)
  │                                │  3. Round-robin 读取 actor 位置 (N/2 ioctl/帧)
  │                                │  4. WorldToScreen + 绘制
```

## Key Components

- **VulkanApp** (`src/VulkanApp.cpp`): Wraps vk-bootstrap, manages swapchain, handles resize/orientation changes. Uses MAILBOX present mode to avoid VSync phase-locking
- **ImGuiLayer** (`src/ImGuiLayer.cpp`): Initializes ImGui-Spectrum with custom styling, handles font (CJK + emoji) and texture loading
- **ANativeWindowCreator** (`include/ANativeWindowCreator.h`): Hooks Android private APIs to create overlay windows (Android 9-16 / API 24+)
- **hook_touch_event** (`src/hook_touch_event.cpp`): Reads Linux input devices, handles screen rotation
- **draw_menu** (`src/draw_menu.cpp`): Tab-based ImGui interface
- **draw_objects** (`src/draw_objects.cpp`): 渲染线程即时读取 actor 位置和 VP 矩阵，round-robin 隔帧交替更新，零缓冲延迟
- **read_mem** (`src/read_mem.cpp`): 读取线程每 500ms 扫描 actor 列表，缓存地址 + RootComponent 指针，通过 shared_ptr 共享给渲染线程
- **FrameSynchronizer** (`include/FrameSynchronizer.h`): 泛型双缓冲同步器，目前仅用于 UI 信息显示

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
- Vulkan present mode: MAILBOX (non-blocking, fallback to FIFO)
- Actor 位置读取在渲染线程中完成，round-robin 隔帧交替，每帧 N/2 次 ioctl
- RootComponent 指针在扫描时缓存，避免每帧重复读取
