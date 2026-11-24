#!/usr/bin/env bash
# ============================================================
# rhi_gate.sh —— RHI 回归门禁（F1；docs/6-测试验证与性能基准）
#
# 用法：
#   tools/rhi_gate.sh                # 双后端全门禁（默认构建路径）
#   APP_VK=... APP_GL=... tools/rhi_gate.sh
#
# 规则：每项 GL 与 VK 各跑一遍；VK 侧验证层 0 error 才算绿；
# 退出码非 0 即失败。CI/提交前调用。
# ============================================================
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP_GL="${APP_GL:-$ROOT/build-gl/app/Debug/my_app}"
APP_VK="${APP_VK:-$ROOT/build/app/Debug/my_app}"
export DYLD_LIBRARY_PATH="${DYLD_LIBRARY_PATH:-/opt/homebrew/lib}"   # MoltenVK loader

FAIL=0
run() {   # run <label> <bin> <env...>
    local label="$1"; local bin="$2"; shift 2
    if [[ ! -x "$bin" ]]; then
        echo "SKIP: $label（$bin 不存在）"; return
    fi
    local log; log=$(mktemp)
    # DYLD 显式传给 env：部分环境（沙箱/硬化运行时）会在 exec /usr/bin/env 时
    # 剥掉继承的 DYLD_*，导致 MoltenVK loader 找不到 ICD（vkCreateInstance -6）
    if env DYLD_LIBRARY_PATH="$DYLD_LIBRARY_PATH" "$@" "$bin" > "$log" 2>&1; then
        # VK：验证层 error 视为失败（WARN 不拦）
        if [[ "$*" == *RHI_BACKEND=vulkan* ]] && grep -qE "\[VK-VAL\].*ERROR|ERROR.*\[VK-VAL\]" "$log"; then
            echo "FAIL: $label（验证层 error，见 $log）"; FAIL=1; return
        fi
        echo "PASS: $label"
    else
        echo "FAIL: $label（exit=$?，见 $log）"; FAIL=1
    fi
    rm -f "$log"
}

echo "== GL 后端 =="
run "GL smoke"      "$APP_GL" RHI_SMOKE=1 RHI_TEXQUAD=1 RHI_EXIT_AFTER=120
run "GL rt+pick"    "$APP_GL" RHI_RT_TEST=1 RHI_PICK_TEST=1 RHI_EXIT_AFTER=120
run "GL viewports"  "$APP_GL" RHI_VIEWPORTS=4 RHI_EXIT_AFTER=120
run "GL resize"     "$APP_GL" RHI_RESIZE_AT=60 RHI_EXIT_AFTER=120

echo "== Vulkan 后端 =="
run "VK smoke"      "$APP_VK" RHI_BACKEND=vulkan RHI_VK_VALIDATION=1 RHI_SMOKE=1 RHI_TEXQUAD=1 RHI_EXIT_AFTER=120
run "VK rt+pick"    "$APP_VK" RHI_BACKEND=vulkan RHI_VK_VALIDATION=1 RHI_RT_TEST=1 RHI_PICK_TEST=1 RHI_EXIT_AFTER=120
run "VK viewports"  "$APP_VK" RHI_BACKEND=vulkan RHI_VK_VALIDATION=1 RHI_VIEWPORTS=4 RHI_EXIT_AFTER=120
run "VK resize"     "$APP_VK" RHI_BACKEND=vulkan RHI_VK_VALIDATION=1 RHI_RESIZE_AT=60 RHI_EXIT_AFTER=120
run "VK churn"      "$APP_VK" RHI_BACKEND=vulkan RHI_VK_VALIDATION=1 RHI_CHURN=200 RHI_EXIT_AFTER=120

echo "== 分层检查 =="
if cmake --build "$ROOT/build" --target check_layers > /dev/null 2>&1; then
    echo "PASS: check_layers"
else
    echo "FAIL: check_layers"; FAIL=1
fi

if [[ $FAIL -eq 0 ]]; then
    echo "ALL GREEN"
else
    echo "GATE FAILED"
    exit 1
fi
