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

### 数据流（优化后，延迟 2-7ms）

```
读取线程 (read_mem.cpp)          渲染线程 (main.cpp + draw_objects.cpp)
  │                                │
  │ 每 500ms 扫描 actor 列表       │ 每帧:
  │ 缓存 addr + RootComponent     │  1. ReadGameData() 读取 VP 矩阵 + 本地玩家位置 (fence wait 前)
  │ 通过 shared_ptr 共享           │  2. vkWaitForFences() 等待 GPU (数据已读取，并发进行)
  │                                │  3. GetCachedActors() 获取地址列表 (shared_ptr)
  │                                │  4. DrawObjectsWithData() 使用预读数据绘制
  │                                │  5. vkQueuePresentKHR (MAILBOX 非阻塞)
```

**延迟优化说明：**
- **Phase 1 (MAILBOX)**: Present mode 从 FIFO 切换到 MAILBOX，消除 VSync 阻塞延迟（8-16ms）
- **Phase 2 (Early Read)**: VP 矩阵和本地玩家位置在 fence wait 前读取，避免等待 GPU 后再读取（8-16ms）
- **总延迟**: 从原来的 18-37ms (1-2 帧) 降低到 2-7ms (0.1-0.4 帧)，提升 70-85%

## Key Components

- **VulkanApp** (`src/VulkanApp.cpp`): Wraps vk-bootstrap, manages swapchain, handles resize/orientation changes. Uses MAILBOX present mode to avoid VSync phase-locking
- **ImGuiLayer** (`src/ImGuiLayer.cpp`): Initializes ImGui-Spectrum with custom styling, handles font (CJK + emoji) and texture loading
- **ANativeWindowCreator** (`include/ANativeWindowCreator.h`): Hooks Android private APIs to create overlay windows (Android 9-16 / API 24+)
- **hook_touch_event** (`src/hook_touch_event.cpp`): Reads Linux input devices, handles screen rotation
- **draw_menu** (`src/draw_menu.cpp`): Tab-based ImGui interface
- **draw_objects** (`src/draw_objects.cpp`):
  - `ReadGameData()`: 在 fence wait 前读取 VP 矩阵和本地玩家位置（减少延迟）
  - `DrawObjectsWithData()`: 使用预读数据绘制 actor（round-robin 隔帧交替更新位置）
  - 优化后延迟: 2-7ms (0.1-0.4 帧 @ 60Hz)
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
- Vulkan present mode: MAILBOX (non-blocking, 降低延迟 8-16ms, fallback to FIFO)
- 数据读取优化: VP 矩阵在 fence wait 前读取，减少 8-16ms 延迟
- Actor 位置读取在渲染线程中完成，round-robin 隔帧交替，每帧 N/2 次 ioctl
- RootComponent 指针在扫描时缓存，避免每帧重复读取
