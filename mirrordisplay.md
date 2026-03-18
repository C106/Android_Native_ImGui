🧩 Android 原生 Surface 镜像方案 —— 让 Surface 在系统截图中可见
🎯 目标

在不依赖 Java/WindowManager 的前提下，让你通过 SurfaceComposerClient::CreateSurface 创建的 原生 Surface
✔ 显示在屏幕上
✔ 能够被系统截图/录屏（比如 MediaProjection / scrcpy）捕获

🧠 背景

Android 的截图机制 不直接读取屏幕像素，而是通过 SurfaceFlinger 合成一个 buffer 来取得最终图像。传统的 WindowManager/Activity 窗口自动进入这个合成链，而你自行创建的 SurfaceControl 默认不会纳入该路径。为了解决这个问题，需要把 Surface 的内容引入截图合成链。

📌 本方案核心思想

使用 SurfaceComposerClient::mirrorSurface 接口，将你创建的 Surface 的内容做一个 镜像副本。这个镜像层在合成管线中可被系统截图/录屏捕获。

SurfaceFlinger 的 API 定义如下：

sp<SurfaceControl> SurfaceComposerClient::mirrorSurface(SurfaceControl* mirrorFromSurface);
sp<SurfaceControl> SurfaceComposerClient::mirrorDisplay(DisplayId displayId);

它会创建一个 新 SurfaceControl，将原始 Surface 的内容进行了镜像层级复制。

📦 使用条件 & 要点

✔ 适合原生创建的 Surface，也可用于虚拟显示场景
✔ 镜像出来的 Surface 会进入系统合成树
✔ 该层支持截图/录屏（MediaProjection / scrcpy 等）捕获
✔ 不依赖 Java/Activity/WindowManager

🛠️ 动态解析 Symbols

在你现有的符号解析机制中补充下面两个方法：

ResolveMethod(SurfaceComposerClient, MirrorSurface, libgui,
 "_ZN7android21SurfaceComposerClient13mirrorSurfaceEPNS_14SurfaceControlE");

ResolveMethod(SurfaceComposerClient, MirrorDisplay, libgui,
 "_ZN7android21SurfaceComposerClient13mirrorDisplayENS_9DisplayIdE");

解析后你可以通过 Functionals::GetInstance() 调用这些函数。

📋 整体实现步骤
1) 创建原始 Surface（你已有）
auto surfaceControl = surfaceComposerClient.CreateSurface(name, width, height, skipScreenshot_);

注意 surfaceControl 是你的原始控制对象。

2) 对原始 Surface 进行镜像

成功解析符号后可调用：

auto mirrorSC = Functionals::GetInstance().SurfaceComposerClient__MirrorSurface(
    /* 参数填 SurfaceControl 的内部 handle */ originalHandle
);

这个镜像 mirrorSC 就是进入合成树的副本。

3) 在 Transaction 中设置镜像层显示
detail::SurfaceComposerClientTransaction transaction;
transaction.SetLayer(mirrorSC, INT32_MAX - 1); // 放到最上层
transaction.Show(mirrorSC);
transaction.Apply(false, true);

这样镜像层就会被合成并可被截图/录屏捕捉到。

📌 虚拟显示 & 深度镜像扩展（可选）

除了镜像单个 Surface，还可以镜像整个显示：

auto mirrorDisplaySC = Functionals::GetInstance().SurfaceComposerClient__MirrorDisplay(displayId);

用于更复杂的全屏层合成。

💡 理解合成链

Android 截图/录屏是通过 SurfaceFlinger 合成管线来生成图像，而镜像层是这个合成链的一部分。所以：

❗ 只有被合成进管线的 Surface 才能被截图/录屏成功捕获。

mirrorSurface() 的作用就是将你的原生 Surface 通过复制的方式加入该合成路径。

⚠️ 注意事项

✅ 镜像层仍可能受安全策略/FLAG_SECURE 等影响
⚠ 如果原始 Surface 使用某些安全标志，可能需要调整 layer flags 以允许镜像层显示/截图
⚠ 解决方案中不涉及 Java 层创建 WindowManager 类型的窗口

📌 示例伪代码整合
// 动态解析 Symbols
ResolveMethod(SurfaceComposerClient, MirrorSurface, libgui, "...mirrorSurface...");
ResolveMethod(SurfaceComposerClient, MirrorDisplay, libgui, "...mirrorDisplay...");

// 创建原始 Surface
auto surfaceControl = surfaceComposerClient.CreateSurface(...);

// 镜像
auto mirrorSC = Functionals::GetInstance().SurfaceComposerClient__MirrorSurface(
    /* SurfaceControl 内部对象 */ surfaceControlHandle
);

// 在 Transaction 中显示镜像层
detail::SurfaceComposerClientTransaction transaction;
transaction.SetLayer(mirrorSC, INT32_MAX);
transaction.Show(mirrorSC);
transaction.Apply(false, true);
📁 优点

✨ 不依赖 Java/Activity
✨ 可进入截图/录屏合成链
✨ 适用于 Android 10+（适配 Android14+ 合成过滤）

🧾 结语

通过 mirrorSurface() 你可以让原始 native 层在系统截图中可见，这是现阶段解决 Android14+ 原生 Surface 截图不可见的主流方案。