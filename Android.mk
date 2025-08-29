LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_CPP_EXTENSION := .cpp .cc
LOCAL_MODULE := hp.sh
#LOCAL_ARM_MODE := arm

#LOCAL_CFLAGS := -w -s -Wno-error=format-security -fvisibility=hidden -fpermissive -fexceptions
#LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-all,-llog
#LOCAL_CPPFLAGS += -w -s -Wno-error=format-security -fvisibility=hidden -Werror -std=c++17
#LOCAL_CPPFLAGS += -Wno-error=c++11-narrowing -fpermissive -Wall -fexceptions
#LOCAL_CFLAGS := -std=c++17

LOCAL_CPPFLAGS += -fexceptions

LOCAL_C_INCLUDES := $(LOCAL_PATH)/Vulkan-Headers/include
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include
LOCAL_C_INCLUDES += $(LOCAL_PATH)/imgui/backends
LOCAL_C_INCLUDES += $(LOCAL_PATH)/imgui
LOCAL_C_INCLUDES += $(LOCAL_PATH)/vk-bootstrap

LOCAL_CFLAGS += -DVK_KHR_surface
LOCAL_CFLAGS += -DVK_KHR_android_surface
LOCAL_CFLAGS += -DVK_USE_PLATFORM_ANDROID_KHR
LOCAL_CPPFLAGS += -DVK_USE_PLATFORM_ANDROID_KHR
LOCAL_CPPFLAGS += -DVK_KHR_android_surface
LOCAL_CPPFLAGS += -DVK_KHR_surface


FILE_LIST += $(wildcard $(LOCAL_PATH)/imgui/imgui.cpp)
FILE_LIST += $(wildcard $(LOCAL_PATH)/imgui/imgui_draw.cpp)
FILE_LIST += $(wildcard $(LOCAL_PATH)/imgui/imgui_tables.cpp)
FILE_LIST += $(wildcard $(LOCAL_PATH)/imgui/imgui_widgets.cpp)
FILE_LIST += $(wildcard $(LOCAL_PATH)/imgui/backends/imgui_impl_android.cpp)
FILE_LIST += $(wildcard $(LOCAL_PATH)/imgui/backends/imgui_impl_vulkan.cpp)
FILE_LIST += $(wildcard $(LOCAL_PATH)/vk-bootstrap/src/VkBootstrap.cpp)
FILE_LIST += $(wildcard $(LOCAL_PATH)/src/*.c*)


LOCAL_SRC_FILES := $(FILE_LIST:$(LOCAL_PATH)/%=%)

LOCAL_LDLIBS := -lm -ldl -lz -llog -landroid -lvulkan

include $(BUILD_EXECUTABLE)