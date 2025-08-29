LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := freetype
LOCAL_SRC_FILES := \
    freetype/src/base/ftsystem.c \
    freetype/src/base/ftinit.c \
    freetype/src/base/ftdebug.c \
    freetype/src/base/ftbase.c \
    freetype/src/base/ftbbox.c \
    freetype/src/base/ftglyph.c \
    freetype/src/base/ftbitmap.c \
    freetype/src/base/ftstroke.c \
	freetype/src/base/ftsynth.c \
    freetype/src/base/fttype1.c \
    freetype/src/base/ftmm.c \
    freetype/src/sfnt/sfnt.c \
    freetype/src/truetype/truetype.c \
    freetype/src/type1/type1.c \
    freetype/src/cff/cff.c \
    freetype/src/cid/type1cid.c \
    freetype/src/pfr/pfr.c \
    freetype/src/type42/type42.c \
    freetype/src/winfonts/winfnt.c \
    freetype/src/pcf/pcf.c \
    freetype/src/bdf/bdf.c \
    freetype/src/sdf/sdf.c \
    freetype/src/psaux/psaux.c \
	freetype/src/svg/svg.c \
	freetype/src/bzip2/ftbzip2.c \
	freetype/src/gzip/ftgzip.c \
	freetype/src/lzw/ftlzw.c \
	freetype/src/svg/ftsvg.c \
    freetype/src/pshinter/pshinter.c \
    freetype/src/psnames/psnames.c \
    freetype/src/raster/raster.c \
    freetype/src/smooth/smooth.c \
    freetype/src/autofit/autofit.c

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/freetype/include

# 打开常见选项（比如 emoji 要用 Freetype 的 color font 支持）
LOCAL_CFLAGS := -DFT_CONFIG_OPTION_USE_PNG -DFT_CONFIG_OPTION_SUBPIXEL_RENDERING
LOCAL_CFLAGS := -DFT_CONFIG_OPTION_USE_MODULE_ERRORS=0
LOCAL_CFLAGS := -DFT2_BUILD_LIBRARY
include $(BUILD_STATIC_LIBRARY)

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
LOCAL_C_INCLUDES += $(LOCAL_PATH)/freetype/include
LOCAL_C_INCLUDES += $(LOCAL_PATH)/imgui/backends
LOCAL_C_INCLUDES += $(LOCAL_PATH)/imgui
LOCAL_C_INCLUDES += $(LOCAL_PATH)/imgui/misc/freetype
LOCAL_C_INCLUDES += $(LOCAL_PATH)/vk-bootstrap

LOCAL_CFLAGS += -DIMGUI_ENABLE_FREETYPE
LOCAL_CFLAGS += -DIMGUI_USE_WCHAR32
LOCAL_CFLAGS += -DVK_KHR_surface
LOCAL_CFLAGS += -DVK_KHR_android_surface
LOCAL_CFLAGS += -DVK_USE_PLATFORM_ANDROID_KHR
LOCAL_CPPFLAGS += -DVK_USE_PLATFORM_ANDROID_KHR
LOCAL_CPPFLAGS += -DVK_KHR_android_surface
LOCAL_CPPFLAGS += -DVK_KHR_surface
LOCAL_CPPFLAGS += -DIMGUI_USE_WCHAR32
LOCAL_CPPFLAGS += -DIMGUI_ENABLE_FREETYPE

FILE_LIST += $(wildcard $(LOCAL_PATH)/imgui/imgui.cpp)
FILE_LIST += $(wildcard $(LOCAL_PATH)/imgui/imgui_draw.cpp)
FILE_LIST += $(wildcard $(LOCAL_PATH)/imgui/imgui_tables.cpp)
FILE_LIST += $(wildcard $(LOCAL_PATH)/imgui/imgui_widgets.cpp)
FILE_LIST += $(wildcard $(LOCAL_PATH)/imgui/backends/imgui_impl_android.cpp)
FILE_LIST += $(wildcard $(LOCAL_PATH)/imgui/backends/imgui_impl_vulkan.cpp)
FILE_LIST += $(wildcard $(LOCAL_PATH)/imgui/misc/freetype/imgui_freetype.cpp)
FILE_LIST += $(wildcard $(LOCAL_PATH)/vk-bootstrap/src/VkBootstrap.cpp)
FILE_LIST += $(wildcard $(LOCAL_PATH)/src/*.c*)


LOCAL_SRC_FILES := $(FILE_LIST:$(LOCAL_PATH)/%=%)

LOCAL_LDLIBS := -lm -ldl -lz -llog -landroid -lvulkan
LOCAL_STATIC_LIBRARIES := freetype
include $(BUILD_EXECUTABLE)