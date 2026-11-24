#!/usr/bin/env bash
# ============================================================
# package_mac.sh —— macOS .app 打包（F2 基础版）
#
# 用法：
#   scripts/package_mac.sh [gl|vulkan]     # 默认 vulkan
#
# 产物：dist/MyDuck.app——双击/直接运行，不依赖 DYLD_LIBRARY_PATH。
# Vulkan 版：libvulkan loader 拷入 Frameworks 并改 @executable_path；
# 启动器注入 RHI_VK_VALIDATION=0（验证层 dylib 随 SDK 装在 Homebrew
# 路径，bundle 运行环境无该搜索路径；开发调试用 build/ 目录 +
# RHI_VK_VALIDATION=1）。MoltenVK ICD 经系统 icd.d 自动发现（需目标
# 机已安装 molten-vk）。
# ============================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-vulkan}"
APP_NAME="MyDuck"
DIST="$ROOT/dist"
BIN_VK="$ROOT/build/app/Debug/my_app"
BIN_GL="$ROOT/build-gl/app/Debug/my_app"

if [[ "$MODE" == "vulkan" ]]; then
    BIN="$BIN_VK"
else
    BIN="$BIN_GL"
fi
if [[ ! -x "$BIN" ]]; then
    echo "错误：$BIN 不存在，请先 cmake --build 对应 preset" >&2
    exit 1
fi

rm -rf "$DIST/$APP_NAME.app"
mkdir -p "$DIST/$APP_NAME.app/Contents/MacOS" \
         "$DIST/$APP_NAME.app/Contents/Resources" \
         "$DIST/$APP_NAME.app/Contents/Frameworks"

cp "$BIN" "$DIST/$APP_NAME.app/Contents/MacOS/$APP_NAME"
cp -R "$ROOT/res" "$DIST/$APP_NAME.app/Contents/Resources/res"

cat > "$DIST/$APP_NAME.app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>$APP_NAME</string>
    <key>CFBundleIdentifier</key><string>com.example.myduck</string>
    <key>CFBundleName</key><string>$APP_NAME</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

if [[ "$MODE" == "vulkan" ]]; then
    # Vulkan loader（动态库）拷入 Frameworks 并改 @executable_path
    VK_LIB="$(otool -L "$BIN" | grep -oE '/[^ ]*libvulkan[^ ]*\.dylib' | head -1 || true)"
    if [[ -n "${VK_LIB:-}" && -f "$VK_LIB" ]]; then
        cp "$VK_LIB" "$DIST/$APP_NAME.app/Contents/Frameworks/"
        VL_BASENAME="$(basename "$VK_LIB")"
        install_name_tool -change "$VK_LIB" \
            "@executable_path/../Frameworks/$VL_BASENAME" \
            "$DIST/$APP_NAME.app/Contents/MacOS/$APP_NAME"
        echo "已内嵌 Vulkan loader: $VL_BASENAME"

        # 启动器：真二进制改名，启动器默认关验证层（bundle 内无 SDK 层路径）
        mv "$DIST/$APP_NAME.app/Contents/MacOS/$APP_NAME" \
           "$DIST/$APP_NAME.app/Contents/MacOS/$APP_NAME.real"
        cat > "$DIST/$APP_NAME.app/Contents/MacOS/$APP_NAME" <<LAUNCH
#!/bin/bash
HERE="\$(cd "\$(dirname "\$0")" && pwd)"
export RHI_VK_VALIDATION=0
exec "\$HERE/$APP_NAME.real" "\$@"
LAUNCH
        chmod +x "$DIST/$APP_NAME.app/Contents/MacOS/$APP_NAME"
        echo "已生成启动器（默认 RHI_VK_VALIDATION=0）"
    else
        echo "警告：未找到 libvulkan 动态库引用（GL-only 构建？）"
    fi
fi

# 签名（ad-hoc，本地可运行；分发需正式证书）
codesign --force --deep --sign - "$DIST/$APP_NAME.app" > /dev/null 2>&1 || true

echo "打包完成：$DIST/$APP_NAME.app"
echo "运行：open $DIST/$APP_NAME.app"
