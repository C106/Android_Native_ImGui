SurfaceControl CreateSurface(const char *name, int32_t width, int32_t height, bool skipScrenshot) {
    void *parentHandle = nullptr;
    String8 windowName(name);
    LayerMetadata layerMetadata;
    
    // 1. 【核心修复】：Android 14+ 必须赋予合法的窗口身份！
    // 2u 对应 METADATA_WINDOW_TYPE，2038 对应 TYPE_APPLICATION_OVERLAY (悬浮窗)
    // 只有拥有合法身份，Android 14 的截图服务才会将其纳入捕获范围。
    if (Functionals::GetInstance().systemVersion >= 10) {
        if (skipScrenshot) {
            layerMetadata.setInt32(2u, 441731); // 你原来的防截屏自定义类型
        } else {
            layerMetadata.setInt32(2u, 2038);   // 允许截屏时的标准类型
        }
    }

    // 2. 【核心修复】：初始 Flags 必须为 0！
    // 坚决不要用 0x20，高版本系统对未知 bit 极度敏感。
    uint32_t flags = 0x0; 

    // 3. 明确指定安全位
    if (skipScrenshot && Functionals::GetInstance().systemVersion >= 12) {
        if (secure_flag == 1) {
            flags |= 0x80; // eSecure: 直接黑屏/提示无法截图
        } else if (secure_flag == 2) {
            flags |= 0x40; // eSkipScreenshot: 肉眼可见，截图/录屏中消失
        }
    } else if (skipScrenshot) {
        flags |= 0x80;     // 低版本只支持 eSecure
    }

    // 处理 Android 12+ 的父节点限制
    if (12 <= Functionals::GetInstance().systemVersion) {
        static void *fakeParentHandleForBinder = nullptr;
        parentHandle = &fakeParentHandleForBinder;
    }
                    
    StrongPointer<void> result;
    if (Functionals::GetInstance().systemVersion == 9) {
        int32_t windowType = skipScrenshot ? 441731 : 2038;
        result = Functionals::GetInstance().SurfaceComposerClient__CreateSurface_and9(data, windowName, width, height, 1, flags, parentHandle, windowType, -1);                
    } else if (Functionals::GetInstance().systemVersion >= 10) {
        // 将包含了 2038 身份的 metadata 传给系统
        result = Functionals::GetInstance().SurfaceComposerClient__CreateSurface(data, windowName, width, height, 1, flags, parentHandle, layerMetadata, nullptr);
    }
    
    if (12 <= Functionals::GetInstance().systemVersion && result != nullptr) {
        static SurfaceComposerClientTransaction transaction;
        // TrustedOverlay 允许保留，它可以防止底层触摸被拦截，且不影响截屏
        transaction.SetTrustedOverlay(result, true);
        transaction.Apply(false, true);
    }
    
    return {result.get()};
}