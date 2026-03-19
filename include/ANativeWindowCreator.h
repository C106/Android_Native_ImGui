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
#include <jni.h>
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
        // 全局变量用于 LayerStack 监控（inline 保证跨 TU 唯一实例）
        inline void* g_mirrorSurfaceControl = nullptr;
        inline void* g_originalSurfaceControl = nullptr;
        inline std::unordered_map<uint32_t, bool> g_processedLayerStacks;  // 已处理的 LayerStack

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

                void *surfaceRef = reinterpret_cast<Surface *>(reinterpret_cast<size_t>(surface) - sizeof(std::max_align_t) / 2);

                // 清理镜像层
                if (mirrorData != nullptr) {
                    // mirrorData is retained with itself as the strong-ref id to avoid
                    // RefBase rejecting transient stack addresses.
                    Functionals::GetInstance().RefBase__DecStrong(mirrorData, mirrorData);
                    mirrorData = nullptr;
                }

                Functionals::GetInstance().RefBase__DecStrong(surfaceRef, surfaceRef);
                DisConnect();
                Functionals::GetInstance().RefBase__DecStrong(data, data);
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

        // 通过 JNI 获取 StatusBar 窗口 Token 的 SurfaceControl，用于 reparent
        // 仅在 system_server 进程内有效（system uid = 1000）
        struct StatusBarReparent
        {
            // 获取 StatusBar WindowToken 的 native SurfaceControl 指针
            // 路径: WMS.mRoot → DisplayContent → DisplayPolicy.mStatusBar → parent (WindowToken) → SurfaceControl.mNativeObject
            static void* FindStatusBarTokenSurfaceControl()
            {
                // 1. 获取 JavaVM
                typedef jint (*GetVMs_t)(JavaVM**, jsize, jsize*);
                auto getVMs = reinterpret_cast<GetVMs_t>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
                if (!getVMs) {
                    __android_log_print(ANDROID_LOG_WARN, "ImGui", "[-] StatusBar: JNI_GetCreatedJavaVMs not found");
                    return nullptr;
                }

                JavaVM* vm = nullptr;
                jsize vmCount = 0;
                if (getVMs(&vm, 1, &vmCount) != JNI_OK || !vm || vmCount == 0) {
                    __android_log_print(ANDROID_LOG_WARN, "ImGui", "[-] StatusBar: No JavaVM available");
                    return nullptr;
                }

                JNIEnv* env = nullptr;
                bool needDetach = false;
                jint envResult = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
                if (envResult == JNI_EDETACHED) {
                    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
                    needDetach = true;
                } else if (envResult != JNI_OK) {
                    return nullptr;
                }

                void* result = nullptr;

                // lambda: 统一清理
                auto clearException = [&]() {
                    if (env->ExceptionCheck()) env->ExceptionClear();
                };

                do {
                    // 2. 检查是否在 system_server（尝试加载 WMS 类）
                    jclass wmsClass = env->FindClass("com/android/server/wm/WindowManagerService");
                    if (!wmsClass || env->ExceptionCheck()) {
                        clearException();
                        __android_log_print(ANDROID_LOG_WARN, "ImGui", "[-] StatusBar: Not in system_server, cannot access WMS");
                        break;
                    }

                    // 3. 通过 ServiceManager 获取 window 服务（在同进程中就是 WMS 实例本身）
                    jclass smClass = env->FindClass("android/os/ServiceManager");
                    if (!smClass) { clearException(); break; }

                    jmethodID getService = env->GetStaticMethodID(smClass, "getService",
                        "(Ljava/lang/String;)Landroid/os/IBinder;");
                    if (!getService) { clearException(); break; }

                    jstring wmStr = env->NewStringUTF("window");
                    jobject wms = env->CallStaticObjectMethod(smClass, getService, wmStr);
                    env->DeleteLocalRef(wmStr);
                    if (!wms || env->ExceptionCheck()) { clearException(); break; }

                    // 4. WMS.mRoot → RootWindowContainer
                    jfieldID mRootField = env->GetFieldID(wmsClass, "mRoot",
                        "Lcom/android/server/wm/RootWindowContainer;");
                    if (!mRootField || env->ExceptionCheck()) { clearException(); break; }

                    jobject root = env->GetObjectField(wms, mRootField);
                    if (!root) break;

                    // 5. RootWindowContainer.getDefaultDisplay() → DisplayContent
                    jclass rwcClass = env->FindClass("com/android/server/wm/RootWindowContainer");
                    if (!rwcClass || env->ExceptionCheck()) { clearException(); break; }

                    jmethodID getDefaultDisplay = env->GetMethodID(rwcClass, "getDefaultDisplay",
                        "()Lcom/android/server/wm/DisplayContent;");
                    if (!getDefaultDisplay || env->ExceptionCheck()) { clearException(); break; }

                    jobject dc = env->CallObjectMethod(root, getDefaultDisplay);
                    if (!dc || env->ExceptionCheck()) { clearException(); break; }

                    // 6. DisplayContent.getDisplayPolicy() → DisplayPolicy
                    jclass dcClass = env->FindClass("com/android/server/wm/DisplayContent");
                    if (!dcClass || env->ExceptionCheck()) { clearException(); break; }

                    jmethodID getDP = env->GetMethodID(dcClass, "getDisplayPolicy",
                        "()Lcom/android/server/wm/DisplayPolicy;");
                    if (!getDP || env->ExceptionCheck()) { clearException(); break; }

                    jobject dp = env->CallObjectMethod(dc, getDP);
                    if (!dp || env->ExceptionCheck()) { clearException(); break; }

                    // 7. DisplayPolicy.mStatusBar → WindowState
                    jclass dpClass = env->FindClass("com/android/server/wm/DisplayPolicy");
                    if (!dpClass || env->ExceptionCheck()) { clearException(); break; }

                    jfieldID mStatusBar = env->GetFieldID(dpClass, "mStatusBar",
                        "Lcom/android/server/wm/WindowState;");
                    if (!mStatusBar || env->ExceptionCheck()) {
                        clearException();
                        // Android 版本差异: 尝试备用字段名
                        mStatusBar = env->GetFieldID(dpClass, "mStatusBarWin",
                            "Lcom/android/server/wm/WindowState;");
                        if (!mStatusBar || env->ExceptionCheck()) { clearException(); break; }
                    }

                    jobject statusBarWin = env->GetObjectField(dp, mStatusBar);
                    if (!statusBarWin) {
                        __android_log_print(ANDROID_LOG_WARN, "ImGui", "[-] StatusBar: mStatusBar window is null (StatusBar not yet created?)");
                        break;
                    }

                    // 8. WindowState.getParent() → WindowToken（StatusBar 的窗口 Token 节点）
                    jclass wcClass = env->FindClass("com/android/server/wm/WindowContainer");
                    if (!wcClass || env->ExceptionCheck()) { clearException(); break; }

                    jmethodID getParent = env->GetMethodID(wcClass, "getParent",
                        "()Lcom/android/server/wm/WindowContainer;");
                    if (!getParent || env->ExceptionCheck()) { clearException(); break; }

                    jobject statusBarToken = env->CallObjectMethod(statusBarWin, getParent);
                    if (!statusBarToken || env->ExceptionCheck()) { clearException(); break; }

                    // 9. WindowToken.getSurfaceControl() → Java SurfaceControl
                    jmethodID getSC = env->GetMethodID(wcClass, "getSurfaceControl",
                        "()Landroid/view/SurfaceControl;");
                    if (!getSC || env->ExceptionCheck()) { clearException(); break; }

                    jobject javaSC = env->CallObjectMethod(statusBarToken, getSC);
                    if (!javaSC || env->ExceptionCheck()) { clearException(); break; }

                    // 10. SurfaceControl.mNativeObject → native SurfaceControl*
                    jclass scClass = env->FindClass("android/view/SurfaceControl");
                    if (!scClass || env->ExceptionCheck()) { clearException(); break; }

                    jfieldID nativeField = env->GetFieldID(scClass, "mNativeObject", "J");
                    if (!nativeField || env->ExceptionCheck()) { clearException(); break; }

                    jlong nativePtr = env->GetLongField(javaSC, nativeField);
                    result = reinterpret_cast<void*>(static_cast<uintptr_t>(nativePtr));

                    if (result) {
                        __android_log_print(ANDROID_LOG_INFO, "ImGui",
                            "[+] StatusBar WindowToken SurfaceControl found: %p", result);
                    }
                } while (false);

                if (needDetach) vm->DetachCurrentThread();
                return result;
            }

            // 将 child reparent 到 statusBarParent 下
            static bool Reparent(StrongPointer<void>& child, void* statusBarParent)
            {
                if (!child.get() || !statusBarParent) return false;

                StrongPointer<void> parentSP;
                parentSP.pointer = statusBarParent;

                SurfaceComposerClientTransaction transaction;
                transaction.Reparent(child, parentSP);
                transaction.SetLayer(child, INT32_MAX);
                transaction.Apply(false, true);

                __android_log_print(ANDROID_LOG_INFO, "ImGui",
                    "[+] ESK reparented under StatusBar WindowToken");
                return true;
            }
        };

        struct SurfaceComposerClient
        {
            char data[1024];
            void *strongRefId = nullptr;

            SurfaceComposerClient()
            {
                Functionals::GetInstance().SurfaceComposerClient__Constructor(data);
                // Use a heap token as the strong-ref id. tempClient is often stack-allocated
                // in DetectAndCreateVirtualDisplayMirrors(), and RefBase rejects stack ids.
                strongRefId = new char;
                Functionals::GetInstance().RefBase__IncStrong(data, strongRefId);
            }

            ~SurfaceComposerClient()
            {
                if (strongRefId != nullptr) {
                    Functionals::GetInstance().RefBase__DecStrong(data, strongRefId);
                    delete static_cast<char *>(strongRefId);
                    strongRefId = nullptr;
                }
            }

            SurfaceControl CreateSurface(const char *name, int32_t width, int32_t height, bool skipScrenshot) {
                String8 windowName(name);
                LayerMetadata layerMetadata;

                // 【核心】使用 system uid (1000) 伪装为状态栏窗口
                // 2u = METADATA_WINDOW_TYPE, 1u = METADATA_OWNER_UID
                if (Functionals::GetInstance().systemVersion >= 10) {
                    if (skipScrenshot) {
                        layerMetadata.setInt32(2u, 441731);  // 防截屏自定义类型
                    } else {
                        layerMetadata.setInt32(2u, 2000);    // TYPE_STATUS_BAR - 状态栏窗口类型
                        layerMetadata.setInt32(1u, 1000);    // system uid
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

                // 【关键发现】使用 display token 作为 parent 会导致 Surface 成为 Display 的直接子层
                // 但正确的层级应该是：Display → WindowToken → Surface
                // 使用 fakeParent (nullptr) 会让系统自动将 Surface 放置在正确的 WindowToken 下
                void *parentHandle = nullptr;

                if (12 <= Functionals::GetInstance().systemVersion) {
                    // 必须使用 fakeParent，让系统自动处理层级
                    static void *fakeParentHandleForBinder = nullptr;
                    parentHandle = &fakeParentHandleForBinder;
                    __android_log_print(ANDROID_LOG_INFO, "ImGui", "[*] Using fakeParent for correct layer hierarchy");
                }

                StrongPointer<void> result;
                if (Functionals::GetInstance().systemVersion == 9) {
                    // Android 9: 使用 windowType 参数
                    int32_t windowType = skipScrenshot ? 441731 : 2000;  // TYPE_STATUS_BAR
                    int32_t ownerUid = 1000;  // system uid
                    result = Functionals::GetInstance().SurfaceComposerClient__CreateSurface_and9(data, windowName, width, height, 1, flags, parentHandle, windowType, ownerUid);
                } else if (Functionals::GetInstance().systemVersion >= 10) {
                    // Android 10+: 使用 LayerMetadata (包含 2038 身份)
                    result = Functionals::GetInstance().SurfaceComposerClient__CreateSurface(data, windowName, width, height, 1, flags, parentHandle, layerMetadata, nullptr);
                }

                SurfaceControl sc{result.get()};

                if (12 <= Functionals::GetInstance().systemVersion && result.get() != nullptr) {
                    StrongPointer<void> surfaceSP;
                    surfaceSP.pointer = result.get();

                    bool reparented = false;

                    // 【优先】尝试获取 StatusBar WindowToken 并 reparent 到其下
                    // 仅在 system_server 进程中有效
                    if (!skipScrenshot) {
                        void* statusBarHandle = StatusBarReparent::FindStatusBarTokenSurfaceControl();
                        if (statusBarHandle) {
                            reparented = StatusBarReparent::Reparent(surfaceSP, statusBarHandle);
                        }
                    }

                    if (!reparented) {
                        // 【回退】直接设置到 LayerStack 0（主显示）
                        SurfaceComposerClientTransaction transaction;

                        ui::LayerStack mainLayerStack;
                        mainLayerStack.id = 0;

                        transaction.SetLayerStack(surfaceSP, mainLayerStack);
                        transaction.SetLayer(surfaceSP, INT32_MAX);
                        transaction.SetTrustedOverlay(surfaceSP, true);
                        transaction.Apply(false, true);

                        __android_log_print(ANDROID_LOG_INFO, "ImGui", "[+] Surface set to LayerStack 0 (fallback, reparent unavailable)");
                    }
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

                            // Keep an extra strong ref with a stable non-stack id.
                            Functionals::GetInstance().RefBase__IncStrong(mirrorResult.get(), mirrorResult.get());
                            ui::LayerStack mainLayerStack;
                            mainLayerStack.id = 0;
                            StrongPointer<void> mirrorSP;
                            mirrorSP.pointer = mirrorResult.get();
                            SurfaceComposerClientTransaction mirrorTransaction;
                            mirrorTransaction.SetLayerStack(mirrorSP, mainLayerStack);
                            mirrorTransaction.SetLayer(mirrorSP, INT32_MAX - 1);
                            mirrorTransaction.Apply(false, true);
                            
                            // 【优化】禁用 LayerStack 监控以降低 CPU 占用
                            // dumpsys SurfaceFlinger 是重操作，每 5 秒执行一次会导致高 CPU 占用
                            // 大多数情况下不需要监控（只有录屏时才需要多个 LayerStack）
                            // 如果需要，可以手动启用：detail::StartLayerStackMonitor();
                            // detail::StartLayerStackMonitor();

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
                __android_log_print(ANDROID_LOG_ERROR, "ANativeWindowCreator", "[-] Failed to execute dumpsys");
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

        // 检测并创建虚拟显示镜像（用于录屏支持）
        // 返回值：创建的新镜像数量，-1 表示错误
        static int DetectAndCreateVirtualDisplayMirrors()
        {
            // 一次性扫描 dumpsys，检测新的 LayerStack 并创建 mirror
            auto layerStacks = detail::GetAllLayerStacks();
            __android_log_print(ANDROID_LOG_INFO, "ANativeWindowCreator", "[*] Found %zu LayerStacks", layerStacks.size());

            int newCount = 0;
            for (uint32_t layerStackId : layerStacks) {
                // 跳过主显示（LayerStack 0）和已处理的 LayerStack
                if (layerStackId == 0 || detail::g_processedLayerStacks.count(layerStackId) > 0) {
                    continue;
                }

                if (!detail::g_mirrorSurfaceControl || !detail::g_originalSurfaceControl) {
                    __android_log_print(ANDROID_LOG_WARN, "ANativeWindowCreator", "[-] Mirror surface not initialized");
                    return -1;
                }

                __android_log_print(ANDROID_LOG_INFO, "ANativeWindowCreator", "[*] Detected new LayerStack: %u", layerStackId);

                // Reuse the process-wide composer instance here. A stack-allocated
                // wrapper places the real RefBase object on stack-backed storage and
                // can still trip RefBase's stack-pointer guard on some builds.
                auto &tempClient = GetComposerInstance();

                // 为新的 LayerStack 创建新的镜像层
                detail::StrongPointer<void> newMirrorResult;
                if (detail::Functionals::GetInstance().SurfaceComposerClient__MirrorSurface) {
                    newMirrorResult = detail::Functionals::GetInstance().SurfaceComposerClient__MirrorSurface(
                        tempClient.data, detail::g_originalSurfaceControl);
                } else if (detail::Functionals::GetInstance().SurfaceComposerClient__MirrorSurface_and16) {
                    newMirrorResult = detail::Functionals::GetInstance().SurfaceComposerClient__MirrorSurface_and16(
                        tempClient.data, detail::g_originalSurfaceControl, nullptr);
                }

                if (newMirrorResult.get() == nullptr) {
                    __android_log_print(ANDROID_LOG_ERROR, "ANativeWindowCreator", "[-] Failed to create mirror for LayerStack %u", layerStackId);
                    continue;
                }

                __android_log_print(ANDROID_LOG_INFO, "ANativeWindowCreator", "[*] Created new mirror: %p for LayerStack %u",
                    newMirrorResult.get(), layerStackId);

                // 保持引用
                detail::Functionals::GetInstance().RefBase__IncStrong(newMirrorResult.get(), newMirrorResult.get());

                // 创建宿主层
                auto hostSurface = tempClient.CreateSurface("MirrorHost", 1, 1, false);
                if (hostSurface.data == nullptr) {
                    __android_log_print(ANDROID_LOG_ERROR, "ANativeWindowCreator", "[-] Failed to create host surface");
                    continue;
                }

                detail::StrongPointer<void> hostSP;
                hostSP.pointer = hostSurface.data;

                detail::StrongPointer<void> newMirrorSP;
                newMirrorSP.pointer = newMirrorResult.get();

                detail::ui::LayerStack targetLayerStack;
                targetLayerStack.id = layerStackId;

                detail::SurfaceComposerClientTransaction transaction;
                transaction.SetLayerStack(hostSP, targetLayerStack);
                transaction.SetLayer(hostSP, INT32_MAX - 2);
                transaction.Reparent(newMirrorSP, hostSP);
                transaction.SetLayer(newMirrorSP, INT32_MAX - 1);
                transaction.Apply(false, true);

                detail::g_processedLayerStacks[layerStackId] = true;
                newCount++;

                __android_log_print(ANDROID_LOG_INFO, "ANativeWindowCreator", "[+] Successfully created mirror for LayerStack %u", layerStackId);
            }

            if (newCount > 0) {
                __android_log_print(ANDROID_LOG_INFO, "ANativeWindowCreator", "[+] Created %d new mirror(s)", newCount);
            } else {
                __android_log_print(ANDROID_LOG_INFO, "ANativeWindowCreator", "[*] No new LayerStack detected");
            }

            return newCount;
        }
    };
}

#undef ResolveMethod

#endif // !A_NATIVE_WINDOW_CREATOR_H
