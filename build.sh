

NDK_PATH=~/android-ndk-r25c
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

echo "=== 编译 Android 项目 ==="
echo "NDK: $NDK_PATH"
echo "ABI: arm64-v8a"
echo ""

# 仅首次编译时清理
if [ ! -d "libs" ]; then
    echo "首次编译，清理旧产物..."
    rm -rf libs obj
    mkdir -p libs
fi

# 增量编译（ndk-build 自动只编译更改的文件）
"$NDK_PATH/ndk-build" \
    NDK_PROJECT_PATH=. \
    APP_BUILD_SCRIPT=Android.mk \
    APP_ABI="arm64-v8a" \
    APP_PLATFORM=android-24 \
    APP_STL=c++_static

echo ""
echo "=== 编译完成 ==="
if [ -f "libs/arm64-v8a/Debugger" ]; then
    echo "产物: $PROJECT_DIR/libs/arm64-v8a/Debugger"
    ls -lh libs/arm64-v8a/Debugger
else
    echo "编译失败，未找到产物"
fi
