# CLAUDE.md

## 项目概述

C++17 跨平台渲染引擎，**双后端（OpenGL 3.3 / Vulkan）**，通过自研 RHI 抽象层统一封装。基于 GLFW + GLAD + Dear ImGui + Assimp + FreeType 构建。目标：通用渲染引擎，可移植到任意产品项目（macOS / Windows / Linux，Vulkan 在 macOS 经 MoltenVK）。

## 分层架构（重要约定）

```
app 层（可执行 my_app）   main.cpp + DebugPanel —— 唯一允许使用 ImGui 的地方
engine 层（静态库）        src/ 下的 RHI + 渲染系统，禁止依赖任何 UI 库
third-party 层            3rdparty/ 各自独立 target
依赖方向：app → engine → third-party，禁止反向
```

- **引擎库 `engine`** 不含 UI 代码；app 通过公共接口交互：
  `RenderSystem::settings()`（读写渲染设置）、`rhiDevice()`（设备句柄）、`preloadTextures()`、`GetFontMng()`
- **资源加载是 app 层职责**：引擎不假设项目资源布局（纹理预载、字体加载由 app 显式调用）
- **日志必须走宏**：引擎代码禁止直接 printf/cout，一律 `ENGINE_LOG_INFO/WARN/ERROR/WARN_ONCE`
  （见 `src/core/log/LogMacros.h`，换日志系统只需改该映射头或编译定义覆盖）

## 构建

### 前置要求
- CMake 3.22+、C++17
- macOS: Xcode 命令行工具
- Vulkan 后端（可选）：`brew install molten-vk vulkan-loader vulkan-headers glslang`
  （找不到则自动降级为仅 GL 后端；SPIR-V 由构建期 glslangValidator 编译）

### 构建命令

```bash
# 推荐：CMake Presets（mac-clang / linux-gcc / win-msvc，另有 *-gl-only）
cmake --preset mac-clang && cmake --build --preset mac-clang

# 等价的手动命令
cmake -S . -B build -G Xcode
cmake --build build
```

- 可执行文件：`build/app/Debug/my_app`（引擎库产物：`build/src/Debug/libengined.a`）
- **GL-only 构建**：`-DENGINE_WITH_VULKAN=OFF`（无 Vulkan SDK 时自动降级）；
  Vulkan 后端启用但缺 `glslangValidator` 时 configure 直接报错
- 多平台构建入口统一走 `CMakePresets.json`；`VULKAN_SDK` 环境变量会被自动探测

### 运行

```bash
# OpenGL 后端（默认）
./build/app/Debug/my_app

# Vulkan 后端（开发期需 DYLD 路径让 GLFW 找到 loader；打包时 Frameworks 内嵌解决）
DYLD_LIBRARY_PATH=/opt/homebrew/lib RHI_BACKEND=vulkan ./build/app/Debug/my_app

# 环境变量
#   RHI_BACKEND = opengl(默认) | vulkan
#   RHI_SMOKE   = 1 强制显示 RHI 冒烟三角形（无 UI 场景验证用）
#   RHI_PROBE   = 1 第 30 帧回读屏幕中心像素打印（渲染链路验证）
#   RHI_ASYNC_DEMO = 1 后台预解析 duck.dae（E5 流式加载验证；VK 生效，GL 同步退化）
#   RHI_EXIT_AFTER = <帧数> 跑完 N 帧正常退出（冒烟/回归，验证析构与缓存落盘）
#   RHI_VK_VALIDATION = 1 强制开验证层 / 0 强制关（Debug 构建默认开）
#   RHI_VK_PIPELINE_CACHE = <path> 管线缓存位置 / 0 禁用
#     （默认 res/shaders_vk/pipeline_cache.bin，已 gitignore）
```

## 目录结构

```
app/                    -- app 层（编译进 my_app）
  main.cpp              -- 窗口创建（按后端选 GL 上下文 / NO_API）、ImGui 初始化、场景组装
  DebugPanel.h/cpp      -- ImGui 渲染调试面板（后端信息 / Pass 开关 / RHI 开关）
src/                    -- 引擎层（engine 静态库）
  CMakeLists.txt        -- engine 库定义 + Vulkan 检测 + SPIR-V 构建规则
  rhi/                  -- RHI 抽象层（Vulkan 语义设计，详见 docs/4-RHI接口契约.md）
    RhiTypes.h            句柄/描述符/UniformValue（bgfx 风格每-draw uniform 块）
    IRHIDevice.h          设备：资源创建 + beginFrame/endFrame + onResize
    IRHICommandList.h     命令录制：beginRenderPass → bindPipeline → setUniform → draw
    RHIFactory.h/cpp      后端工厂
    gl/                   OpenGL 后端（GLDevice / GLCommandList，"翻译器"实现）
    vk/                   Vulkan 后端（VulkanDevice / VkCommandList，"直译"实现，M3 ✅）
  core/
    math/                 自定义 Matrix 类
    log/LogMacros.h       日志宏映射点（换项目时只改这里）
    taskQueue.h           任务队列（未大量使用）
  render/
    RenderSystem.*        顶层编排（Pass 循环、输入事件、RHI 设备持有）
    RenderSettings.h      渲染管线级开关（Pass 跳过 / RHI 开关 / vsync）
    scene.* / camera/     场景容器、轨道相机（camera.h）+ 控制器
    shader.* / image.*    GLSL 缓存（GL 后端用）、纹理管理（stb）
    FontManager.* / Light.h
    model/                Assimp 模型加载、骨骼动画；MeshPass（原 RenderPass，网格级策略）
    pass/                 帧级 Pass（GL 专属实现）：ShadowPass/NormalPass/SkyPass/PickPass
    object/               Line / Sky / Text / ImageRectangle 可绘制对象
res/
  shaders/                GLSL（GL 后端，无扩展名 vert_xxx/frag_xxx）
  shaders_vk/             GLSL（Vulkan 变体，.vert/.frag → 构建期编译为 .spv）
  textures/ font/ model/  资源
docs/1-架构设计.md        分几层、每层干什么、边界
docs/2-类设计与时序图.md  代码怎么组织、怎么协作
docs/3-ADR.md            关键决策和理由
docs/4-RHI接口契约.md     RHI 保证什么、不能依赖什么
docs/5-后端对照与差异.md  OpenGL 和 Vulkan 哪里不同
docs/6-测试验证与性能基准.md  怎么证明一致、怎么达标
docs/7-实现进度.md        做到哪了、下一步做什么
```

## 文档约定（固定清单）

项目文档是**固定清单**，各司其职。AI 写文档时：**禁止新建 .md 文档**，只能写进下面某一份；
先判断内容属于哪份，写进去，不要另起炉灶。

| 文档 | 唯一职责 |
|------|---------|
| `README.md`（根目录） | 项目是什么 + 文档清单 + 构建运行 |
| `docs/1-架构设计.md` | 分几层、每层干什么、边界 |
| `docs/2-类设计与时序图.md` | 代码怎么组织、怎么协作 |
| `docs/3-ADR.md` | 关键决策和理由 |
| `docs/4-RHI接口契约.md` | RHI 保证什么、不能依赖什么 |
| `docs/5-后端对照与差异.md` | OpenGL 和 Vulkan 哪里不同 |
| `docs/6-测试验证与性能基准.md` | 怎么证明一致、怎么达标 |
| `docs/7-实现进度.md` | 做到哪了、下一步做什么 |

写文档的规矩：

1. **大白话优先**：用表格、短句、短列表，不用表情符号和术语堆砌；能画表就画表，别写长段。
2. **进度唯一**：「做到哪了」只写进 `docs/7-实现进度.md`，其他文档不要各自维护一份进度。
3. **一事一处**：同一主题只在一份文档展开，跨文档用链接引用，不复制粘贴。
4. **新增内容归位**：设计决策写进对应文档（架构 → 1，决策 → 3，契约 → 4，差异 → 5，
   验证 → 6，状态 → 7），不新开文件。

## 渲染管线

**OpenGL 路径**：ShadowPass → NormalPass → SkyPass → PickPass（帧级 Pass，GL 代码）+ RHI 冒烟三角形
**Vulkan 路径**：GL Pass 整体跳过（M4 逐个迁移），仅渲染已迁移到 RHI 的内容（冒烟三角形）

### RHI 使用模式

```cpp
auto* device = RenderSystem::getInstance().rhiDevice();   // 可能为空（后端未启用）
auto* cmd = device->commandList();
device->beginFrame();
cmd->beginRenderPass(desc);        // Vulkan: swapchain framebuffer；GL: FBO 直通
cmd->bindPipeline(pipe);           // 管线 = 着色器文件名 + 顶点布局 + 状态（创建期烘焙）
cmd->setUniform("uMatrixMVP", v);  // GL: glUniform；VK: push constant（固定布局）
cmd->bindVertexBuffer(vb);  cmd->bindIndexBuffer(ib);
cmd->drawIndexed(n);
cmd->endRenderPass();
device->endFrame();
```

### 后端差异速查

| 事项 | GL | Vulkan |
|------|----|--------|
| 着色器 | res/shaders/（GLSL 直编） | res/shaders_vk/（GLSL→SPIR-V 构建期） |
| uniform | glUniform 按名 | push constant 固定布局（MVP@0 mat4 / color@64 vec4） |
| 颜色空间 | 默认帧缓冲 `GL_FRAMEBUFFER_SRGB` + 颜色纹理 `GL_SRGB8_ALPHA8` | sRGB 交换链 + `Format::RGBA8_SRGB` 颜色纹理 |
| Pass 边界状态 | 保存/恢复（GLCommandList） | 天然隔离 |
| 遗留 GL Pass | 全部可用 | 跳过（M4 迁移） |
| 字体/文字 | FreeType→GL 纹理 | 未迁移（M4） |

## 关键单例

`RenderSystem::getInstance()` / `ShaderCache::GetInstance()`（GL 专用）/ `TextureMng::getInstance()` / `Light::GlobalLight()` / `TaskQueue::instance()`

## 重要约定

- **构造函数禁止 GL/Vulkan 调用**：无图形上下文的后端模式下构造即崩
  （纹理惰性创建模式见 TextureMng::getTexture 对 STR_DEPTH_TEXTURE 的处理）
- **销毁顺序**：main 必须在 `glfwTerminate()` 之前调用 `RenderSystem::shutdown()`；
  单例静态析构发生在窗口销毁之后，此时析构 Vulkan 设备会崩溃（A3 教训）
- **GL Pass 边界状态保存/恢复**：Vulkan 语义的 Pass 不允许泄漏状态给周边
  （GLCommandList::beginRenderPass/endRenderPass 实现，历史教训见 docs）
- **Vulkan 命名**：后端类名 `VulkanDevice`（不得用 `VkDevice`，会遮蔽 Vulkan 原生句柄类型）
- **uniform 名称**：遗留拼写 `uMaterail` 保留，改时需同步 GLSL
- **几何着色器**：MoltenVK 不支持，`model_multiview.geom` 的替代方案见 docs
- **路径字符串一律 UTF-8**（A4）：引擎接口的路径 `std::string` 按 UTF-8 解释；
  打开文件必须走 `core/fs/FilePathUtil`（Windows 宽字符边界），
  Assimp 用 `Utf8IOSystem`；禁止 `path::string()` 往返（Windows 上是 ACP，
  中文路径会坏），需要字符串时用 `engine::fs::utf8FromPath`
- 部分历史代码注释为 GBK 编码，macOS 下显示乱码（编辑时用 ASCII 锚点匹配）

## 已知限制

- Vulkan 模式渲染 RHI 迁移内容（模型/天空盒/线/验证叠加）；阴影、拾取、文字等 GL Pass 尚未迁移
- ImGui Vulkan backend 已接入，VK 模式面板可见（B1 动态渲染模式）
- Vulkan 仅 1 个全局平行光 / 无 PBR；阴影/拾取/文字 Pass 待迁移
- 纹理资源 96MB 未用 Git LFS
- RHI 对象级销毁已实现（buffer/texture/pipeline/uniform block）；离屏 RenderTarget 与 readback 未实现（B4）
