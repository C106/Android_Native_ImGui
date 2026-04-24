#!/usr/bin/env bash

NDK_PATH=~/android-ndk-r25c
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

create_self_extract_script() {
    local binary_path="$1"
    local output_script="$2"
    local binary_name
    local payload

    if [ ! -f "$binary_path" ]; then
        echo "跳过自解压脚本生成，未找到二进制: $binary_path"
        return 1
    fi

    binary_name="$(basename "$binary_path")"
    payload="$(base64 "$binary_path" | tr -d '\n')" || {
        echo "base64 编码失败，未生成自解压脚本"
        return 1
    }

    cat > "$output_script" <<EOF
#!/system/bin/sh
set -e

SCRIPT_DIR=\$(CDPATH= cd -- "\$(dirname -- "\$0")" && pwd)
TARGET_DIR=\${1:-\$SCRIPT_DIR}
TARGET_PATH="\$TARGET_DIR/$binary_name"
shift \$(( \$# > 0 ? 1 : 0 ))

mkdir -p "\$TARGET_DIR"

decode_base64() {
    if command -v base64 >/dev/null 2>&1; then
        base64 -d
        return
    fi

    if command -v toybox >/dev/null 2>&1; then
        toybox base64 -d
        return
    fi

    if command -v busybox >/dev/null 2>&1; then
        busybox base64 -d
        return
    fi

    if command -v openssl >/dev/null 2>&1; then
        openssl base64 -d -A
        return
    fi

    echo "未找到可用的 base64 解码器" >&2
    exit 1
}

cat <<'__PAYLOAD__' | decode_base64 > "\$TARGET_PATH"
$payload
__PAYLOAD__

chmod 0755 "\$TARGET_PATH"
echo "已释放: \$TARGET_PATH"
"\$TARGET_PATH" "\$@"
EOF

    chmod 0755 "$output_script"
    echo "自解压脚本: $output_script"
}

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
    create_self_extract_script \
        "$PROJECT_DIR/libs/arm64-v8a/Debugger" \
        "$PROJECT_DIR/libs/arm64-v8a/Debugger_extract.sh"
else
    echo "编译失败，未找到产物"
fi
