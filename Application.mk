APP_ABI := arm64-v8a
APP_PLATFORM := android-24
APP_STL := c++_shared

# Keep symbols and frame pointers in native builds so perf/simpleperf can
# resolve stacks reliably without forcing a full debug build.
APP_CFLAGS += -g -fno-omit-frame-pointer
APP_CPPFLAGS += -g -fno-omit-frame-pointer
APP_LDFLAGS += -Wl,--build-id
APP_STRIP_MODE := none
