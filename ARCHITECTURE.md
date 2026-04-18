# Android Native ImGui — 项目架构文档

## 目录

1. [概览](#概览)
2. [目录结构](#目录结构)
3. [构建系统](#构建系统)
4. [外部依赖](#外部依赖)
5. [线程模型](#线程模型)
6. [子系统详解](#子系统详解)
   - [驱动层 (Driver Layer)](#驱动层)
   - [内存读取 (Memory Reader)](#内存读取)
   - [渲染管线 (Rendering Pipeline)](#渲染管线)
   - [绘制对象 (Draw Objects / ESP)](#绘制对象)
   - [PhysX 几何 (PhysX Geometry)](#physx-几何)
   - [可见性检测 (Visibility)](#可见性检测)
   - [自瞄控制器 (Auto-Aim)](#自瞄控制器)
   - [输入系统 (Input)](#输入系统)
   - [菜单框架 (Menu Framework)](#菜单框架)
   - [子弹散布监控 (Bullet Spread Monitor)](#子弹散布监控)
   - [性能工具 (Performance Utilities)](#性能工具)
7. [子系统交互图](#子系统交互图)
8. [关键设计模式](#关键设计模式)
9. [数据流](#数据流)

---

## 概览

本项目是一个运行于 Android 的独立原生可执行文件（`Debugger`），通过内核驱动读取目标游戏进程内存，使用 Vulkan 创建透明叠加窗口并借助 Dear ImGui 进行绘制。

产物为 `arm64-v8a` ELF 可执行文件，通过 NDK `ndk-build` 构建。

---

## 目录结构

```
Android_Native_ImGui/
├── src/                    # 所有 C++ 源文件
│   ├── main.cpp            # 入口点、主渲染循环
│   ├── read_mem.cpp        # 后台内存读取线程
│   ├── draw_objects.cpp    # ESP 绘制、PhysX 几何、GPU 深度可见性
│   ├── draw_menu.cpp       # ImGui 菜单 UI
│   ├── auto_aim.cpp        # 自瞄控制器
│   ├── visibility_scene.cpp# CPU BVH 射线检测
│   ├── hook_touch_event.cpp# 触摸事件读取
│   ├── menu_framework.cpp  # 声明式菜单注册系统
│   ├── VulkanApp.cpp       # Vulkan 初始化
│   ├── ImGuiLayer.cpp      # ImGui 初始化、字体、纹理加载
│   ├── physx_bvh_export.cpp# PhysX BVH 网格导出
│   └── bullet_spread_monitor.cpp # 硬件断点子弹散布监控
│
├── include/                # 所有项目头文件
│   ├── mem_struct.h        # 核心数学类型 (Vec2/Vec3/FMatrix/FTransform)、游戏偏移量
│   ├── kernel.h            # c_driver — ioctl 内核驱动接口
│   ├── driver.h            # paradise_driver — 预编译驱动 API
│   ├── driver_manager.h    # DriverManager — 线程本地驱动复用器
│   ├── read_mem.h          # 内存读取接口、Actor/骨骼/帧数据结构
│   ├── auto_aim.h          # 自瞄配置、目标状态、控制器类
│   ├── draw_objects.h      # ESP 绘制接口
│   ├── draw_menu.h         # 菜单绘制接口
│   ├── visibility_scene.h  # CPU BVH 射线检测场景
│   ├── physx_bvh_export.h  # PhysX BVH 导出接口
│   ├── hook_touch_event.h  # 触摸事件接口
│   ├── menu_framework.h    # 声明式菜单框架
│   ├── VulkanApp.h         # Vulkan 上下文
│   ├── ImGuiLayer.h        # ImGui 渲染层
│   ├── Gyro.h              # 陀螺仪 TCP 代理
│   ├── HwBreakpointMgr4.h  # 硬件断点管理器
│   ├── cpu_affinity.h      # CPU 拓扑与线程绑核
│   ├── game_fps_monitor.h  # 游戏 FPS 监测
│   ├── volume_control.h    # 音量键监听
│   ├── FrameSynchronizer.h # 双缓冲帧同步器
│   └── banner.h / logo.h / font*.h  # 嵌入式二进制资源
│
├── imgui/                  # Git 子模块 — Dear ImGui (Adobe Spectrum 主题)
├── vk-bootstrap/           # Git 子模块 — Vulkan 设备/实例启动库
├── Vulkan-Headers/         # Git 子模块 — Khronos Vulkan 头文件
├── freetype/               # Git 子模块 — FreeType 字体光栅化
├── glm/                    # Git 子模块 — OpenGL 数学库
├── implot3d/               # Git 子模块 — ImGui 3D 绘图扩展
│
├── shaders/                # GLSL 计算着色器源码
│   ├── visibility_depth.comp       # GPU 软光栅化深度缓冲
│   └── visibility_depth_query.comp # GPU 深度查询遮挡检测
├── generated/              # 预编译 SPIR-V 二进制
│
├── scripts/                # Python 离线分析脚本
├── Unreal/                 # UE4 SDK 参考头文件
│
├── Android.mk              # NDK 构建主文件
├── Application.mk          # NDK 应用级设置
└── build.sh                # 构建脚本
```

---

## 构建系统

- **工具链**：Android NDK `ndk-build`
- **目标 ABI**：`arm64-v8a`，API Level `android-24`
- **C++ STL**：`c++_shared`
- **产物**：`libs/arm64-v8a/Debugger`（ELF 可执行文件）
- **静态库**：`freetype`（源码编译）、`libparadise_api.a`（预编译闭源驱动）
- **系统库**：`-lm -ldl -lz -llog -landroid -lvulkan`
- **编译标志**：保留调试符号 (`-g -fno-omit-frame-pointer`) 以支持 Simpleperf 性能分析

---

## 外部依赖

| 子模块 | 用途 |
|---|---|
| `imgui` (Adobe fork) | UI 框架，含 Vulkan + Android 后端、Freetype 字体渲染 |
| `vk-bootstrap` | 简化 Vulkan 实例/设备/交换链创建 |
| `Vulkan-Headers` | Khronos 官方 Vulkan API 头文件 |
| `freetype` | Unicode/Emoji 字体光栅化 |
| `glm` | 向量/矩阵/四元数数学库 |
| `libparadise_api.a` | 闭源内核驱动接口（内存读写、陀螺仪注入、触摸注入、进程隐藏） |

---

## 线程模型

```
┌──────────────────────────────────────────────────────────────┐
│  主线程 / 渲染线程 (BIG Core)                                │
│                                                              │
│  main() → Vulkan Init → ImGui Init                           │
│       └─→ 主循环:                                            │
│            1. 处理触摸输入                                    │
│            2. 检测屏幕旋转                                    │
│            3. 等待 GPU Fence                                  │
│            4. 抓取游戏帧数据 (ReadGameData)                    │
│            5. beginFrame → Draw* → endFrame → submitAndPresent│
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────┐  ┌───────────────────────────┐
│  读取线程 (Read Thread)       │  │  自瞄线程 (Auto-Aim)      │
│                              │  │                           │
│  扫描 UE4 Actor 数组          │  │  120 Hz 目标选择           │
│  读取骨骼数据                 │  │  PD 控制器 + Holt 预测     │
│  分类 Actor (玩家/Bot/NPC..)  │  │  陀螺仪/触摸输出           │
│  发布到共享缓存               │  │  触发器 Bot                │
└──────────────────────────────┘  └───────────────────────────┘

┌──────────────────────────────┐  ┌───────────────────────────┐
│  音量监听线程 (LITTLE Core)   │  │  游戏 FPS 监测 (LITTLE)   │
│                              │  │                           │
│  poll() 音量键事件            │  │  dumpsys SurfaceFlinger   │
│  切换菜单开关                 │  │  或 DeltaTime 采样         │
└──────────────────────────────┘  └───────────────────────────┘

┌──────────────────────────────┐
│  子弹散布线程 (可选)          │
│                              │
│  硬件断点采样                 │
│  读取寄存器获取散布值          │
└──────────────────────────────┘
```

---

## 子系统详解

### 驱动层

**文件**：`kernel.h`、`driver.h`、`driver_manager.h`

提供对目标进程的内核级内存读写能力。支持两套后端：

- **RT Hook (`c_driver`)**：基于 ioctl 的内核模块，命令码包括 `READ(601)`、`WRITE(602)`、`BASE(603)`、`GETPID(604)`、`HIDE_PROCESS(605)`、`HW_BREAKPOINT(608/609)`
- **Paradise (`paradise_driver`)**：封装 `libparadise_api.a`，额外支持陀螺仪注入、触摸注入

`DriverManager` 是**线程本地单例**（`thread_local`），通过原子代数计数器实现无锁跨线程驱动切换。RT Hook 模式下读操作无互斥锁，Paradise 模式下受 mutex 保护。

```
DriverManager (thread_local)
    ├── c_driver (ioctl fd)        ← DRIVER_RT_HOOK
    └── paradise_driver (API)      ← DRIVER_PARADISE
```

### 内存读取

**文件**：`read_mem.cpp`、`read_mem.h`、`FrameSynchronizer.h`

后台线程持续扫描 UE4 `GUObjectArray`，构建 Actor 列表并分类：

```
ReadThread
    │
    ├── 扫描 GUObjectArray → 过滤有效 Actor
    ├── 读取每个 Actor 的:
    │     ├── RootComponent → WorldPosition
    │     ├── SkeletalMeshComponent → BoneArray (FTransform[])
    │     ├── ClassName (FName → GNames 解析)
    │     ├── TeamID / PlayerName / PlayerKey
    │     └── ActorType 分类
    │
    └── 发布到共享状态:
          ├── ClassifiedActors (11 类: Player/Bot/NPC/Monster/TombBox...)
          └── GameFrameData (VP 矩阵、本地玩家位置、DeltaTime、帧号)
```

关键数据结构：
- `CachedActor` — 每个 Actor 的缓存（地址、骨骼映射、分类信息）
- `BoneScreenData` — 屏幕空间骨骼坐标（含骨骼缓存指针，供 auto-aim 实时刷新）
- `GameFrameData` — 单帧游戏快照

### 渲染管线

**文件**：`VulkanApp.cpp`、`ImGuiLayer.cpp`、`main.cpp`

```
SurfaceFlinger Binder IPC
    └── ANativeWindow (透明叠加窗口)
         └── VulkanApp
              ├── VkInstance / VkDevice / VkQueue
              ├── VkSwapchain (MAILBOX 模式，低延迟)
              ├── VkRenderPass (单 pass，load=CLEAR, store=STORE)
              └── maxFramesInFlight = 1 (最小化管线深度)
                   └── ImGuiLayer
                        ├── ImGui Vulkan Backend
                        ├── Freetype 字体渲染 (Unicode/Emoji)
                        └── 异步纹理加载队列
```

**两阶段渲染分离**：
1. `waitForPreviousFrame()` — 等待 GPU Fence
2. 抓取游戏数据（Fence 释放后再读，最小化数据延迟）
3. `beginFrame()` → 绘制 → `endFrame()` → `submitAndPresent()`

**游戏帧同步**：渲染循环轮询 `GFrameCounter`（游戏内存），仅在新游戏帧到达时渲染，实现与引擎帧率 1:1 对齐。

### 绘制对象

**文件**：`draw_objects.cpp`（~340 KB，项目最大文件）

负责所有 ESP 叠加层绘制：

- **骨骼绘制**：读取 `FTransform[]` 骨骼数组 → `WorldToScreen` 投影 → ImDrawList 画线
- **名称/距离标签**：Actor 分类后按类型着色
- **PhysX 几何渲染**：读取游戏 PhysX 场景的三角网格/凸包/高度场/球/胶囊体
- **本地模型缓存**：导出 PhysX 网格为 `.obj`/`.bvh.txt` 文件，跨运行复用
- **GPU 深度可见性**：Vulkan 计算着色器软光栅化 PhysX 三角形为深度缓冲

**异步文件加载**：`.obj`/`.bvh.txt` 文件加载通过 `std::async` 后台线程执行，避免阻塞绘制线程。每帧轮询 `PollAsyncLocalLoads()` 收集结果。

**内容哈希签名**：三角网格使用 FNV-1a 哈希（顶点 + 索引数据）作为签名，确保跨运行稳定匹配本地文件。

### PhysX 几何

**文件**：`draw_objects.cpp`（PhysX 部分）、`physx_bvh_export.cpp`

从游戏内存读取 PhysX 3.4 场景树：

```
UWorld → FPhysScene → GPhysXSceneMap → NpScene
    ├── ActorArray → NpRigidStatic/Dynamic
    │     └── NpShape[] → Geometry (Sphere/Capsule/Box/ConvexMesh/TriangleMesh/HeightField)
    │
    └── NpSceneQueries → AABBPruner/BucketPruner
          └── PruningPool → PhysXPrunerPayload[] + PhysXBounds3[]
```

**缓存体系**：
- `gTriangleMeshCache` — 从内存读取的三角网格（顶点 + 索引）
- `gHeightFieldCache` — 高度场采样数据
- `gConvexMeshCache` — 凸包数据
- `gLocalTriObjCache` / `gLocalHfObjCache` — 从本地 `.obj` 文件加载的数据
- `gLocalTriBvhCache` — 从本地 `.bvh.txt` 文件加载的 BVH 数据
- `gPrunerCache` — 每帧 Pruner 数据快照（按预算分帧读取，最多 20k/帧）

**自动导出**：新遇到的网格自动导出为 `.obj` + `.bvh.txt` 文件，供后续运行直接加载。

### 可见性检测

**文件**：`visibility_scene.cpp`、`draw_objects.cpp`（GPU 路径）、`shaders/`

双路径可见性检测系统：

**CPU 路径 — BVH 射线检测**：
```
VisibilityScene
    ├── SceneMesh (SOA 布局, 32 三角形/块, NEON 友好)
    │     ├── V0X[], V0Y[], V0Z[]
    │     ├── Edge1X[], Edge1Y[], Edge1Z[]
    │     └── Edge2X[], Edge2Y[], Edge2Z[]
    │
    ├── shared_mutex 保护的网格映射
    ├── BuildSnapshot() → 不可变快照供并发射线检测
    └── RaycastAny(origin, dir) → Möller-Trumbore 交叉检测
```

**GPU 路径 — 计算着色器深度缓冲**：
```
Pass 1: visibility_depth.comp
    ├── 输入: PhysX 三角形 SSBO
    ├── 每线程处理一个三角形
    ├── 重心坐标边函数光栅化
    └── atomicMax(uint) 写入深度 (Reversed-Z)

Pass 2: visibility_depth_query.comp
    ├── 输入: 骨骼屏幕坐标 + 深度
    ├── 3×3 邻域深度采样
    └── 输出: visible/occluded 标志
```

GPU Fence 超时 8ms，超时则跳过本帧可见性检测，避免阻塞绘制线程。

### 自瞄控制器

**文件**：`auto_aim.cpp`、`auto_aim.h`

独立线程运行（~120 Hz），完整控制流：

```
AutoAimController::UpdateThreadFunc()
    │
    ├── 1. 检测开火状态 (IsLocalPlayerFiring)
    │
    ├── 2. 选择目标 (SelectTarget)
    │     ├── 获取骨骼屏幕缓存 (GetBoneScreenCache)
    │     ├── 实时刷新骨骼世界坐标 (ReadFreshBoneWorldPos)
    │     ├── 读取 VP 矩阵 → WorldToScreen 投影
    │     ├── FOV 限制 / 距离限制 / 队友过滤
    │     ├── 可见性检测 (可选)
    │     └── 迟滞阈值防止频繁切换
    │
    ├── 3. 计算控制输出 (ComputePDOutput)
    │     ├── PD 控制器 (KpX/KdX/KpY/KdY)
    │     ├── Holt 双指数平滑 (位置 + 趋势预测)
    │     ├── 前馈补偿 (目标速度低通滤波)
    │     ├── FOV 缩放 / 灵敏度补偿
    │     ├── 后坐力补偿 (基础抬升 + 上跳偏移)
    │     └── 人性化噪声 (平滑随机漂移 + 高频微颤)
    │
    ├── 4. 输出模式
    │     ├── ASSIST → 陀螺仪注入 (SendGyroOutput)
    │     └── MAGNET → 吸附模式 (捕获/释放半径)
    │
    └── 5. 触发器 Bot (UpdateTriggerBot)
          ├── 多骨骼命中检测 (头/颈/胸/骨盆)
          └── 触摸注入开火 (paradise_driver touch_down/up)
```

**目标丢失保护**：80ms 宽限期，避免目标闪烁导致重置。
**速度平滑**：指数平滑低通滤波 (`kVelSmoothAlpha=8.0`)，防止敌方变向时前馈突变。

### 输入系统

**文件**：`hook_touch_event.cpp`、`volume_control.h`、`Gyro.h`

```
输入源                        处理方式
──────────────────────────────────────────
/dev/input/eventN (触摸屏)  → evdev MT 协议读取 → 映射到屏幕坐标 → ImGui 输入
/dev/input/eventN (音量键)  → poll() 阻塞监听 → IsMenuOpen 原子切换
localhost:12345 (TCP)        → Gyro::update(x,y) → 陀螺仪代理守护进程
paradise_driver              → touch_down/move/up → 内核级触摸注入
                             → gyro_update(x,y) → 内核级陀螺仪注入
```

### 菜单框架

**文件**：`menu_framework.cpp`、`menu_framework.h`、`draw_menu.cpp`

声明式注册系统：

```
MenuRegistry (单例)
    └── MenuPageSpec[]
         └── MenuSectionSpec[]
              └── MenuSettingSpec[]
                   ├── 类型: Bool / Int / Float / Choice / Button / Text
                   ├── VisibleIf 条件谓词 (动态显隐)
                   └── 快捷挂件 (屏幕任意位置, Hold/Toggle 模式)
```

- **JSON 导入/导出**：`debugger_config.json` 持久化所有设置
- **悬浮球**：80×80 可拖动浮球，点击打开菜单，记忆关闭时的位置
- **三标签页**：Camera / Objects / Config

### 子弹散布监控

**文件**：`bullet_spread_monitor.cpp`

通过 `CHwBreakpointMgr` 在目标函数入口设置硬件执行断点，命中时读取寄存器和堆栈帧：

```
断点地址: libUE4 + 0x8087F88 (子弹散布函数)
命中时:
    x0 = this 指针
    this + 0x26C → spread Vec3
    this + 0x70C/710/714 → deviation / yaw / pitch
```

### 性能工具

| 组件 | 文件 | 功能 |
|---|---|---|
| CPU 绑核 | `cpu_affinity.h` | 检测核心拓扑 (BIG/MEDIUM/LITTLE)，`SetThreadAffinity()` |
| 游戏 FPS 监测 | `game_fps_monitor.h` | `dumpsys SurfaceFlinger --latency` 解析或内存 DeltaTime 滑动窗口 |
| FPS 计数器 | `main.cpp` | 仅统计同步到游戏帧的渲染帧数 |

---

## 子系统交互图

```
                    ┌─────────────────┐
                    │   内核驱动       │
                    │ (c_driver /     │
                    │  paradise)      │
                    └────────┬────────┘
                             │ ioctl / API
                    ┌────────┴────────┐
                    │  DriverManager  │  (thread_local, 无锁切换)
                    └────────┬────────┘
              ┌──────────────┼──────────────┐
              │              │              │
    ┌─────────▼──────┐ ┌────▼────────┐ ┌───▼──────────────┐
    │  读取线程       │ │  自瞄线程    │ │  主线程/渲染线程   │
    │                │ │             │ │                   │
    │ Actor 扫描     │ │ 目标选择    │ │ Vulkan 渲染       │
    │ 骨骼读取       │ │ PD 控制     │ │ ImGui 绘制        │
    │ 分类发布       │ │ 陀螺仪输出  │ │ PhysX 几何        │
    └───────┬────────┘ └──────┬──────┘ │ 深度可见性        │
            │                 │        └───────┬───────────┘
            │                 │                │
            ▼                 ▼                ▼
    ┌───────────────────────────────────────────────┐
    │              共享数据                          │
    │                                               │
    │  ClassifiedActors  (mutex)                    │
    │  BoneScreenCache   (mutex)                    │
    │  gPrunerCache      (per-frame invalidate)     │
    │  VisibilityScene   (shared_mutex + snapshot)  │
    │  IsMenuOpen        (atomic)                   │
    │  IsToolActive      (atomic)                   │
    └───────────────────────────────────────────────┘
```

---

## 关键设计模式

### 1. 线程本地驱动管理

每个线程拥有独立的 `DriverManager` 实例 (`thread_local`)，通过共享原子代数计数器 (`DriverSharedState`) 实现无锁驱动切换。读取路径零互斥锁开销。

### 2. 游戏帧同步渲染

渲染循环轮询 `GFrameCounter`（游戏内存地址），仅在新游戏帧到达时触发渲染，实现 Overlay 与引擎帧率 1:1 对齐。无新帧时按目标 FPS 回退渲染 UI。

### 3. 两阶段渲染分离

```
waitForPreviousFrame()  ──── GPU Fence 等待
        │
  ReadGameData()        ──── Fence 释放后立即读取，最小化数据年龄
        │
  submitAndPresent()    ──── 立即提交，MAILBOX 非阻塞
```

### 4. 内容哈希签名

PhysX 三角网格使用 FNV-1a 哈希（基于实际顶点/索引数据）作为签名，而非内存地址。确保：
- 同一网格在不同运行中产生相同签名
- 本地导出文件跨运行可复用
- 签名不唯一时回退到完整内存读取

### 5. SOA BVH 用于 NEON 向量化

`VisibilityScene` 将三角形按 32 个一组存储为 SOA（Structure of Arrays）布局，使 Möller-Trumbore 射线-三角形交叉检测可利用 ARM NEON SIMD 指令并行处理。

### 6. 分帧预算控制

防止单帧卡顿的预算机制：
- **几何缓存 Miss**：最多 512 次/帧 (`kGeomCacheMissBudgetPerFrame`)
- **Pruner 读取**：最多 20,000 对象/帧 (`kMaxPrunerObjectsPerFrame`)
- **三角形绘制**：最多 50,000 个/帧 (`kMaxTrianglesPerFrame`)
- **自动导出**：最多 8 个/帧 (`kAutoExportBudgetPerFrame`)
- **Actor 扫描**：最多 256 个/帧 (`gPhysXMaxActorsPerFrame`)

### 7. 异步文件加载

本地 `.obj` / `.bvh.txt` 文件通过 `std::async` 后台加载，绘制线程每帧轮询结果 (`PollAsyncLocalLoads`)。加载期间返回 nullptr，下一帧获取结果。

---

## 数据流

### 从内存到屏幕

```
游戏进程内存
    │
    ├── GUObjectArray → Actor 扫描 → CachedActor[]
    │                                    │
    │                                    ├── 骨骼数据 FTransform[] ──→ WorldToScreen ──→ 屏幕骨骼线
    │                                    ├── TeamID / Name ──→ 名称标签
    │                                    └── RootPosition ──→ 距离计算
    │
    ├── ViewProjectionMatrix ──→ 3D→2D 投影
    │
    ├── PhysX Scene Chain ──→ TriangleMesh/HeightField/ConvexMesh
    │                              │
    │                              ├── 在线读取 → 几何缓存 → ImDrawList 线框渲染
    │                              ├── 导出 .obj/.bvh.txt → 本地文件 → 异步加载
    │                              └── GPU 深度缓冲 → 遮挡查询 → visible[]
    │
    └── GFrameCounter ──→ 帧同步信号
```

### 自瞄数据流

```
BoneScreenCache (渲染线程填充)
    │
    ├── ReadFreshBoneWorldPos (实时刷新)
    │       └── SkeletalMeshComponent → FTransform → WorldToScreen
    │
    ├── SelectTarget (FOV/距离/队友/可见性过滤)
    │
    ├── ComputePDOutput
    │       ├── 误差 = 目标屏幕位置 - 屏幕中心
    │       ├── PD 输出 = Kp·误差 + Kd·误差变化率
    │       ├── Holt 预测 = level + trend·leadTime
    │       ├── 后坐力补偿 = 镜心偏移
    │       └── 噪声 = Perlin-like 平滑漂移
    │
    └── 输出
         ├── Gyro::update(x, y) ──→ 陀螺仪代理
         └── touch_down/up ──→ 触发器 Bot 开火
```
