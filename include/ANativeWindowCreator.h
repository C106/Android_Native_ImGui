#ifndef A_NATIVE_WINDOW_CREATOR_H // !A_NATIVE_WINDOW_CREATOR_H
#define A_NATIVE_WINDOW_CREATOR_H

#include <android/native_window.h>
#include <android/log.h>
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <unistd.h>
#include <sys/system_properties.h>
#define ResolveMethod(ClassName, MethodName, Handle, MethodSignature)                                                                    \
    ClassName##__##MethodName = reinterpret_cast<decltype(ClassName##__##MethodName)>(symbolMethod.Find(Handle, MethodSignature));       \
    if (nullptr == ClassName##__##MethodName)                                                                                            \
    {                                                                                                                                    \
        __android_log_print(ANDROID_LOG_ERROR, "ImGui", "[-] Method not found: %s -> %s::%s", MethodSignature, #ClassName, #MethodName); \
    }
extern int secure_flag;
namespace android
{
    namespace detail
    {
        // 全局变量用于 LayerStack 监控
        static void* g_mirrorSurfaceControl = nullptr;
        static void* g_originalSurfaceControl = nullptr;
        static std::atomic<bool> g_monitorRunning{false};
        static std::thread g_monitorThread;
        static std::unordered_map<uint32_t, bool> g_processedLayerStacks;  // 已处理的 LayerStack

        // 前向声明
        void MonitorLayerStack();
        void StartLayerStackMonitor();

        namespace ui
        {
            // A LayerStack identifies a Z-ordered group of layers. A layer can only be associated to a single
            // LayerStack, but a LayerStack can be associated to multiple displays, mirroring the same content.
            struct LayerStack
            {
                uint32_t id = UINT32_MAX;
            };

            enum class Rotation
            {
                Rotation0 = 0,
                Rotation90 = 1,
                Rotation180 = 2,
                Rotation270 = 3
            };

            // A simple value type representing a two-dimensional size.
            struct Size
            {
                int32_t width = -1;
                int32_t height = -1;
            };

            // Transactional state of physical or virtual display. Note that libgui defines
            // android::DisplayState as a superset of android::ui::DisplayState.
            struct DisplayState
            {
                LayerStack layerStack;
                Rotation orientation = Rotation::Rotation0;
                Size layerStackSpaceRect;
            };

            typedef int64_t nsecs_t; // nano-seconds
            struct DisplayInfo
            {
                uint32_t w{0};
                uint32_t h{0};
                float xdpi{0};
                float ydpi{0};
                float fps{0};
                float density{0};
                uint8_t orientation{0};
                bool secure{false};
                nsecs_t appVsyncOffset{0};
                nsecs_t presentationDeadline{0};
                uint32_t viewportW{0};
                uint32_t viewportH{0};
            };

            enum class DisplayType
            {
                DisplayIdMain = 0,
                DisplayIdHdmi = 1
            };

            struct PhysicalDisplayId
            {
                uint64_t value;
            };
        }

        struct String8;

        struct LayerMetadata;

        struct Surface;

        struct SurfaceControl;

        struct SurfaceComposerClientTransaction;

        struct SurfaceComposerClient;

        template <typename any_t>
        struct StrongPointer
        {
            union
            {
                any_t *pointer;
                char padding[sizeof(std::max_align_t)];
            };

            inline any_t *operator->() const { return pointer; }
            inline any_t *get() const { return pointer; }
            inline explicit operator bool() const { return nullptr != pointer; }
        };

        struct Functionals
        {
            struct SymbolMethod
            {
                void *(*Open)(const char *filename, int flag) = nullptr;
                void *(*Find)(void *handle, const char *symbol) = nullptr;
                int (*Close)(void *handle) = nullptr;
            };

            size_t systemVersion = 13;

            void (*RefBase__IncStrong)(void *thiz, void *id) = nullptr;
            void (*RefBase__DecStrong)(void *thiz, void *id) = nullptr;

            void (*String8__Constructor)(void *thiz, const char *const data) = nullptr;
            void (*String8__Destructor)(void *thiz) = nullptr;

            void (*LayerMetadata__Constructor)(void *thiz) = nullptr;
            void (*LayerMetadata__setInt32)(void *thiz, uint32_t key, int32_t value) = nullptr;

            void (*SurfaceComposerClient__Constructor)(void *thiz) = nullptr;
            void (*SurfaceComposerClient__Destructor)(void *thiz) = nullptr;
            StrongPointer<void> (*SurfaceComposerClient__CreateSurface)(void *thiz, void *name, uint32_t w, uint32_t h, int32_t format, uint32_t flags, void *parentHandle, void *layerMetadata, uint32_t *outTransformHint) = nullptr;
            StrongPointer<void> (*SurfaceComposerClient__CreateSurface_and9)(void *thiz, void *name, uint32_t w, uint32_t h, int32_t format, uint32_t flags, void *parentHandle, int32_t windowType, int32_t ownerUid) = nullptr;
            // mirrorSurface: 非静态成员函数，需要 this 指针
            // Android 15-: sp<SurfaceControl> mirrorSurface(SurfaceControl*)
            // Android 16+: sp<SurfaceControl> mirrorSurface(SurfaceControl*, SurfaceControl* parent)
            StrongPointer<void> (*SurfaceComposerClient__MirrorSurface)(void *thiz, void *surfaceControl) = nullptr;
            StrongPointer<void> (*SurfaceComposerClient__MirrorSurface_and16)(void *thiz, void *surfaceControl, void *parent) = nullptr;
            StrongPointer<void> (*SurfaceComposerClient__GetInternalDisplayToken)() = nullptr;
            StrongPointer<void> (*SurfaceComposerClient__GetBuiltInDisplay)(ui::DisplayType type) = nullptr;
            int32_t (*SurfaceComposerClient__GetDisplayState)(StrongPointer<void> &display, ui::DisplayState *displayState) = nullptr;
            int32_t (*SurfaceComposerClient__GetDisplayInfo)(StrongPointer<void> &display, ui::DisplayInfo *displayInfo) = nullptr;
            std::vector<ui::PhysicalDisplayId> (*SurfaceComposerClient__GetPhysicalDisplayIds)() = nullptr;
            StrongPointer<void> (*SurfaceComposerClient__GetPhysicalDisplayToken)(ui::PhysicalDisplayId displayId) = nullptr;

            void (*SurfaceComposerClient__Transaction__Constructor)(void *thiz) = nullptr;
            void *(*SurfaceComposerClient__Transaction__SetLayer)(void *thiz, StrongPointer<void> &surfaceControl, int32_t z) = nullptr;
            void *(*SurfaceComposerClient__Transaction__SetLayerStack)(void *thiz, StrongPointer<void> &surfaceControl, ui::LayerStack layerStack) = nullptr;
            void *(*SurfaceComposerClient__Transaction__Reparent)(void *thiz, StrongPointer<void> &surfaceControl, StrongPointer<void> &newParent) = nullptr;
            void *(*SurfaceComposerClient__Transaction__SetTrustedOverlay)(void *thiz, StrongPointer<void> &surfaceControl, bool isTrustedOverlay) = nullptr;
            int32_t (*SurfaceComposerClient__Transaction__Apply)(void *thiz, bool synchronous, bool oneWay) = nullptr;

            int32_t (*SurfaceControl__Validate)(void *thiz) = nullptr;
            StrongPointer<Surface> (*SurfaceControl__GetSurface)(void *thiz) = nullptr;
            void (*SurfaceControl__DisConnect)(void *thiz) = nullptr;

            Functionals(const SymbolMethod &symbolMethod)
            {
                std::string systemVersionString(128, 0);

               // systemVersionString.resize(__system_property_get("ro.build.version.release", systemVersionString.data()));
				
				systemVersionString.resize(__system_property_get("ro.build.version.release", (char*)systemVersionString.data()));
  if (!systemVersionString.empty())
                    systemVersion = std::stoi(systemVersionString);

                if (9 > systemVersion)
                {
                    __android_log_print(ANDROID_LOG_ERROR, "ImGui", "[-] Unsupported system version: %zu", systemVersion);
                    return;
                }

                static std::unordered_map<size_t, std::unordered_map<void **, const char *>> patchesTable = {
                    {
                        16,
                        {
                            { reinterpret_cast<void**>(&LayerMetadata__Constructor),
                            "_ZN7android3gui13LayerMetadataC2Ev" },
                            { reinterpret_cast<void**>(&SurfaceComposerClient__CreateSurface),
                            "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjiiRKNS_2spINS_7IBinderEEENS_3gui13LayerMetadataEPj" },
                        },
                    },
                    {
                        15,
                        {
                            { reinterpret_cast<void**>(&LayerMetadata__Constructor),
                            "_ZN7android3gui13LayerMetadataC2Ev" },
                            { reinterpret_cast<void**>(&SurfaceComposerClient__CreateSurface),
                            "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjiiRKNS_2spINS_7IBinderEEENS_3gui13LayerMetadataEPj" },
                        },
                    },
                    {
                        14,
                        {
                            {reinterpret_cast<void **>(&LayerMetadata__Constructor), "_ZN7android3gui13LayerMetadataC2Ev"},
                            {reinterpret_cast<void **>(&SurfaceComposerClient__CreateSurface), "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjiiRKNS_2spINS_7IBinderEEENS_3gui13LayerMetadataEPj"},
                        },
                    },
                    {
                        12,
                        {
                            {reinterpret_cast<void **>(&SurfaceComposerClient__Transaction__Apply), "_ZN7android21SurfaceComposerClient11Transaction5applyEb"},
                        },
                    },
                    {
                        11,
                        {
                            {reinterpret_cast<void **>(&SurfaceComposerClient__CreateSurface), "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijPNS_14SurfaceControlENS_13LayerMetadataEPj"},
                            {reinterpret_cast<void **>(&SurfaceControl__GetSurface), "_ZNK7android14SurfaceControl10getSurfaceEv"},
                        },
                    },
                    {
                        10,
                        {
                            {reinterpret_cast<void **>(&SurfaceComposerClient__CreateSurface), "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijPNS_14SurfaceControlENS_13LayerMetadataE"},
                            {reinterpret_cast<void **>(&SurfaceControl__GetSurface), "_ZNK7android14SurfaceControl10getSurfaceEv"},
                        },
                    },
                    {
                        9,
                        {
                            {reinterpret_cast<void **>(&SurfaceComposerClient__CreateSurface_and9), "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijPNS_14SurfaceControlEii"},
                            {reinterpret_cast<void **>(&SurfaceComposerClient__GetBuiltInDisplay), "_ZN7android21SurfaceComposerClient17getBuiltInDisplayEi"},
                            {reinterpret_cast<void **>(&SurfaceControl__GetSurface), "_ZNK7android14SurfaceControl10getSurfaceEv"},
                        },
                    },
                };

#ifdef __LP64__
                auto libgui = symbolMethod.Open("/system/lib64/libgui.so", RTLD_LAZY);
                auto libutils = symbolMethod.Open("/system/lib64/libutils.so", RTLD_LAZY);
#else
                auto libgui = symbolMethod.Open("/system/lib/libgui.so", RTLD_LAZY);
                auto libutils = symbolMethod.Open("/system/lib/libutils.so", RTLD_LAZY);
#endif

                ResolveMethod(RefBase, IncStrong, libutils, "_ZNK7android7RefBase9incStrongEPKv");
                ResolveMethod(RefBase, DecStrong, libutils, "_ZNK7android7RefBase9decStrongEPKv");

                ResolveMethod(String8, Constructor, libutils, "_ZN7android7String8C2EPKc");
                ResolveMethod(String8, Destructor, libutils, "_ZN7android7String8D2Ev");

                ResolveMethod(LayerMetadata, Constructor, libgui, "_ZN7android13LayerMetadataC2Ev");
                // Android 13+ fallback: android::gui 命名空间
                if (!LayerMetadata__Constructor) {
                    LayerMetadata__Constructor = (void (*)(void *))symbolMethod.Find(libgui, "_ZN7android3gui13LayerMetadataC2Ev");
                    if (LayerMetadata__Constructor) {
                        __android_log_print(ANDROID_LOG_INFO, "ImGui", "[+] Loaded Android 13+ symbol: android::gui::LayerMetadata::Constructor");
                    }
                }

                ResolveMethod(LayerMetadata, setInt32, libgui, "_ZN7android13LayerMetadata8setInt32Eji");
                // Android 13+ fallback: android::gui 命名空间 (参数类型相同)
                if (!LayerMetadata__setInt32) {
                    LayerMetadata__setInt32 = (void (*)(void *, uint32_t, int32_t))symbolMethod.Find(libgui, "_ZN7android3gui13LayerMetadata8setInt32Eji");
                    if (LayerMetadata__setInt32) {
                        __android_log_print(ANDROID_LOG_INFO, "ImGui", "[+] Loaded Android 13+ symbol: android::gui::LayerMetadata::setInt32");
                    }
                }


                ResolveMethod(SurfaceComposerClient, Constructor, libgui, "_ZN7android21SurfaceComposerClientC2Ev");
                ResolveMethod(SurfaceComposerClient, CreateSurface, libgui, "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijRKNS_2spINS_7IBinderEEENS_13LayerMetadataEPj");
                ResolveMethod(SurfaceComposerClient, MirrorSurface, libgui, "_ZN7android21SurfaceComposerClient13mirrorSurfaceEPNS_14SurfaceControlE");
                // Android 16+ fallback: mirrorSurface 增加了 parent 参数
                if (!SurfaceComposerClient__MirrorSurface) {
                    SurfaceComposerClient__MirrorSurface_and16 = reinterpret_cast<decltype(SurfaceComposerClient__MirrorSurface_and16)>(
                        symbolMethod.Find(libgui, "_ZN7android21SurfaceComposerClient13mirrorSurfaceEPNS_14SurfaceControlES2_"));
                    if (SurfaceComposerClient__MirrorSurface_and16) {
                        __android_log_print(ANDROID_LOG_INFO, "ImGui", "[+] Loaded Android 16+ symbol: SurfaceComposerClient::MirrorSurface (with parent)");
                    }
                }
                // Android 13+ fallback: android::gui::LayerMetadata
                if (!SurfaceComposerClient__CreateSurface) {
                    SurfaceComposerClient__CreateSurface = reinterpret_cast<decltype(SurfaceComposerClient__CreateSurface)>(
                        symbolMethod.Find(libgui, "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjiiRKNS_2spINS_7IBinderEEENS_3gui13LayerMetadataEPj"));
                    if (SurfaceComposerClient__CreateSurface) {
                        __android_log_print(ANDROID_LOG_INFO, "ImGui", "[+] Loaded Android 13+ symbol: SurfaceComposerClient::CreateSurface (gui::LayerMetadata)");
                    }
                }
                ResolveMethod(SurfaceComposerClient, GetInternalDisplayToken, libgui, "_ZN7android21SurfaceComposerClient23getInternalDisplayTokenEv");
                ResolveMethod(SurfaceComposerClient, GetDisplayState, libgui, "_ZN7android21SurfaceComposerClient15getDisplayStateERKNS_2spINS_7IBinderEEEPNS_2ui12DisplayStateE");
                ResolveMethod(SurfaceComposerClient, GetDisplayInfo, libgui, "_ZN7android21SurfaceComposerClient14getDisplayInfoERKNS_2spINS_7IBinderEEEPNS_11DisplayInfoE");
                ResolveMethod(SurfaceComposerClient, GetPhysicalDisplayIds, libgui, "_ZN7android21SurfaceComposerClient21getPhysicalDisplayIdsEv");
                ResolveMethod(SurfaceComposerClient, GetPhysicalDisplayToken, libgui, "_ZN7android21SurfaceComposerClient23getPhysicalDisplayTokenENS_17PhysicalDisplayIdE");

                ResolveMethod(SurfaceComposerClient__Transaction, Constructor, libgui, "_ZN7android21SurfaceComposerClient11TransactionC2Ev");
                ResolveMethod(SurfaceComposerClient__Transaction, SetLayer, libgui, "_ZN7android21SurfaceComposerClient11Transaction8setLayerERKNS_2spINS_14SurfaceControlEEEi");
                ResolveMethod(SurfaceComposerClient__Transaction, SetLayerStack, libgui, "_ZN7android21SurfaceComposerClient11Transaction13setLayerStackERKNS_2spINS_14SurfaceControlEEENS_2ui10LayerStackE");
                ResolveMethod(SurfaceComposerClient__Transaction, Reparent, libgui, "_ZN7android21SurfaceComposerClient11Transaction8reparentERKNS_2spINS_14SurfaceControlEEES6_");
                ResolveMethod(SurfaceComposerClient__Transaction, SetTrustedOverlay, libgui, "_ZN7android21SurfaceComposerClient11Transaction17setTrustedOverlayERKNS_2spINS_14SurfaceControlEEEb");
                ResolveMethod(SurfaceComposerClient__Transaction, Apply, libgui, "_ZN7android21SurfaceComposerClient11Transaction5applyEbb");

                ResolveMethod(SurfaceControl, Validate, libgui, "_ZNK7android14SurfaceControl8validateEv");
                ResolveMethod(SurfaceControl, GetSurface, libgui, "_ZN7android14SurfaceControl10getSurfaceEv");
                ResolveMethod(SurfaceControl, DisConnect, libgui, "_ZN7android14SurfaceControl10disconnectEv");
                
                auto it = patchesTable.find(systemVersion);
                if (it != patchesTable.end()) {
                    for (const auto &[patchTo, signature] : patchesTable.at(systemVersion))
                    {
                        *patchTo = symbolMethod.Find(libgui, signature);
                        if (nullptr != *patchTo)
                            continue;

                        __android_log_print(ANDROID_LOG_ERROR, "ImGui", "[-] Patch method not found: %s", signature);
                    }
                }

                symbolMethod.Close(libutils);
                symbolMethod.Close(libgui);
            }

            static const Functionals &GetInstance(const SymbolMethod &symbolMethod = {.Open = dlopen, .Find = dlsym, .Close = dlclose}) {
                static Functionals functionals(symbolMethod);
                return functionals;
            }
        };

        struct String8
        {
            char data[1024];

            String8(const char *const string)
            {
                Functionals::GetInstance().String8__Constructor(data, string);
            }

            ~String8()
            {
                Functionals::GetInstance().String8__Destructor(data);
            }

            operator void *()
            {
                return reinterpret_cast<void *>(data);
            }
        };

        struct LayerMetadata {
            char data[1024];

            LayerMetadata() {
                if (9 < Functionals::GetInstance().systemVersion) {
                    Functionals::GetInstance().LayerMetadata__Constructor(data);
                }
            }
            
            void setInt32(uint32_t key, int32_t value) {
                Functionals::GetInstance().LayerMetadata__setInt32(data, key, value);            
            }
            
            operator void *() {
                if (9 < Functionals::GetInstance().systemVersion)
                    return reinterpret_cast<void *>(data);
                else
                    return nullptr;
            }
        };

        struct Surface
        {
        };

        struct SurfaceControl
        {
            void *data;
            void *mirrorData;  // 镜像层，用于截图/录屏捕获

            SurfaceControl() : data(nullptr), mirrorData(nullptr) {}
            SurfaceControl(void *data) : data(data), mirrorData(nullptr) {}

            int32_t Validate()
            {
                if (nullptr == data)
                    return 0;

                return Functionals::GetInstance().SurfaceControl__Validate(data);
            }

            Surface *GetSurface()
            {
                if (nullptr == data)
                    return nullptr;

                auto result = Functionals::GetInstance().SurfaceControl__GetSurface(data);

                return reinterpret_cast<Surface *>(reinterpret_cast<size_t>(result.pointer) + sizeof(std::max_align_t) / 2);
            }

            void DisConnect()
            {
                if (nullptr == data)
                    return;

                Functionals::GetInstance().SurfaceControl__DisConnect(data);
            }

            void DestroySurface(Surface *surface)
            {
                if (nullptr == data || nullptr == surface)
                    return;

                // 清理镜像层
                if (mirrorData != nullptr) {
                    Functionals::GetInstance().RefBase__DecStrong(mirrorData, this);
                    mirrorData = nullptr;
                }

                Functionals::GetInstance().RefBase__DecStrong(reinterpret_cast<Surface *>(reinterpret_cast<size_t>(surface) - sizeof(std::max_align_t) / 2), this);
                DisConnect();
                Functionals::GetInstance().RefBase__DecStrong(data, this);
            }
        };

        struct SurfaceComposerClientTransaction
        {
            char data[1024];

            SurfaceComposerClientTransaction()
            {
                Functionals::GetInstance().SurfaceComposerClient__Transaction__Constructor(data);
            }

            void *SetLayer(StrongPointer<void> &surfaceControl, int32_t z)
            {
                return Functionals::GetInstance().SurfaceComposerClient__Transaction__SetLayer(data, surfaceControl, z);
            }

            void *SetLayerStack(StrongPointer<void> &surfaceControl, ui::LayerStack layerStack)
            {
                return Functionals::GetInstance().SurfaceComposerClient__Transaction__SetLayerStack(data, surfaceControl, layerStack);
            }

            void *Reparent(StrongPointer<void> &surfaceControl, StrongPointer<void> &newParent)
            {
                return Functionals::GetInstance().SurfaceComposerClient__Transaction__Reparent(data, surfaceControl, newParent);
            }

            void *SetTrustedOverlay(StrongPointer<void> &surfaceControl, bool isTrustedOverlay)
            {
                return Functionals::GetInstance().SurfaceComposerClient__Transaction__SetTrustedOverlay(data, surfaceControl, isTrustedOverlay);
            }

            int32_t Apply(bool synchronous, bool oneWay)
            {
                if (12 >= Functionals::GetInstance().systemVersion)
                    return reinterpret_cast<int32_t (*)(void *, bool)>(Functionals::GetInstance().SurfaceComposerClient__Transaction__Apply)(data, synchronous);
                else
                    return Functionals::GetInstance().SurfaceComposerClient__Transaction__Apply(data, synchronous, oneWay);
            }
        };

        struct SurfaceComposerClient
        {
            char data[1024];

            SurfaceComposerClient()
            {
                Functionals::GetInstance().SurfaceComposerClient__Constructor(data);
                Functionals::GetInstance().RefBase__IncStrong(data, this);
            }

            SurfaceControl CreateSurface(const char *name, int32_t width, int32_t height, bool skipScrenshot) {
                void *parentHandle = nullptr;  // 默认 nullptr，让 Surface 进入标准层级
                String8 windowName(name);
                LayerMetadata layerMetadata;

                // 【核心修复】Android 10+ 必须赋予合法的窗口身份
                // 2u = METADATA_WINDOW_TYPE, 2038 = TYPE_APPLICATION_OVERLAY (悬浮窗)
                // 只有拥有合法身份，Android 14 的截图服务才会将其纳入捕获范围
                if (Functionals::GetInstance().systemVersion >= 10) {
                    if (skipScrenshot) {
                        layerMetadata.setInt32(2u, 441731);  // 防截屏自定义类型
                    } else {
                        layerMetadata.setInt32(2u, 2038);    // 允许截屏的标准悬浮窗类型
                        layerMetadata.setInt32(1u, getuid());
                    }
                }

                // 【核心修复】初始 Flags 必须为 0！高版本系统对未知 bit 极度敏感
                uint32_t flags = 0x0;

                // 明确指定安全位
                if (skipScrenshot && Functionals::GetInstance().systemVersion >= 12) {
                    if (secure_flag == 1) {
                        flags |= 0x80;  // eSecure: 直接黑屏/提示无法截图
                    } else if (secure_flag == 2) {
                        flags |= 0x40;  // eSkipScreenshot: 肉眼可见，截图/录屏中消失
                    }
                } else if (skipScrenshot) {
                    flags |= 0x80;  // 低版本只支持 eSecure
                }

                // Android 12+ 需要 fakeParent，否则 createSurface 会崩溃
                if (12 <= Functionals::GetInstance().systemVersion) {
                    static void *fakeParentHandleForBinder = nullptr;
                    parentHandle = &fakeParentHandleForBinder;
                }

                /**************************************************************************
                eFX_NODRAW：不绘制内容到 Surface 上。
                eFX_DRAW_PBO：使用 PBO（Pixel Buffer Object）进行离屏渲染。
                eFX_DRAW_FBO：使用 FBO（Frame Buffer Object）进行离屏渲染。
                eFX_DRAW_TEX：将 Surface 作为纹理使用。
                eFX_READ_COLOR：允许从 Surface 读取颜色数据。
                eFX_WRITE_COLOR：允许向 Surface 写入颜色数据。
                eFX_CONTENT_ALPHA：设置 Surface 的内容透明度。
                eFX_CONTENT_TRANSPARENT：设置 Surface 的内容为透明。
                eFX_HW_CODEC：使用硬件编解码器对 Surface 进行操作。
                eFX_MULTI_BUFFER：为 Surface 创建多个缓冲区。
                eFX_SAVE_STATE：保存 Surface 的状态。
                eFX_RESTORE_STATE：恢复 Surface 的状态。
                eFX_RELEASE_ALL_MEMORY：释放 Surface 占用的所有内存。
                eFX_RELEASE_ALL_SHARED_MEMORY：释放 Surface 占用的所有共享内存。
                eFX_RELEASE_ALL_RESOURCES：释放 Surface 占用的所有资源。
                
                eFX_DRAW_TEXTURE：将 Surface 作为纹理绘制到其他 Surface 上。
                eFX_SHADE_RGB8：使用 8 位 RGB 色彩模式进行阴影处理。
                eFX_SHADE_RGB565：使用 565 色彩模式进行阴影处理。
                eFX_SHADE_RGBA_5551：使用 5551 色彩模式进行阴影处理。
                eFX_SHADE_RGBA_4444：使用 4444 色彩模式进行阴影处理。
                eFX_SHADE_RGBA_8888：使用 8888 色彩模式进行阴影处理。
                eFX_BLEND_MODE：设置 Surface 的混合模式。
                eFX_ROTATE：旋转 Surface。
                eFX_MIRROR_HORIZONTALLY：水平镜像 Surface。
                eFX_MIRROR_VERTICALLY：垂直镜像 Surface。
                eFX_PREMULTIPLY_ALPHA：预乘 Surface 的 alpha 通道。
                eFX_POSTMULTIPLY_ALPHA：后乘 Surface 的 alpha 通道。
                eFX_CONSTANT_COLOR：设置 Surface 的常量颜色。
                eFX_CLEAR_COLOR：清除 Surface 的颜色。
                eFX_FLIP_VERTICAL：垂直翻转 Surface。
                eFX_FLIP_HORIZONTAL：水平翻转 Surface。
                
                eFX_NODRAW = 0x01
                eFX_DRAW_PBO = 0x80
                eFX_DRAW_FBO = 0x40
                eFX_DRAW_TEX = 0x20
                eFX_READ_COLOR = 0x10
                eFX_WRITE_COLOR = 0x08
                eFX_CONTENT_ALPHA = 0x04
                eFX_CONTENT_TRANSPARENT = 0x02
                eFX_HW_CODEC = 0x01
                eFX_MULTI_BUFFER = 0x80
                eFX_SAVE_STATE = 0x40
                eFX_RESTORE_STATE = 0x20
                eFX_RELEASE_ALL_MEMORY = 0x10
                eFX_RELEASE_ALL_SHARED_MEMORY = 0x08
                eFX_RELEASE_ALL_RESOURCES = 0x04
                
                eFX_DRAW_TEXTURE: 0x20
                eFX_SHADE_RGB8: 0x01
                eFX_SHADE_RGB565: 0x02
                eFX_SHADE_RGBA_5551: 0x03
                eFX_SHADE_RGBA_4444: 0x04
                eFX_SHADE_RGBA_8888: 0x05
                eFX_BLEND_MODE: 0x06
                eFX_ROTATE: 0x07
                eFX_MIRROR_HORIZONTALLY: 0x08
                eFX_MIRROR_VERTICALLY: 0x09
                eFX_PREMULTIPLY_ALPHA: 0x0A
                eFX_POSTMULTIPLY_ALPHA: 0x0B
                eFX_CONSTANT_COLOR: 0x0C
                eFX_CLEAR_COLOR: 0x0D
                eFX_FLIP_VERTICAL: 0x0E
                eFX_FLIP_HORIZONTAL: 0x0F
                ***********************************************************  ********/


                StrongPointer<void> result;
                if (Functionals::GetInstance().systemVersion == 9) {
                    // Android 9: 使用 windowType 参数
                    int32_t windowType = skipScrenshot ? 441731 : 2038;
                    int32_t ownerUid = -1;
                    result = Functionals::GetInstance().SurfaceComposerClient__CreateSurface_and9(data, windowName, width, height, 1, flags, parentHandle, windowType, ownerUid);
                } else if (Functionals::GetInstance().systemVersion >= 10) {
                    // Android 10+: 使用 LayerMetadata (包含 2038 身份)
                    result = Functionals::GetInstance().SurfaceComposerClient__CreateSurface(data, windowName, width, height, 1, flags, parentHandle, layerMetadata, nullptr);
                }

                SurfaceControl sc{result.get()};

                if (12 <= Functionals::GetInstance().systemVersion && result.get() != nullptr) {
                    static SurfaceComposerClientTransaction transaction;

                    // 【关键】将 Surface 设置到 LayerStack 0（主显示）
                    // 这样 Surface 才会显示在屏幕上，而不是 Offscreen
                    StrongPointer<void> surfaceSP;
                    surfaceSP.pointer = result.get();

                    ui::LayerStack mainLayerStack;
                    mainLayerStack.id = 0;  // LayerStack 0 = 主显示

                    transaction.SetLayerStack(surfaceSP, mainLayerStack);
                    transaction.SetLayer(surfaceSP, INT32_MAX);  // 设置到��上层
                    transaction.SetTrustedOverlay(surfaceSP, true);
                    transaction.Apply(false, true);

                    __android_log_print(ANDROID_LOG_INFO, "ImGui", "[+] Surface set to LayerStack 0 (main display)");
                }

                // 【mirrorSurface】创建镜像层，让 Surface 进入系统合成链，从而可被截图/录屏捕获
                if (!skipScrenshot && result.get() != nullptr &&
                    (Functionals::GetInstance().SurfaceComposerClient__MirrorSurface ||
                     Functionals::GetInstance().SurfaceComposerClient__MirrorSurface_and16)) {

                    __android_log_print(ANDROID_LOG_INFO, "ImGui", "[*] mirrorSurface: this=%p, SurfaceControl=%p",
                        data, result.get());

                    int32_t valid = Functionals::GetInstance().SurfaceControl__Validate(result.get());
                    __android_log_print(ANDROID_LOG_INFO, "ImGui", "[*] SurfaceControl validate result: %d", valid);

                    if (valid != 0) {
                        __android_log_print(ANDROID_LOG_WARN, "ImGui", "[-] SurfaceControl invalid, skipping mirror");
                    } else {
                        __android_log_print(ANDROID_LOG_INFO, "ImGui", "[*] Calling mirrorSurface (this=%p, sc=%p)...", data, result.get());

                        // 调用 mirrorSurface（根据 Android 版本选择签名）
                        StrongPointer<void> mirrorResult;
                        if (Functionals::GetInstance().SurfaceComposerClient__MirrorSurface) {
                            // Android 15-: mirrorSurface(SurfaceControl*)
                            mirrorResult = Functionals::GetInstance().SurfaceComposerClient__MirrorSurface(data, result.get());
                        } else if (Functionals::GetInstance().SurfaceComposerClient__MirrorSurface_and16) {
                            // Android 16+: mirrorSurface(SurfaceControl*, SurfaceControl* parent)
                            // parent 传 nullptr，让镜像层成为顶层
                            mirrorResult = Functionals::GetInstance().SurfaceComposerClient__MirrorSurface_and16(data, result.get(), nullptr);
                        }

                        __android_log_print(ANDROID_LOG_INFO, "ImGui", "[*] mirrorSurface returned: %p", mirrorResult.get());

                        if (mirrorResult.get() != nullptr) {
                            sc.mirrorData = mirrorResult.get();

                            // 保存原始层和镜像层指针
                            detail::g_originalSurfaceControl = result.get();
                            detail::g_mirrorSurfaceControl = mirrorResult.get();

                            static void* mirrorRef = nullptr;
                            Functionals::GetInstance().RefBase__IncStrong(mirrorResult.get(), &mirrorRef);

                            StrongPointer<void> mirrorSP;
                            mirrorSP.pointer = mirrorResult.get();
                            SurfaceComposerClientTransaction mirrorTransaction;
                            mirrorTransaction.SetLayer(mirrorSP, INT32_MAX - 1);
                            mirrorTransaction.Apply(false, true);

                            // 启动 LayerStack 监控线程
                            detail::StartLayerStackMonitor();

                            __android_log_print(ANDROID_LOG_INFO, "ImGui", "[+] Mirror surface created successfully");
                        } else {
                            __android_log_print(ANDROID_LOG_WARN, "ImGui", "[-] mirrorSurface returned null");
                        }
                    }
                }

                return sc;
            }

            bool GetDisplayInfo(ui::DisplayState *displayInfo)
            {
                StrongPointer<void> defaultDisplay;

                if (9 >= Functionals::GetInstance().systemVersion)
                    defaultDisplay = Functionals::GetInstance().SurfaceComposerClient__GetBuiltInDisplay(ui::DisplayType::DisplayIdMain);
                else
                {
                    if (14 > Functionals::GetInstance().systemVersion)
                        defaultDisplay = Functionals::GetInstance().SurfaceComposerClient__GetInternalDisplayToken();
                    else
                    {
                        auto displayIds = Functionals::GetInstance().SurfaceComposerClient__GetPhysicalDisplayIds();
                        if (displayIds.empty())
                            return false;

                        defaultDisplay = Functionals::GetInstance().SurfaceComposerClient__GetPhysicalDisplayToken(displayIds[0]);
                    }
                }

                if (nullptr == defaultDisplay.get())
                    return false;

                if (11 <= Functionals::GetInstance().systemVersion)
                    return 0 == Functionals::GetInstance().SurfaceComposerClient__GetDisplayState(defaultDisplay, displayInfo);
                else
                {
                    ui::DisplayInfo realDisplayInfo{};
                    if (0 != Functionals::GetInstance().SurfaceComposerClient__GetDisplayInfo(defaultDisplay, &realDisplayInfo))
                        return false;

                    displayInfo->layerStackSpaceRect.width = realDisplayInfo.w;
                    displayInfo->layerStackSpaceRect.height = realDisplayInfo.h;
                    displayInfo->orientation = static_cast<ui::Rotation>(realDisplayInfo.orientation);
                    
                    return true;
                }
            }
        };

    }

    class ANativeWindowCreator
    {
    public:
        struct DisplayInfo
        {
            int32_t orientation;
            int32_t width;
            int32_t height;
        };

    private:
        inline static std::unordered_map<ANativeWindow *, detail::SurfaceControl> m_cachedSurfaceControl;

    public:
        static detail::SurfaceComposerClient &GetComposerInstance()
        {
            static detail::SurfaceComposerClient surfaceComposerClient;

            return surfaceComposerClient;
        }

        static DisplayInfo GetDisplayInfo()
        {
            auto &surfaceComposerClient = GetComposerInstance();
            detail::ui::DisplayState displayInfo{};

            if (!surfaceComposerClient.GetDisplayInfo(&displayInfo))
                return {};

            DisplayInfo local_displayInfo{0};
            int32_t local_orientation = static_cast<int32_t>(displayInfo.orientation);
            int32_t local_abs_x = (displayInfo.layerStackSpaceRect.width > displayInfo.layerStackSpaceRect.height ? displayInfo.layerStackSpaceRect.width : displayInfo.layerStackSpaceRect.height);
            int32_t local_abs_y = (displayInfo.layerStackSpaceRect.width < displayInfo.layerStackSpaceRect.height ? displayInfo.layerStackSpaceRect.width : displayInfo.layerStackSpaceRect.height);
            if (local_orientation == 1 || local_orientation == 3) {
                local_displayInfo.width = local_abs_x;
                local_displayInfo.height = local_abs_y;
            } else {
                local_displayInfo.width = local_abs_y;
                local_displayInfo.height = local_abs_x;
            }
            local_displayInfo.orientation = local_orientation;
            return local_displayInfo;
        }

        static ANativeWindow *Create(const char *name, int32_t width = -1, int32_t height = -1, bool skipScrenshot_ = false)
        {
            auto &surfaceComposerClient = GetComposerInstance();

            while (-1 == width || -1 == height)
            {
                detail::ui::DisplayState displayInfo{};

                if (!surfaceComposerClient.GetDisplayInfo(&displayInfo))
                    break;

                width = displayInfo.layerStackSpaceRect.width;
                height = displayInfo.layerStackSpaceRect.height;

                break;
            }

            auto surfaceControl = surfaceComposerClient.CreateSurface(name, width, height, skipScrenshot_);
            auto nativeWindow = reinterpret_cast<ANativeWindow *>(surfaceControl.GetSurface());

            m_cachedSurfaceControl.emplace(nativeWindow, std::move(surfaceControl));
            return nativeWindow;
        }

        static void Destroy(ANativeWindow *nativeWindow)
        {
            auto it = m_cachedSurfaceControl.find(nativeWindow);
            if (it == m_cachedSurfaceControl.end())
                return;

            // 重置镜像状态，让监控线程重新处理所有 LayerStack
            __android_log_print(ANDROID_LOG_INFO, "ImGui", "[*] Resetting mirror state (clearing processed LayerStacks)");
            detail::g_processedLayerStacks.clear();

            it->second.DestroySurface(reinterpret_cast<detail::Surface *>(nativeWindow));
            m_cachedSurfaceControl.erase(it);
        }
    };

    // LayerStack 监控函数实现
    namespace detail
    {
        // 通过 dumpsys 获取所有 LayerStack ID
        inline std::vector<uint32_t> GetAllLayerStacks()
        {
            std::vector<uint32_t> layerStacks;

            // 执行 dumpsys SurfaceFlinger
            FILE* pipe = popen("dumpsys SurfaceFlinger 2>/dev/null", "r");
            if (!pipe) {
                __android_log_print(ANDROID_LOG_ERROR, "ImGui", "[-] Failed to execute dumpsys");
                return layerStacks;
            }

            char buffer[256];
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                // 查找 "LayerStack=" 行
                if (strstr(buffer, "LayerStack=") != nullptr) {
                    uint32_t layerStackId = 0;
                    if (sscanf(buffer, "LayerStack=%u", &layerStackId) == 1) {
                        // 去重
                        if (std::find(layerStacks.begin(), layerStacks.end(), layerStackId) == layerStacks.end()) {
                            layerStacks.push_back(layerStackId);
                        }
                    }
                }
            }

            pclose(pipe);
            return layerStacks;
        }

        inline void MonitorLayerStack()
        {
            // 使用堆分配的 SurfaceComposerClient，避免 Android 16 RefBase 栈指针检测
            static SurfaceComposerClient* persistentClient = nullptr;
            if (!persistentClient) {
                persistentClient = new SurfaceComposerClient();
            }

            while (g_monitorRunning.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(2));

                if (!g_mirrorSurfaceControl || !g_originalSurfaceControl) {
                    continue;
                }

                // 通过 dumpsys 获取所有 LayerStack
                auto layerStacks = GetAllLayerStacks();

                __android_log_print(ANDROID_LOG_DEBUG, "ImGui", "[*] Found %zu LayerStacks", layerStacks.size());

                for (uint32_t layerStackId : layerStacks) {
                    // 跳过主显示（LayerStack 0）和已处理的 LayerStack
                    if (layerStackId == 0 || g_processedLayerStacks.count(layerStackId) > 0) {
                        continue;
                    }

                    __android_log_print(ANDROID_LOG_INFO, "ImGui", "[*] Detected new LayerStack: %u", layerStackId);

                    // 为新的 LayerStack 创建新的镜像层
                    StrongPointer<void> newMirrorResult;
                    if (Functionals::GetInstance().SurfaceComposerClient__MirrorSurface) {
                        newMirrorResult = Functionals::GetInstance().SurfaceComposerClient__MirrorSurface(
                            persistentClient->data, g_originalSurfaceControl);
                    } else if (Functionals::GetInstance().SurfaceComposerClient__MirrorSurface_and16) {
                        newMirrorResult = Functionals::GetInstance().SurfaceComposerClient__MirrorSurface_and16(
                            persistentClient->data, g_originalSurfaceControl, nullptr);
                    }

                    if (newMirrorResult.get() == nullptr) {
                        __android_log_print(ANDROID_LOG_ERROR, "ImGui", "[-] Failed to create mirror for LayerStack %u", layerStackId);
                        continue;
                    }

                    __android_log_print(ANDROID_LOG_INFO, "ImGui", "[*] Created new mirror: %p for LayerStack %u",
                        newMirrorResult.get(), layerStackId);

                    // 保持引用
                    Functionals::GetInstance().RefBase__IncStrong(newMirrorResult.get(), newMirrorResult.get());

                    // 创建宿主层
                    auto hostSurface = persistentClient->CreateSurface("MirrorHost", 1, 1, false);
                    if (hostSurface.data == nullptr) {
                        __android_log_print(ANDROID_LOG_ERROR, "ImGui", "[-] Failed to create host surface");
                        continue;
                    }

                    StrongPointer<void> hostSP;
                    hostSP.pointer = hostSurface.data;

                    StrongPointer<void> newMirrorSP;
                    newMirrorSP.pointer = newMirrorResult.get();

                    ui::LayerStack targetLayerStack;
                    targetLayerStack.id = layerStackId;

                    // 设置宿主层到新的 LayerStack
                    SurfaceComposerClientTransaction transaction;
                    transaction.SetLayerStack(hostSP, targetLayerStack);
                    transaction.SetLayer(hostSP, INT32_MAX - 2);

                    // 将新镜像层 reparent 到宿主层下
                    transaction.Reparent(newMirrorSP, hostSP);
                    transaction.SetLayer(newMirrorSP, INT32_MAX - 1);
                    transaction.Apply(false, true);

                    __android_log_print(ANDROID_LOG_INFO, "ImGui", "[+] New mirror reparented to LayerStack %u", layerStackId);
                    g_processedLayerStacks[layerStackId] = true;
                }
            }
        }

        inline void StartLayerStackMonitor()
        {
            if (g_monitorRunning.load()) {
                return;
            }

            g_monitorRunning.store(true);
            g_monitorThread = std::thread(MonitorLayerStack);
            g_monitorThread.detach();
            __android_log_print(ANDROID_LOG_INFO, "ImGui", "[+] LayerStack monitor started");
        }

        inline void ResetMirrorState()
        {
            __android_log_print(ANDROID_LOG_INFO, "ImGui", "[*] Resetting mirror state (clearing processed LayerStacks)");
            g_processedLayerStacks.clear();
            // 注意：不清空 g_mirrorSurfaceControl 和 g_originalSurfaceControl
            // 因为它们会在新的 Create 调用中被更新
        }
    }
}

#undef ResolveMethod

#endif // !A_NATIVE_WINDOW_CREATOR_H
