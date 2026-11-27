#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
自动化功能回归测试：跑各个渲染功能，验证日志 + 截图像素，输出 PASS/FAIL 报告。

用法：python scripts/run_tests.py
前置：已构建 build/win-verify/app/Debug/my_app.exe（Windows Debug）
"""
import subprocess, os, sys, re, tempfile

EXE = os.path.abspath(os.path.join(os.path.dirname(__file__), "..",
                                   "build", "win-verify", "app", "Debug", "my_app.exe"))
RESULTS = []

def run(env, name, timeout=120, must_log=None, forbid_log=None):
    e = dict(os.environ)
    e.update(env)
    e["RHI_EXIT_AFTER"] = env.get("RHI_EXIT_AFTER", "60")
    try:
        r = subprocess.run([EXE], env=e, capture_output=True, text=True, timeout=timeout,
                           encoding='utf-8', errors='ignore')
    except subprocess.TimeoutExpired:
        RESULTS.append((name, False, "超时"))
        return
    log = r.stdout + r.stderr
    ok = (r.returncode == 0)
    why = f"exit={r.returncode}"
    if must_log:
        for m in must_log:
            if m not in log:
                ok = False; why = f"缺日志 {m}"
    if forbid_log:
        for m in forbid_log:
            if re.search(m, log):
                ok = False; why = f"出现 {m}"
    RESULTS.append((name, ok, why))

def pixel_check(env, name, color_count=3):
    """跑 RHI_DUMP，分析 /tmp/frame_gl.ppm 里非背景彩色像素是否够多种"""
    e = dict(os.environ); e.update(env); e["RHI_DUMP"] = "1"
    try:
        subprocess.run([EXE], env=e, capture_output=True, text=True, timeout=120,
                       encoding='utf-8', errors='ignore')
    except subprocess.TimeoutExpired:
        RESULTS.append((name, False, "超时")); return
    ppm = os.path.join(tempfile.gettempdir(), "frame_gl.ppm")
    if not os.path.exists(ppm):
        RESULTS.append((name, False, "无截图")); return
    with open(ppm, 'rb') as f:
        f.readline(); line = f.readline().strip()
        while line.startswith(b'#'): line = f.readline().strip()
        w, h = map(int, line.split()); f.readline(); px = f.read()
    colors = {}
    for y in range(int(h*0.35), int(h*0.55)):
        for x in range(w):
            o = (y*w+x)*3
            r, g, b = px[o], px[o+1], px[o+2]
            if b > r+15: continue           # 天空
            if r>200 and g<60 and b<60: continue  # 红坐标轴
            if abs(r-g)<8 and abs(g-b)<8: continue  # 灰
            colors.setdefault((r//16, g//16, b//16), 0)
            colors[(r//16, g//16, b//16)] += 1
    # 显著彩色簇（>300 像素）数量
    big = sum(1 for c in colors.values() if c > 300)
    ok = big >= color_count
    RESULTS.append((name, ok, f"{big} 种显著彩色（需≥{color_count}）"))

def main():
    if not os.path.exists(EXE):
        print(f"找不到 {EXE}，请先构建"); sys.exit(1)

    # 1. 后端启动 + 渲染（验证层 0 error）
    run({"RHI_BACKEND": "opengl"}, "GL 启动渲染", forbid_log=[r"DEVICE LOST", r"ERROR.*shader"])
    run({"RHI_BACKEND": "vulkan"}, "VK 启动渲染(验证层0error)", forbid_log=[r"VK-VAL.*error", r"DEVICE LOST"])

    # 2. PBR 贴图（6 材质颜色）
    run({"RHI_BACKEND": "vulkan", "RHI_PBR_TEX": "1"}, "VK PBR 贴图", forbid_log=[r"VK-VAL.*error"])
    run({"RHI_BACKEND": "opengl", "RHI_PBR_TEX": "1"}, "GL PBR 贴图")
    pixel_check({"RHI_BACKEND": "opengl", "RHI_PBR_TEX": "1", "RHI_EXIT_AFTER": "40"},
                "PBR 贴图颜色(6材质)", color_count=4)

    # 3. PBR 标量（金属/非金属）
    run({"RHI_BACKEND": "vulkan", "RHI_PBR_DEMO": "1"}, "VK PBR 标量", forbid_log=[r"VK-VAL.*error"])

    # 4. 拾取命中（两端）：RHI_PICK_TEST 全图扫描命中对象 → auto-selected
    run({"RHI_BACKEND": "vulkan", "RHI_PICK_TEST": "1"},
        "VK 拾取(ID图命中)", must_log=["auto-selected"])
    run({"RHI_BACKEND": "opengl", "RHI_PICK_TEST": "1"},
        "GL 拾取(ID图命中)", must_log=["auto-selected"])

    # 5. 阴影
    run({"RHI_BACKEND": "vulkan", "RHI_SHADOW_DEMO": "1"}, "VK 阴影", forbid_log=[r"VK-VAL.*error"])

    # 6. 多线程录制（画面逐字节一致）
    def probe(env):
        e = dict(os.environ); e.update(env); e["RHI_PROBE"] = "1"; e["RHI_EXIT_AFTER"] = "40"
        r = subprocess.run([EXE], env=e, capture_output=True, text=True, timeout=120,
                           encoding='utf-8', errors='ignore')
        m = re.search(r"VKPROBE.*center bytes = ([0-9,]+)", r.stdout + r.stderr)
        return m.group(1) if m else None
    st = probe({"RHI_BACKEND": "vulkan", "RHI_STRESS": "1000"})
    mt = probe({"RHI_BACKEND": "vulkan", "RHI_MT": "4", "RHI_STRESS": "1000"})
    if st is None or mt is None:
        RESULTS.append(("多线程录制(画面一致)", False, "无PROBE"))
    elif st == mt:
        RESULTS.append(("多线程录制(画面一致)", True, f"{st} == {mt}"))
    else:
        RESULTS.append(("多线程录制(画面一致)", False, f"{st} != {mt}"))

    # 7. 冒烟三角形 / MSAA
    run({"RHI_BACKEND": "vulkan", "RHI_SMOKE": "1"}, "VK 冒烟三角形", forbid_log=[r"VK-VAL.*error"])
    run({"RHI_BACKEND": "vulkan", "RHI_RT_MSAA": "4"}, "VK 离屏MSAA", forbid_log=[r"VK-VAL.*error"])

    # 8. 交互自检（选中 → 换材质 → 点空白取消）
    run({"RHI_BACKEND": "vulkan", "RHI_AUTOTEST": "1", "RHI_EXIT_AFTER": "70"},
        "交互自检(选中/换材质/取消)", must_log=["换材质(竹子)完成", "取消选中结果 selectedId=0"])

    # 报告
    print("\n" + "="*60)
    print(f"{'结果':6} 测试项")
    print("="*60)
    npass = 0
    for name, ok, why in RESULTS:
        print(f"{'PASS' if ok else 'FAIL':6} {name}  ({why})")
        if ok: npass += 1
    print("="*60)
    print(f"通过 {npass}/{len(RESULTS)}")

if __name__ == "__main__":
    main()
