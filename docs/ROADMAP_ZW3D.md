# 面向 ZW3D 的引擎路线图（草案 v1）

> 日期：2026-11-23
> 定位：本引擎以**库**的形式嵌入 ZW3D 的渲染视图（不拥有主循环/窗口）。
> 优先级：正确性与稳定性 > CAD 显示能力 > 帧率 > 高级 Vulkan 特性。
> 平台：Windows / Linux 为主；macOS + MoltenVK 保留为开发便利，不作为产品基线。
> OpenGL 后端是产品回退路径（RDP/VDI/老显卡），不能砍。

## 当前状态（2026-11-23）

- ✅ **A1** Validation Layer + Debug Utils + 对象命名（验证层 0 error）
- ✅ **A3** VkCaps 能力探测 + Pipeline Cache 持久化 + 面板/日志能力表
  （顺带修复：pipeline layout 泄漏、退出时对象在命令缓冲中销毁、
  单例析构晚于 `glfwTerminate` 的崩溃）
- ✅ **A2** CMakePresets（mac/linux/win + GL-only）、VULKAN_SDK 探测、
  glslang 硬校验、`ENGINE_HAS_VULKAN` 对 app 可见、临时目录跨平台
  （macOS + Windows 已实机构建；Linux 待验证）
- ✅ **B0** RHI v2 契约冻结（tag `rhi-v2-contract`）：DeviceDesc/NativeWindow、
  RenderTarget、SamplerDesc、TextureDesc(mip/sRGB)、Readback、对象级销毁、
  DeviceCapabilities；GL/VK 对象销毁已落地，其余由 B1–B4 逐后端实现
- ✅ **VK 场景接线**（`8737900`）：Line 自动 RHI、天空盒开关、
  `RHI_SMOKE` / `RHI_TEXQUAD` 在场景 pass 内叠加
- ✅ **Windows 实机验证**（GTX 1050 Ti / Vulkan 1.3.280）：Vulkan 后端渲染成功、
  验证层 0 error、`RHI_PROBE` 中心像素 119,136,119,255、ImGui 面板可见（CJK 字体已加载）；
  同时修复：MSVC 代码页 936 需 `/utf-8`、只构建 app 时 SPIR-V 缺失、
  per-image render-done semaphore、`RHI_PROBE` present 后布局转换
- ✅ **D7 剖切平面**：模型着色路径（GL 330 / Vulkan 450）+ 调试面板
  + `RHI_CLIP=<距离>` 调试入口；文字/线暂不裁剪（随 D4/D1 处理）
- ✅ **B3a Y 翻转**（2026-11-23，Windows 实机）：VK 负高度 viewport
  （`VK_KHR_maintenance1` 显式启用）+ 管线 frontFace=CLOCKWISE；
  `RHI_DUMP` 行翻转 hack 移除。3D 与 UI 方向一致
- ✅ **B3b Reversed-Z + 深度范围**（同日验证）：MVP 类 mat4 在 VK setUniform
  后乘 `z'=0.5*(w-z)`（GL [-1,1] → VK [0,1] 且反转），比较算子 GREATER 族、
  清屏 0，天空盒远平面改 `z=0`；鸭子遮挡天空盒作回归断言。
  B3 剩余：sRGB 交换链 + mipmap（见 P0 表 B3c）
- ✅ **顺带修复退出段错误**（基线即崩，非 B3 引入）：`FontManager::face`
  未初始化，VK 模式不加载字体时 `FT_Done_Face(垃圾指针)` 崩溃；
  `RHI_EXIT_AFTER` 回归路径恢复可用（exit=0）
- ⚠ **已知小问题（不阻塞，已记录）**：
  1. 中文安装路径下 Assimp 打不开模型（A4：Windows UTF-8/宽字符路径）；
  2. resize 仍走“重建 PSO”路径（B1：Dynamic Rendering）；
  3. B3 剩余：sRGB 交换链 + mipmap（VK 当前 UNORM 交换链、mipLevels=1）；
  4. 光源指示鸭（objLight，scale 0.5）包住相机 → 画面顶部巨鸭弧形，
     RHI 绘制路径固有（GL selfTestAB 的 frame_rhi.ppm 同样复现），随 D 系列处理
- ✅ **收尾状态**：所有改动已提交并推送（origin/master）；
  Windows Vulkan 实机可运行，验证层 0 error；ImGui 面板可见（CJK 字体）

## 剩余工作总览（2026-11-23 收尾版，评审用）

> 这是给产品/排期看的浓缩清单；详细阶段表见第 2 节。
> 规模：S ≈ 1–2 天，M ≈ 3–5 天，L ≈ 1–2 周（单人）。

| 优先级 | ID | 工作项 | 现状 | 依赖 | 并行性 | 规模 |
|--------|----|--------|------|------|--------|------|
| P0 | B3a | ~~Y 翻转：负高度 viewport，并移除 dump 行翻转~~ | ✅ 完成（2026-11-23 实机验证） | B0 | — | S–M |
| P0 | B3b | ~~深度：Vulkan 投影 z∈[0,1] + Reversed-Z~~ | ✅ 完成（同日验证） | B3a | — | M |
| P0 | B3c | sRGB 交换链 + 纹理 sRGB/mipmap（GL/VK 视觉对齐的剩余项） | VK UNORM 交换链、mipLevels=1 | B3b | 与 A4 并行 | S–M |
| P0 | A4 | Windows UTF-8/宽字符路径（Assimp IOSystem、纹理/着色器/缓存路径） | 中文安装路径加载失败 | 无 | **可与 B3 并行**（资源/IO） | M |
| P0 | B1 | Dynamic Rendering + resize/ImGui 失效收尾 | 固定 render pass；resize 重建 PSO | B0 | 与 B3 串行（VkDevice/管线） | M |
| P0 | B2 | GL 后端 v2：render target / sampler / readback / caps | 契约已冻结，GL 未实现 | B0 | **可并行** | M |
| P0 | B4 | 离屏 RenderTarget + readback | 仅默认 framebuffer | B1 | 与 B2 并行；与 B1 串行（VkDevice） | M |
| P1 | C1 | 宿主窗口/离屏接入，引擎去 GLFW | 引擎依赖 GLFW 主循环 | B1 | 可与 C2 并行 | M |
| P1 | C2 | 多视口 | 单视口 | B0 | 可并行 | M |
| P1 | D1 | 宽线/边线 + AA + polygon offset | VK 线宽仅 1 | B3 | 可并行 | L |
| P1 | D2 | 拾取 Pass + 回读 | 无拾取回读 | B4 | 可并行 | M |
| P1 | D3/D4/D7/D8 | 透明 / SDF 文字 / 剖切 / 选中高亮 | D7 已完成；D3/D4/D8 待做 | B1/B4 | 大部分可并行 | M/S |
| P2 | E1/E2 | VMA + 异步上传环 + Timeline + fif=2 | 每次上传 `vkQueueWaitIdle` | A3 | E1→E2 串行 | M/L |
| P2 | E3/E5 | CPU 剔除 / LOD / 流式加载 | 未开始 | 无 / E1 | 可与 E1/E2 并行 | M |
| P3 | F1 | headless 测试 + CI | 无 | B4 | 可并行 | S |
| P3 | F2 | 打包 / GL 回退验证 / 设备丢失恢复 | 未开始 | C1/C4 | 部分并行 | M |

### 推荐执行顺序（评审后再定）

1. **第一波（正确性收尾）**：~~B3a → B3b~~ ✅ 已完成（2026-11-23）；
   剩余 **B3c（sRGB/mipmap）+ A4**。B3c 与本机 VK 环境可直接验证；
   A4 只动资源/IO 层，可同时进行。
2. **第二波（RHI 基座）**：B1 → B4；B2 并行。B1/B4 同改 `VkDevice`，需串行或同一人。
3. **第三波（ZW3D 接入）**：C1/C2 + D2/D4/D7；D1 等 B3 完成后做。
4. **第四波（大装配）**：E1/E2 → E3/E5。
5. **第五波（工程化）**：F1/F2。

明确不做：bindless/BDA、GPL、VRS/FSR、Ray Query、HDR、完整 RT 管线、
Vulkan Video / Mesh Shader / Shader Object（理由见第 1 节）。

## 0. 待确认假设（影响架构，尽早确认）

1. 宿主 UI 框架与窗口句柄：原生 HWND / Qt QWidget / MFC？
2. 是否需要多文档、多视图、多渲染上下文并存？
3. 目标机器的 GPU 基线（是否必须支持 RDP/VDI 的软件或 GL 路径）？
4. ZW3D 是否要求引擎以动态库形式交付、是否共享设备？
5. 大装配规模：零件数、三角面数、纹理内存上限。

在确认前，计划按以下假设推进：**渲染到宿主提供的窗口或离屏纹理；单进程单设备、多视口；GL 后端必须可用。**

## 1. 能力取舍（对照 Vulkan 特性清单）

| 特性 | 决策 | 理由 / 触发条件 |
|------|------|----------------|
| Validation Layer + VK_EXT_debug_utils | **现在做** | 当前错误大量是“静默失败”，CAD 产品不能靠猜 |
| Dynamic Rendering | **现在做** | 一次解决离屏渲染、resize 重建 PSO、ImGui renderPass 失效；对缩略图和嵌入渲染是基础 |
| Synchronization2 | **现在做** | 上传、离屏、后续流式加载的 barrier 统一 |
| VMA（或等价 suballocator） | **现在做** | 大装配资源数量多，当前每资源一次 `vkAllocateMemory` 不可持续 |
| Pipeline Cache 持久化 | **现在做** | 成本低；CAD 切换显示模式/材质时减少卡顿 |
| Reversed-Z + 深度精度 | **现在做** | CAD 视图范围大（mm~km），普通投影会 z-fighting；清单未列但 ZW3D 必需 |
| sRGB / mipmap 正确性 | **现在做** | 与 GL 视觉对齐、远景质量；VK 当前 `mipLevels=1`、无 sRGB |
| Timeline Semaphore + 异步上传环 | Phase 4 | 大模型/纹理流式加载时兑现；当前每次上传 `vkQueueWaitIdle` |
| frame-in-flight > 1、per-image depth | Phase 4 | 与上传环一起做 |
| 多队列（copy/compute 独立） | Phase 4+ | 上传环稳定后再评估；MoltenVK 单队列族 |
| MDI / MDI-count、GPU 剔除 | Phase 4+ | 大装配 + 有 GPU 剔除需求时 |
| Bindless / BDA | 观察，暂不进主线 | 需 RHI 资源寻址模型重构；必须先有性能证据，且 GL 必须能回退 |
| EDS 1 | 后续评估 | 显示模式/材质组合增多后减少 PSO 数量 |
| Subgroup | 不排期 | 先有 compute 管线 |
| GPL | **不做** | 编辑器/热更场景才需要；MoltenVK 不支持 |
| VRS（清单中误写成 FSR） | **不做** | CAD 需要画质；MoltenVK 不支持 |
| Ray Query | **不做**（远期可选） | Apple GPU 无 RT；AS 基建成本高；拾取用 ID pass 更简单 |
| HDR 交换链 | **不做**（远期可选） | CAD 无 HDR 需求；先保证 sRGB 正确 |
| Shader Object / Video / Mesh Shader | **不做** | 生态与硬件支持不足 |
| ImGui Vulkan backend | 仅示例/调试用 | ZW3D 产品 UI 由宿主负责；不作为阻塞项 |
| 阴影 / 地面 / 鸭子示例内容 | 不投入 VK 迁移 | 非产品需求；保留 GL 示例路径即可 |

## 2. 阶段与工作流

规模估算：S ≈ 1–2 天，M ≈ 3–5 天，L ≈ 1–2 周（单人、熟悉代码后）。

### Phase 0 —— 基线整备（3 条可并行）

| ID | 内容 | 规模 | 依赖 | 并行性 | 主要文件 |
|----|------|------|------|--------|----------|
| A1 | Validation Layer + Debug Utils + 对象命名 + `RHI_VK_VALIDATION` | S | 无 | 可与 A2 并行；与 A3 同改 VkDevice，**建议同一人** | `VkDevice.*`、`main.cpp` |
| A2 | 跨平台构建：CMakePresets、Vulkan SDK 探测、glslang、`ENGINE_HAS_VULKAN` 对 app 可见、`/tmp` 路径、GL-only 构建修复 | S | 无 | **可完全并行** | `CMakeLists.txt`、`src/CMakeLists.txt`、`app/*` |
| A3 | `VkCaps` 能力探测 + Pipeline Cache 持久化 + 调试面板展示 | S | A1 | 与 A1 串行或同一人 | `VkDevice.*`、`DebugPanel.*` |
| A4 | Windows UTF-8/宽字符路径：Assimp `IOSystem`、纹理/着色器/pipeline cache 路径 | M | 无 | **可与 B3 并行**（资源/IO 层） | `image.*`、`model.*`、`shader.*`、`VkDevice.*` |

验收：Windows/Linux 都能构建；GL-only 构建不报错；验证层对现有场景无 error；启动日志打印能力表；
中文安装路径下模型、纹理、着色器均可加载（A4）。

### Phase 1 —— RHI v2 契约 + 渲染正确性（先串行冻结，再并行）

| ID | 内容 | 规模 | 依赖 | 并行性 | 主要文件 |
|----|------|------|------|--------|----------|
| B0 | **接口冻结**：`DeviceDesc`（外接原生窗口/离屏）、RenderTarget、SamplerDesc、TextureDesc（格式/mip/sRGB）、readback、资源销毁、Capabilities | M | A | **串行门禁**；冻结后打 tag `rhi-v2-contract` | `src/rhi/*.h` |
| B1 | Dynamic Rendering 迁移（VK） | M | B0 | B0 之后可并行；与 B3 同改管线创建，**需串行或同人** | `VkDevice.*`、`VkCommandList.*`、`main.cpp` |
| B2 | GL 后端 v2：render target、sampler、readback、capabilities 对齐 | M | B0 | **可与 B1/B3/B4 并行** | `src/rhi/gl/*` |
| B3 | Reversed-Z + 深度范围 + sRGB + mipmap | M | B0 | 与 B1 串行（管线深度状态）；其余独立 | `camera.*`、shaders、`VkDevice.*` |
| B4 | 离屏 RenderTarget + readback（VK + GL） | M | B0 | 与 B2 并行；与 B1 串行（VkDevice） | `VkDevice.*`、`GLDevice.*` |
| B5 | ImGui VK 修复（动态渲染模式） | S | B1 | 可延后；仅调试用途 | `main.cpp` |

验收：CAD 样例场景中 GL/VK 视觉一致；窗口 resize 不再重建 PSO；近/远物体无 z-fighting；缩略图可离屏渲染并回读。

### Phase 2 —— ZW3D 嵌入与视口

| ID | 内容 | 规模 | 依赖 | 并行性 | 主要文件 |
|----|------|------|------|--------|----------|
| C1 | 宿主窗口/离屏设备接入；引擎去除对 GLFW 的硬依赖 | M | B0/B1 | 可与 C2/C4 并行 | `IRHIDevice.h`、`RenderSystem.*`、`app/main.cpp` |
| C2 | 多视口：相机/视口矩形/scissor，一套场景多视图渲染 | M | B0 | 可并行 | `camera/*`、`RenderSystem.*` |
| C3 | headless 测试工具：离屏渲染 + 哈希/截图，供多机验证 | S | B4 | 可并行 | `tools/`（新建） |
| C4 | 设备丢失 / swapchain 重建 / TDR 恢复 | M | B1 | 可并行 | `VkDevice.*` |
| C5 | 示例 app 重构为“引擎库的消费者” | S | C1 | 与 C2 可并行 | `app/*` |

验收：引擎可渲染到 ZW3D 提供的窗口/纹理；无 GLFW 依赖也能初始化；多视口正确；设备丢失后可恢复。

### Phase 3 —— CAD 显示能力

| ID | 内容 | 规模 | 依赖 | 并行性 | 主要文件 |
|----|------|------|------|--------|----------|
| D1 | 宽线/边线渲染（三角形扩展或实例化）+ 抗锯齿 + polygon offset | L | B1/B3 | 可并行 | `Line.*`、shaders、管线 |
| D2 | 拾取 Pass：ID 渲染 + 回读 + 多视口 | M | B4/C3 | 可并行 | `PickPass.*`、`VkDevice.*` |
| D3 | 透明排序 / OIT-lite | M | B1 | 可并行 | 场景/管线 |
| D4 | SDF 文字（RHI 纹理图集） | M | B0 | 可并行 | `FontManager.*`、`Text.*` |
| D5 | 显示模式（着色/线框/边线）管线变体 | M | B1 | 依赖 B1 | `MeshPass.*`、shaders |
| D6 | MSAA + 边缘 AA | M | B3/B4 | 与 D1 有管线冲突，需协调 | `VkDevice.*`、管线 |
| D7 | 剖切平面 / 裁剪面 | S | B0 | 可并行 | shaders、uniform |
| D8 | 选中高亮 / 描边 | M | D2 | 与 D2 串行或同人 | 渲染/着色器 |

验收：线框与边线清晰无断裂；拾取与多视口一致；透明正确；文字清晰；剖切可用。

### Phase 4 —— 大场景与性能

| ID | 内容 | 规模 | 依赖 | 并行性 | 主要文件 |
|----|------|------|------|--------|----------|
| E1 | VMA 接入 + memory budget 统计 | M | A3 | 与 E2 串行 | `VkDevice.*`、3rdparty |
| E2 | 异步上传环 + Timeline Semaphore + fif=2 + per-image depth | L | E1 | 串行 | `VkDevice.*`、上传模块 |
| E3 | CPU 视锥剔除 / 实例合批 / LOD | M | 无（上层） | **可与 E1/E2 并行** | 场景/MeshPass |
| E4 | GPU 剔除 + MDI-count | L | E2/E3 | 串行 | 新增模块 |
| E5 | 模型/纹理流式加载 + 预算降级 | M | E1/E2 | 与 E3 可并行 | 资源层 |
| E6 | Bindless/BDA 预研（可选） | M | 性能数据 | 独立 spike，不合并主线 | 实验分支 |

验收：大装配帧率/内存可控；上传不阻塞主线程；显存超预算时能降级不崩。

### Phase 5 —— 可选，不进基线

EDS 1、多队列、Subgroup、Ray Query 拾取、HDR、GPL。
明确不做：VRS/FSR 上采样、完整 RT 管线、Vulkan Video、Mesh/Task Shader、Shader Object。

## 3. 现有缺陷归并（2026-11-23 收尾版）

| 问题 | 状态 | 归属/说明 |
|---|---|---|
| ImGui VK 面板不可见 | ✅ 已解决（Windows 验证） | 面板可见；B1 改 Dynamic Rendering 时注意 ImGui backend |
| VK 未接 RenderSettings | ✅ 已解决（`8737900`） | sky/smoke/tex quad 开关接线 |
| Line 在 VK 静默消失 | ✅ 已解决（`8737900`） | 自动走 RHI；宽线质量仍由 D1 提升 |
| GL-only 构建坏 / CMake macOS 路径 | ✅ 已解决（A2 + Windows 验证） | Linux 预设待实机验证 |
| MSVC 代码页 936 中文源码 | ✅ 已解决（`7a6de50`） | engine/app 加 `/utf-8` |
| 只构建 app 时缺 SPIR-V | ✅ 已解决（`11d4dfa`） | `my_app` 依赖 `vk_shaders` |
| swapchain semaphore 复用 / probe 布局 | ✅ 已解决（`e19a227`） | per-image render-done + probe 移到 present 前 |
| Y 翻转 | ✅ 已解决（B3a，2026-11-23） | 负高度 viewport + maintenance1 扩展 + frontFace=CW；dump 行翻转已移除 |
| 深度范围 z∈[-1,1] / 大范围精度 | ✅ 已解决（B3b，2026-11-23） | Reversed-Z：setUniform 后乘 z 修正 + GREATER 族 + 清屏 0；sRGB/mipmap 归 B3c |
| VK 模式退出段错误 | ✅ 已解决（2026-11-23） | `FontManager::face` 未初始化；VK 不加载字体时 FT_Done_Face 崩溃，NSDMI + 空值保护 |
| 中文安装路径加载失败 | ⏳ 待修 A4 | Assimp/纹理/着色器路径需 UTF-8/宽字符 |
| resize 重建 PSO / ImGui 失效 | ⏳ 待修 B1 | Dynamic Rendering 一并解决 |
| 描述符“当前块”隐式残留 | ⏳ 待修 B1/E1 | 每 draw 显式 block 或每 draw 独立描述符集 |
| 阴影 / 地面 / 文字（demo） | 部分 | 文字 D4；阴影/地面非产品需求，不投入 |
| pipeline layout 生命周期 | ✅ A3 已修 | 后续 B1 重建管线时保持 |

## 4. 并行开发与多机协作约定

### 4.1 接口先行
- **B0 是唯一串行门禁**。先冻结 RHI v2 头文件并打 tag，所有工作流从该 tag 拉分支。
- 接口变更必须改 `docs/RHI_DESIGN.md` 并通知所有工作流；禁止在实现分支里私自改公共头。

### 4.2 文件所有权（避免两台机器改同一文件）
- 同一时间**每个文件只有一个负责人**。重点冲突文件：`VkDevice.cpp`、`VkCommandList.cpp`、`RenderSystem.cpp`、`MeshPass.cpp`、`app/main.cpp`、`CMakeLists.txt`。
- 新功能尽量落新文件（如 `VkRenderTarget.cpp`、`UploadRing.cpp`、`VkCaps.cpp`），减少对既有大文件的并发修改。
- 管线/着色器目录按工作流分配：D1 用 `shaders_vk/*line*`，D4 用 `*font*`，D6 用 `*msaa*`。

### 4.3 分支与合并
- `master` 始终可构建、可运行 GL 与 VK 冒烟。
- 工作流分支：`feat/zw3d-<phase>-<topic>`，例如 `feat/zw3d-p1-dynrender`。
- 每周至少合并一次；合并前 rebase 到最新 master；冲突由后合并方解决。
- 接口冻结后不再接受“顺手重构公共头”的提交。

### 4.4 多机构建与验证
- 提交 `CMakePresets.json`（`win-msvc`、`linux-gcc`、`mac-clang`），统一构建入口。
- 依赖全部 vendored；不提交 `res/shaders_vk/*.spv` 与 `build/`。
- glslangValidator 版本写入 README/CI；找不到时 configure 直接报错，不静默降级。
- 新增 `tools/rhi_smoke`：离屏渲染固定场景 → 读回 → 哈希/截图，两台机器都能跑。
- 跨 GPU 不比逐字节：用容差/结构断言；需要基准图时按 GPU 型号分别保存。
- 验证层只在 Debug/开发机开启；提供 `RHI_VK_VALIDATION=1` 开关。

### 4.5 同步节奏
- 本文件作为计划唯一来源；每周更新“当前阶段/阻塞项/下一步”。
- 每个工作流结束在 issue/文档里写：改了什么、怎么验证、回退方式。

### 4.6 本机已验证的 Windows 构建/运行环境（2026-11-23）

- Windows 10 19045；VS 2022 17.14；MSVC 19.44.35207；Windows SDK 10.0.26100
- CMake **4.4.4**（VS 自带 3.31 在 C+CXX 检测阶段崩溃；4.4 正常。
  旧第三方 `cmake_minimum_required(<3.5)` 需 `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`）
- Vulkan SDK **1.4.363.1**：`C:\work\VulkanSDK`（headers / `vulkan-1.lib` / glslang / validation layer）
- GPU：GTX 1050 Ti，Vulkan 1.3.280，驱动 560.94

构建：

```bat
set VULKAN_SDK=C:\work\VulkanSDK
cmake -S . -B build/win-verify -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DENGINE_WITH_VULKAN=ON
cmake --build build/win-verify --config Debug --target my_app --parallel 8
```

运行：仓库路径含中文时 Assimp 无法打开模型（A4 待修）。临时办法是把
`my_app.exe` 与 `res/` 复制到 ASCII 目录（如 `C:\work\engine-bin`）后运行：

```bat
set RHI_BACKEND=vulkan
set RHI_VK_VALIDATION=1
set RHI_PROBE=1
set RHI_DUMP=1
my_app.exe
```

实测：验证层 0 error；`RHI_PROBE` 中心像素开烟雾时 `119,136,119,255`；
ImGui 面板可见；3D 内容 Y 翻转待 B3a。

**2026-11-23 B3a/B3b 复验**（同机同环境）：负高度 viewport 需要显式启用
`VK_KHR_maintenance1`（实例按 1.0 请求时验证层按 effective 版本判定，
1.1 核心化不豁免）；Reversed-Z 后画面正立、深度正确（鸭子遮挡天空盒）、
验证层 0 error、`RHI_EXIT_AFTER=90` exit=0（FontManager 修复后）。
`RHI_DUMP` 导出即屏幕行序，无需再手工翻转。

## 5. 里程碑验收

| 里程碑 | 验收标准 |
|---|---|
| M0 | Windows/macOS 已构建；Linux 待验证；GL-only 可构建；验证层无 error；能力表可查 |
| M1 | Y 翻转修复；Vulkan 深度 z∈[0,1] + Reversed-Z；sRGB + mipmap；GL/VK 视觉对齐；resize 不重建 PSO |
| M2 | 嵌入宿主窗口/离屏纹理；无 GLFW 依赖；headless 冒烟通过；多视口正确；中文路径资源加载正常 |
| M3 | 边线/宽线、拾取、透明、SDF 文字、剖切可用 |
| M4 | VMA + 异步上传 + fif=2；大装配在预算内稳定运行 |
| M5 | （可选）GPU 剔除/MDI；bindless 预研结论 |

## 6. 风险

1. **宿主集成信息不足**：窗口类型、线程模型、设备共享方式未确认，可能推翻 C1 的部分设计。
2. **GL 回退成本被低估**：所有新 RHI 能力都要有 GL 实现；Vulkan-only 接口会侵蚀可移植性。
3. **深度/坐标改造波及所有着色器**：B3 要与 D 阶段着色器工作排期错开。
4. **两台机器 GPU 不同**：跨机器像素对比不可靠，必须依赖结构断言与验证层。
5. **大装配规模未知**：E 阶段的上限（零件数/内存）需要在 ZW3D 真实数据上取样。
6. **许可证**：新增第三方（VMA/glslang 工具链）需确认与产品交付兼容。
