# RHI 渲染抽象层设计（v1）

> 目标：为引擎引入 **OpenGL / Vulkan 双后端** 打地基。接口按 **Vulkan 语义**抽象，
> GL 后端做"翻译器"，Vulkan 后端做"直译器"，未来 Metal 后端同样直译。

## 1. 设计原则

1. **向 Vulkan 的抽象层级看齐**，不向 GL 的便捷习惯看齐（这是双后端设计的成败点）
   - 状态在创建期烘焙进 Pipeline；运行期只有 bind + draw
   - uniform 用 bgfx 风格"每次绘制更新的小数据块"（`setUniform`），
     由各后端翻译成自己的绑定方式（GL: glUniform；Vulkan: push constant / UBO descriptor）
2. **强类型句柄**（`BufferHandle` / `PipelineHandle`...），防止资源句柄互相误用
3. **逃生舱**：`IRHIDevice::nativeHandle()` 可取后端原生句柄（GL 的 GLuint 等），
   供过渡期与旧 GL 代码互操作（如把现有阴影 FBO 直通给 `RenderPassDesc`）
4. **渐进迁移**：业务类（BaseDraw 体系 / camera / Light / Scene）本阶段不动

## 2. 模块布局

```
src/rhi/               ← 引擎库 (engine STATIC)
  RhiTypes.h          公共类型：句柄、枚举、描述符（BufferDesc/PipelineDesc/RenderPassDesc/UniformValue）
  IRHIDevice.h        设备接口：资源创建 + 帧循环（beginFrame/endFrame 对应 acquire/present）
  IRHICommandList.h   命令录制接口：beginRenderPass → bindPipeline → setUniform → draw
  RHIFactory.h/.cpp   后端工厂（Vulkan 分支 M3 接入）
  gl/                 OpenGL 后端（当前可用）
    GLDevice          资源表 + 复用现有 Shader 类做 GLSL 编译
    GLCommandList     "模拟器"：显式调用翻译回 GL 状态机（VAO 按 pipeline+buffer 缓存）
  vk/                 Vulkan 后端（M3 实施，尚未开始）
src/render/ ...        引擎库其余部分（RenderSystem/Pass/对象/相机），禁止 include ImGui
src/app/               ← app 层（编译进 my_app 可执行文件，不在引擎库里）
  DebugPanel.*        ImGui 调试面板（唯一允许 include ImGui 的地方）
main.cpp               app 入口：GLFW 窗口、ImGui 初始化、面板调用
```

**分层规则**：引擎库 `engine` 静态库不含任何 UI 代码；ImGui 只在 app 层
（`src/app/` + main.cpp）使用，通过 `RenderSystem::settings()` / `rhiDevice()`
公共接口读写引擎状态。未来 Vulkan 后端加在 `src/rhi/vk/`，同样属于引擎库。

## 3. 与现有代码的对应关系

| 现有概念 | RHI 对应 | 说明 |
|---------|---------|------|
| `BaseDraw` 的 VAO/VBO/EBO | `device->createBuffer()` | 顶点/索引数据上传 |
| `ShaderCache` + `ShaderType` | `GraphicsPipelineDesc` | 描述符携带 vert_/frag_ 文件名 + 全部渲染状态 |
| `CShadowPass/CNormalPass/CSkyPass/CPickPass` | `beginRenderPass(RenderPassDesc)` | GL 阶段 FBO id 可直通（阴影 Pass 现有 FBO） |
| `Shader::setXXX(name)` | `cmd->setUniform(name, UniformValue)` | 名字沿用现有 GLSL uniform |
| `DrawOption::MULTI_VIEWPORT` + `model_multiview.geom` | （Vulkan 阶段）`vkCmdSetViewport` + 每视口一次绘制 | MoltenVK 不支持几何着色器，必须替换 |
| `RenderSystem::draw()` 的 Pass 循环 | 不变，Pass 内部逐步改走 CommandList | 本阶段已支持调试面板按 Pass 跳过 |

## 4. 冒烟验证（已接入）

`RenderSystem` 里的 `drawSmokeTriangle()`：窗口 + RHI 设备 → Pipeline（vert_rhi_smoke/frag_rhi_smoke）
→ Buffer → CommandList 录制 → draw(3) 的完整链路。

运行程序后，在 **渲染调试** 面板勾选 **"RHI 冒烟三角形"** 即可看到场景中央的彩色三角形。

## 5. 调试面板

ImGui 窗口"渲染调试"（`src/render/DebugPanel.cpp`）：

- 后端信息（后端名 / GPU / API 版本）
- RHI 冒烟三角形 开关
- 帧级 Pass 开关：阴影 / 天空盒 / 拾取（关拾取会失效鼠标拾取，有弹窗提示）
- 全局：垂直同步 / ImGui Demo
- 业务层开关（相机/光照）下一阶段接入，面板中有占位

## 6. 里程碑路线

| 里程碑 | 内容 | 状态 |
|--------|------|------|
| M1 | RHI 接口 + GL 后端 + 冒烟三角形 + 调试面板 | ✅ |
| M2 | Line 迁移 RHI + `updateBuffer` + VAO 缓存 + Pass 边界状态保存/恢复 + **逐像素等价性验证** | ✅ |
| M2.5 | CMake 三层重构（engine 静态库 / app 独立）+ ImGui 出引擎 + 日志宏收口 + 通用化清理 | ✅ |
| M3 | Vulkan 后端（MoltenVK）：swapchain/pipeline/push constant 命令框架 + **像素回读验证通过** | ✅ |
| M4.1 | Vulkan 纹理（staging 上传）+ 描述符集（combined image sampler）+ 深度附件 + bindTexture 接口，**贴图四边形像素回读精确验证** | ✅ |
| M4.2a | 绘制级 uniform UBO（DrawUniforms 160B 固定布局）+ 实例化顶点属性（binding1/INSTANCE）+ **模型 RHI 迁移（DrawRHI 双路径）** + Vulkan 模式场景绘制（drawRhiScene）| ✅ 代码完成，两处视觉问题待修（见下） |
| M4.2b | **Vulkan 渲染鸭子场景成功**（帧导出确认：3 只黄鸭、纹理、深度、实例全部正确）+ UniformBlock 每对象独立 + 修复 UBO 描述符缺失/构造顺序/纹理数据源 | ✅ |
| M4.2c | 遗留排查完成：Vulkan 帧顶部大块 = 光源标记鸭（近距离正常内容，非 bug）；GL 原生鸭子不可见 = 原引擎 matModel 上传条件错误（已修） | ✅ |
| M4.3a | 天空盒 RHI 迁移（cubemap 纹理 GL/VK 双端 + 深度 LEQUAL + push 128B 双 mat4）+ VK 模式天空渲染成功 | ✅ |
| M4.3b | ImGui Vulkan backend 接入：VkCommandList 原生命令缓冲暴露、overlay 回调机制、VK 模式面板 | ✅ 代码完成 |
| M4.3c | VK 模式 ImGui 面板可见性（需 Metal capture 深查）；阴影/拾取 Pass 迁移 | 进行中 |
| **A1** | **Validation Layer + Debug Utils + 对象命名 + RHI_VK_VALIDATION 开关。修复真实缺陷：draw 期间原位更新描述符缺 UPDATE_AFTER_BIND（119 条违规 → 0）** | ✅ |
| **A3** | **VkCaps 能力探测（启动日志能力表）+ Pipeline Cache 持久化 + 调试面板能力表**；顺带修复：pipeline layout 每管线泄漏、退出时对象在命令缓冲中即销毁、单例析构晚于 `glfwTerminate` | ✅ |
| **A2** | **CMakePresets（mac/linux/win + GL-only）+ VULKAN_SDK 探测 + glslang 硬校验 + `ENGINE_HAS_VULKAN` 对 app 可见 + 临时目录跨平台** | ✅ macOS 验证；Win/Linux 待实机 |
| **B0** | **RHI v2 契约冻结**：DeviceDesc/NativeWindow、RenderTarget、SamplerDesc、TextureDesc(mip/sRGB/sampler)、ReadbackDesc、对象级销毁、结构化 DeviceCapabilities、setScissor。tag `rhi-v2-contract` | ✅ |
| **B3c** | **sRGB 线性工作流 + mipmap**：VK sRGB 交换链与颜色纹理；GL sRGB 默认帧缓冲与 `GL_SRGB8_ALPHA8`；ImGui 双端色彩空间处理；GL RHI 纹理 Format/采样器/mipmap 对齐；修复 mip 链未生成与缺屏障 | ✅ |
| M5 | 打包：MoltenVK Frameworks 内嵌（消除 DYLD_LIBRARY_PATH）、启动参数选后端 | 待做 |

## M3 调试历程（关键经验，都是真金白银换的）

Vulkan 后端从"创建即崩"到"像素回读验证通过"，共定位修复 **5 个问题**：

1. **`glfwCreateWindowSurface` 返回 -3（扩展缺失）**
   必须用 `glfwGetRequiredInstanceExtensions()` 取 GLFW 需要的平台扩展（macOS 是
   `VK_KHR_surface` + `VK_EXT_metal_surface`），不能只写 `VK_KHR_surface`。
2. **GLFW 找不到 loader（返回 0 个扩展）**：GLFW 内部 `dlopen("libvulkan.dylib")`，
   brew 装在 `/opt/homebrew/lib` 不在默认 dyld 路径 → 开发期需
   `DYLD_LIBRARY_PATH=/opt/homebrew/lib`；打包用 Frameworks 内嵌（M5）。
3. **类名遮蔽**：后端类叫 `VkDevice` 会遮蔽 Vulkan 原生句柄类型 `VkDevice`，
   字段声明变成不完整类型 → 类改名 `VulkanDevice`。
4. **GL 资源在构造函数/初始化路径被触碰**（Vulkan 模式无 GL 上下文，
   glad 函数指针为空 → 跳空地址段错误）：
   `ShaderCache::init`、4 个 GL Pass 的构造函数（FBO 创建）、`TextureMng` 构造、
   `BaseDraw` 构造、`FontManager` 字形纹理——Vulkan 模式全部跳过或改惰性创建。
   **教训：引擎任何构造函数禁止 GL/GPU 调用。**
5. **`VkCommandList::bindPipeline` 漏调 `vkCmdBindPipeline`**（致命 Bug）：
   只更新了包装器内部状态，命令缓冲里没有绑定管线 → 编码时
   `getGraphicsPipeline()` 返回 null → MVKCmdDrawIndexed::encode 空指针崩溃。
   定位手段：独立最小复现程序（相同调用序列逐一复刻）+ 逐调用二分 +
   ASAN + 读 lr 寄存器。**教训：包装器必须逐条翻译 API 语义，漏一条就是静默/延迟爆炸。**

### M3 验证方法

`RHI_PROBE=1`：第 30 帧 `vkCmdCopyImageToBuffer` 回读 swapchain 中心像素。
冒烟三角形质心预期颜色 `119,136,119,255`，实测一致 ✅。GL/Vulkan 同源验证。
B3c 改为线性工作流后，同一探针输出为 BGRA 字节 `215,140,122`（RGB `122,140,215`，
线性插值后 sRGB 编码的预期值）；`RHI_TEXQUAD` 中心像素仍精确等于纹理色。

### M4.2b 调试战果（重要修复 + 经验）

修复的问题：
1. **描述符集 binding0（UBO）从未写入**——着色器读未定义数据。
2. **共享 UBO 逐帧覆盖**——多个绘制共用一个 UBO 时后写的覆盖先写的
   → 改为 UniformBlock 每绘制对象独立（RHI 接口 `createUniformBlock`，
   GL 哑实现按名直传，VK 独立 UBO+描述符集）。
3. **构造顺序**：默认 uniform block 引用白色兜底纹理，而白纹理创建在后
   → 调整为白纹理 → 描述符对象 → 默认块。
4. **模型纹理数据源**：材质 `tex.data` 对外部纹理（duckCM.tga）为空
   → TextureMng 新增 `getTextureData()` 取预载数据（preloadTextures 递归
   加载过 res/ 全目录，含 res/model/）。
5. **GLSL 450 接口块需要显式 location**（mat3 占 3 个连续 location）。

验证方法沉淀：
- `RHI_DUMP=1`：Vulkan 帧整帧导出 PPM（BGRA→RGB 需注意字节序，
  导出未换通道时黄色显示为青色——**看帧先怀疑导出字节序**）。
- `TextureMng::getTextureData` + sips 转 PNG 目视纹理本体。
- 实例矩阵逐元素打印（对齐验证一锤定音：列主序、平移在 12..14）。

### M4.2c/M4.3 排查战果

1. **Vulkan 帧顶部大块 = 光源标记鸭**（objLight 位于 (0,10,0) 距相机近 →
   黄身黑眼白高光占满画面上方）——正常场景内容，非 bug。
2. **GL 原生鸭子不可见根因**：`PassTexture::setup` 实例矩阵数据仅在
   MULTI_VIEWPORT 时上传，普通模式实例属性读越界垃圾 → 已改为始终上传。
   （与 M2 坐标轴 VAO0 问题同族的原引擎既有缺陷，均已修复。）
3. **纹理纵向翻转**：stb top-first 行序 vs UV v=0 底部约定 → RHI 上传时
   翻转行序（不翻则鸭子采样到纹理顶部黑色眼睛区域）。

### A1 验证层使用说明

- 安装：`brew install vulkan-validationlayers`，运行时加
  `VK_LAYER_PATH=/opt/homebrew/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d`
- 开关：`RHI_VK_VALIDATION=1` 强制开 / `=0` 强制关；Debug 构建默认开
- 依赖扩展已自动处理：VK_EXT_debug_utils、VK_KHR_get_physical_device_properties2、
  VK_EXT_descriptor_indexing、VK_KHR_maintenance3、VK_KHR_portability_subset
- **已修复的真实缺陷**：bindTexture 在 draw 期间原位更新已绑定的描述符集
  （缺 UPDATE_AFTER_BIND）→ 命令缓冲失效 → 全部命令静默失败。
  修复：设备启用 Vulkan12 descriptorBindingSampledImageUpdateAfterBind +
  set layout/pool 加 UPDATE_AFTER_BIND 位 + descriptorIndexing 特性
- 场景当前验证层输出：0 error（A1 验收通过）

### A3 能力探测与 Pipeline Cache

- **能力表**：`VkCaps::query()`（`src/rhi/vk/VkCaps.*`）在选取物理设备后探测一次，
  启动时经 `ENGINE_LOG_INFO` 打印（设备/队列族/显存堆/上限/关键特性/路线图相关扩展）。
  同一份列表经 `IRHIDevice::capabilities()` 暴露给 app 层，调试面板
  「设备能力」折叠区展示（GL 后端默认空，B2 对齐）。
- **Pipeline Cache**：创建逻辑设备后加载、退出时导出到
  `res/shaders_vk/pipeline_cache.bin`（`.gitignore` 已忽略）。
  加载前校验 `VkPipelineCacheHeaderVersionOne`（vendorID/deviceID/UUID），
  不匹配则忽略（跨机器/驱动安全）；写盘走 `.tmp` + rename，崩溃不留半截文件。
  - `RHI_VK_PIPELINE_CACHE=<path>` 自定义位置；`=0` 完全禁用（性能对比用）。
- **顺带修复的真实缺陷**：
  1. pipeline layout 原来每创建一条管线就新建一个且仅保留最后一个句柄 →
     旧 layout 全部泄漏。改为设备级单一共享 layout，析构时销毁。
  2. `~VulkanDevice()` 未销毁 `_pipelines`（验证层报 leaked objects）→ 补上。
  3. ImGui 的 buffer/pipeline/描述符集被最后一帧命令缓冲引用，而
     `ImGui_ImplVulkan_Shutdown()` 在设备 idle 之前执行 → 销毁时验证层报
     "currently in use"。现在先 `waitIdle()` 再 Shutdown。
  4. `RenderSystem` 单例在 `glfwTerminate()` **之后**才析构（静态析构顺序），
     Vulkan 设备析构时驱动/窗口已拆除 → `std::system_error: mutex lock failed`。
     新增 `RenderSystem::shutdown()`，main 在 `glfwTerminate()` 之前显式调用。
     **教训：图形设备必须在拥有它的窗口之前销毁，不能依赖单例静态析构。**

验收方法（macOS，MoltenVK）：

```bash
DYLD_LIBRARY_PATH=/opt/homebrew/lib RHI_BACKEND=vulkan RHI_SMOKE=1 \
  RHI_EXIT_AFTER=120 ./build/app/Debug/my_app
# 第一次：capabilities 表 + "pipeline cache empty" ... "pipeline cache saved: N bytes"
# 第二次： "pipeline cache loaded: N bytes"
```

### B0：RHI v2 契约（已冻结，tag `rhi-v2-contract`）

冻结原则：**只加不改**。v1 的调用路径原样可用，新增能力默认"不支持"或在
B1–B4 逐后端落地；冻结后公共头的任何改动必须同步本节并通知所有并行工作流。

| 契约项 | 定义 | 实现状态 |
|---|---|---|
| `DeviceDesc` / `NativeWindow` | 宿主窗口（Win32/Xlib/Xcb/Wayland/Cocoa/MetalLayer）或离屏设备；引擎不再硬依赖 GLFW | C1 实现；`RHIFactory::createDevice(DeviceDesc)` 当前明确告警返回空 |
| `RenderTargetDesc` / `RenderTargetHandle` | 离屏颜色/深度附件，`RenderPassDesc::renderTarget`；invalid = 默认帧缓冲 | ✅ B4（GL/VK 双端；颜色附件可采样可回读；VK 格式须与管线烘焙格式兼容） |
| `SamplerDesc` | filter/wrap/LOD/aniso/compare；在 `TextureDesc::sampler` 内提供 | ✅ GL B2（独立采样器对象 `glGenSamplers/glSamplerParameter*` + `attachSampler` 显式关联，`bindTexture` 优先 `glBindSampler`；纹理烘焙参数路径原样保留）、VK B3（createTexture 内建） |
| `TextureDesc` 扩展 | `mipLevels` / `generateMips` / `arrayLayers`；sRGB 通过 `Format` 表达 | B3 生效 |
| `ReadbackDesc` + `readbackTexture` | 同步回读（缩略图/拾取/验证） | ✅ B4（GL/VK；VK submitOneTime 同步；已知行序差异：VK row0=画面顶、GL row0=画面底，统一随 B2） |
| 对象级销毁 | `destroyBuffer/Texture/Pipeline/UniformBlock`；句柄 id 不复用，槽位保留 | ✅ GL/VK 已实现 |
| `DeviceCapabilities` | 结构化能力（limits/扩展特性）+ `details` 人类可读表 | ✅ GL/VK 均已填充（B2：GL 用 `glGetIntegerv/glGetString/glGetStringi` 惰性查询，中文 details 表与 VK 同风格；启动时不打印，面板「设备能力」按需展示） |
| `setScissor` | 多视口裁剪（C2 前置） | C2 实现 |

注意：
- `RenderPassDesc::framebufferId`（GL FBO 直通）是 v1 遗留，保留到 B2 完成后移除。
- VK `destroyPipeline` 后槽位标记 dead，swapchain 重建不会复活；`destroyUniformBlock`
  只回收 UBO 内存，描述符集随 pool 回收（pool 未开 FREE_DESCRIPTOR_SET_BIT，E1 细化）。
- 设备默认 uniform block 由设备持有，`destroyUniformBlock` 对其告警并忽略。

### B2：GL 后端收口——独立采样器 + 能力表对齐（2026-11-23，macOS 验证）

**独立采样器对象**（`GLDevice::createSampler/destroySampler` + `GLCommandList::bindTexture`）：

- 参数映射与 VK 端一致：`minFilter×mipFilter` 组合成 GL 四档 mipmap MIN_FILTER
  （对应 VK `minFilter+mipmapMode`）；wrap S/T/R；`min/maxLod`；anisotropy 按
  `GL_EXT_texture_filter_anisotropic` 硬件上限 clamp；compareEnable 开
  `GL_TEXTURE_COMPARE_MODE=GL_COMPARE_REF_TO_TEXTURE`、比较函数取 `GL_GREATER`
  （Reversed-Z 约定，见 VkDevice.cpp compareOp 注释）。
- **双路径并存，既有行为零变化**：`createTexture` 依旧把 `SamplerDesc` 烘在纹理对象上
  （glTexParameteri）；独立采样器只有显式 `createSampler + GLDevice::attachSampler`
  （GL 后端内部扩展，不在冻结契约内）关联到 `GLTexture::sampler` 才生效。`bindTexture`
  对关联纹理优先 `glBindSampler`，未关联纹理绑 0（恢复烘焙参数）；设备从未创建过
  采样器时完全不触碰 `GL_SAMPLER_BINDING`。
- **Pass 边界状态中性扩展**：Pass 内绑定过的采样器槽位在 `endRenderPass` 统一解绑
  （`GL_SAMPLER_BINDING` 也是全局状态，不能泄漏给遗留 GL 代码）。
- 已知约束：GL 的 mipmap 档 MIN_FILTER 要求纹理有完整 mip 链（不完整纹理采样为黑），
  `SamplerDesc` 无"不用 mip"表达（枚举冻结），独立采样器请只用于带 mip 链的纹理；
  立方体贴图要求 Clamp 类地址模式，由调用方保证（VK 端是在 createTexture 内强制）。
- `destroySampler` 槽位保留失效（id 不复用），并清理纹理上的悬空引用。

**DeviceCapabilities**：GL 侧 `capabilities()` 惰性查询（启动不打印），
`glGetIntegerv/glGetString/glGetStringi` 产出与 VK 同风格的中文 details 表。
GL 特有语义：`maxUniformBufferRange=GL_MAX_UNIFORM_BLOCK_SIZE`、
`maxSampledImagesPerSet=GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS`、push constant/显存
查询不到（0 + details 注明）；`samplerAnisotropy` 按扩展探测；
`sRGBSwapchain` 按 GL 3.0 核心的 `GL_FRAMEBUFFER_SRGB` 能力报告
（macOS 查不到 `GLFW_SRGB_CAPABLE`，`GL_BACK` 附件编码查询非法会报
GL_INVALID_ENUM——用 `GL_BACK_LEFT`，app/main.cpp 的启动诊断踩过前者）。

验证（macOS / Apple M4 Pro，GL 4.1 Metal）：

```bash
cmake --preset mac-clang && cmake --build --preset mac-clang   # 零错误
RHI_EXIT_AFTER=60 ./build/app/Debug/my_app                     # exit=0，无 GL 错误
RHI_EXIT_AFTER=60 RHI_RT_TEST=1 ./build/app/Debug/my_app       # corner=124,89,63,255
                                                               # center=214,142,121,255（B4 基线不变）
RHI_TEXQUAD=1 RHI_EXIT_AFTER=60 ./build/app/Debug/my_app       # 贴图四边形正常（bindTexture 改动回归）
```

另以临时自检代码（未入库）驱动过独立采样器全链路：createSampler → attachSampler →
Pass 内 bindTexture（glBindSampler 分支）→ endRenderPass 解绑 → destroySampler 引用清理，
`glGetError` 全程 0x0；能力表查询后 `glGetError` 亦为 0x0。

### B3a/B3b：VK 后端坐标与深度约定（2026-11-23，Windows 实机验证）

Vulkan 后端在内部统一消化 GL↔VK 的坐标系差异，GL 路径与上层代码零改动：

1. **Y 翻转（B3a）**：所有 VK pass 用**负高度 viewport**（origin.y=高、height=-高）。
   实例仍按 API 1.0 请求（规避 loader/MVK 版本协商），验证层按 min(实例,设备)
   版本判定，maintenance1 虽已核心化仍须**显式启用 `VK_KHR_maintenance1`
   设备扩展**（否则 VUID-VkViewport-apiVersion-07917 报错）。
   配套地所有管线 `frontFace = CLOCKWISE`：负高度镜像使屏幕绕序翻转，
   GL 网格的 CCW 正面才能正确剔除背面。
2. **Reversed-Z（B3b）**：`VkCommandList::setUniform` 对 MVP 类 mat4
   （uMatrixMVP / uViewProj / uProjMat；不含 uViewMat）后乘修正
   `z' = 0.5*(w - z)`：GL 的 z∈[-1,1]（近=-1/远=+1）映射为 VK 的 z∈[0,1]
   且**深度反转**（近=1/远=0），吃满浮点深度的近端精度（CAD 大深跨必需）。
   深度清屏值固定 0（最远）；管线比较算子 LESS→GREATER、
   LEQUAL（天空盒）→GREATER_OR_EQUAL；天空盒 vert_sky 的远平面技巧相应从
   `pos.xyww`（GL 远=1）改为 `z=0`（reversed 远=0）。

验证（GTX 1050 Ti / Vulkan 1.3.280）：验证层 0 error；画面正立、UI 不受影响；
鸭子正确遮挡天空盒——若比较算子泄漏成 LEQUAL 语义，后画的天空盒会糊掉鸭子，
可作回归断言。`RHI_DUMP` 的行翻转 hack 已随 B3a 移除（dump 即屏幕行序）。

顺带修复退出段错误（基线即崩，非 B3 引入）：`FontManager::face`（FT_Face）
未初始化，VK 模式不加载引擎字体时 `~FontManager` 对垃圾指针调 `FT_Done_Face`
崩溃；GL 模式永远加载字体所以从未暴露。已 NSDMI 初始化 nullptr + 空值保护。

已知遗留（非回归，随 D 系列处理）：光源指示鸭 `objLight`（scale 0.5，duck.dae
原始尺寸约百单位 → 约 50 单位高，位于 (0,10,0)）会包住轨道相机，画面顶部出现
巨大鸭眼弧形。GL 侧 selfTestAB 的 frame_rhi.ppm 同样复现 → 属 DrawRHI/场景内容
既有问题，与后端无关。

### B3c：sRGB 线性工作流 + mipmap（2026-11-23，macOS MoltenVK 验证）

颜色空间约定改为**线性工作流**，两端语义一致：

| 环节 | GL | Vulkan |
|---|---|---|
| 交换链/默认帧缓冲 | `GL_FRAMEBUFFER_SRGB`（macOS 实测 `glClearColor(0.5)` 回读 188，编码生效） | 优先 `B8G8R8A8_SRGB`/`R8G8B8A8_SRGB` surface format；无则告警回退 UNORM |
| 颜色纹理 | `GL_SRGB8_ALPHA8`（diffuse/specular/天空盒/地面 diffuse） | `Format::RGBA8_SRGB` |
| 数据纹理 | `RGBA8/R8` 原值（法线/高度/遮罩/字体图集） | `Format::RGBA8_UNorm` |
| 作者 sRGB 常量 | 清屏色、冒烟顶点色在 CPU 侧 `srgbToLinear`（`src/core/color/ColorSpace.h`） | 同一份 `RenderPassDesc`/顶点数据，行为一致 |
| ImGui | 绘制前 `glDisable(GL_FRAMEBUFFER_SRGB)`，保持既有 gamma UI | 初始化后把 `ImGuiStyle::Colors` RGB 预转线性，写回编码后恢复原色 |

- GL RHI 纹理创建补齐 `Format` 映射、sRGB、mipmap（`generateMips`/`mipLevels==0`）
  与 `SamplerDesc` 的 filter/wrap/LOD；mip 链由 `glGenerateMipmap` 生成。
- 顺带修复两个 B3 遗留缺陷：
  1. `generateMips=true` 但未显式设 `mipLevels=0` 时只分配 1 级 —— 天空盒 mip
     链实际从未生成；现改为“`generateMips` 且级数 ≤1 → 完整链”。
  2. mip blit 的目标级没有 `UNDEFINED → TRANSFER_DST` 屏障（此前链路未真正启用，
     验证层未暴露）；现逐级补屏障。
- 验证：VK 验证层 0 error；`RHI_TEXQUAD` 中心像素仍精确 `180,60,220`（sRGB
  解码+编码往返无损）；`RHI_SMOKE` 探针字节 `215,140,122`（BGRA → RGB
  `122,140,215`）为线性插值再编码的预期结果；GL `selfTestAB` 的 RHI/GL 两张
  帧图在天空/水面采样逐像素一致（差异仅剩已知的光源指示鸭 RHI 路径问题）。

### M4.3b 已知问题（M4.3c 优先）

- **VK 模式 ImGui 面板不显示**：overlay 每帧调用（716 顶点）、DisplaySize=800×500
  fbScale=2、RenderDrawData 全链路无 VK 错误，但帧导出无面板。
  下一步：Metal capture（Xcode GPU 断点）或 vendored imgui_impl_vulkan.cpp 临时
  printf，定位顶点变换/裁剪环节。备选排查：viewport/scissor（DisplaySize 点 vs
  帧缓冲像素的 Retina 倍率）、imgui 内部 Frames[ImageCount] 复用。

### 远程并行改动移植清单（origin 17 提交，分支 backup/remote-17commits）

背景：远程并行演化的版本做了 `RenderSystem/` 目录重构与类改名
（Shader→ShaderMng、image→TextureMng/ResMng）。已按主线策略以本地
RHI 版本为树合并（`-s ours`），远程功能清单如下，待逐项移植：

| 远程提交 | 内容 | 规模 | 移植评估 |
|---------|------|------|---------|
| 7138466 | **SDF 字体渲染**（frag_distance 场 + FontManager 扩展） | 74 行 | 高价值——文字渲染质量，随 RHI 文字路径（M4.4）一起移植 |
| af067ec | **相机重构** + 新增 vector3/vector4 数学类 | 665 行 | 大头——需与现有 camera（原 camera_old）合并评估 |
| 0fdb9af | **Windows 编译修复**（CMake + camera 头） | 小 | 直接参考——M5 跨平台打包时应用 |
| 49e136a | 中文编码问题修复（GBK→UTF8） | — | 参考——本地仍有 GBK 注释，逐步转 UTF8 |
| 7bc9d3f | 天空盒混合问题修复（SkyPass/NormalPass 小改） | 小 | 待对比——评估 RHI 天空路径（LEQUAL 已处理深度）是否受同一问题影响 |
| 9ace7a2 | 编译警告修复 | 小 | 参考性 |
| 其余 | 目录结构/删除文件/绘制接口整理 | — | 已被本地三层结构取代，不移植 |

备份分支：`backup/remote-17commits`（17 提交完整可查）；
本地 RHI 工作备份：`backup/rhi-work-88cbca3`。

## 调试设施（保留，正式功能）

- `selfTestAB()`：同帧 A/B 逐像素对比（RenderSystem，GL 模式）
- `RHI_PROBE=1`：VK 第 30 帧中心像素回读
- `RHI_DUMP=1`：VK 整帧导出 PPM（注意 BGRA→RGB 已正确交换）
- `RHI_SMOKE=1` / `RHI_TEXQUAD=1`：无 UI 环境强制显示验证内容
- `RHI_EXIT_AFTER=<帧数>`：跑完 N 帧正常退出（走完整析构，冒烟/缓存落盘验证用）
- `RHI_VK_PIPELINE_CACHE=<path>|0`：Vulkan 管线缓存位置 / 禁用

### M4.1 后当前限制（M4.2 待办）

- Vulkan 模式跳过全部 GL Pass（旧 Pass 构造/绘制是 GL 代码）；已迁移内容：
  冒烟三角形、贴图四边形（`RHI_TEXQUAD=1`，像素回读精确等于纹理颜色）
- ~~纹理无 mipmap、暂无 sRGB~~（B3/B3c 已完成：sRGB 纹理/交换链 + GPU mip 链）；大纹理 staging 上传为一次性同步提交
- 描述符集为 frame-in-flight=1 的简化实现（bindTexture 直接更新，多飞行帧需换池化策略）
- uniform 仅 push constant（80B：MVP+颜色）；材质/骨骼/光照数据需 UBO+描述符（M4.2）
- `beginRenderPass` 不支持非默认帧缓冲（离屏渲染/阴影深度图 M4.2）
- ImGui Vulkan backend 已接入但面板在 VK 模式不可见（B5/M4.3b 排查中）；能力表可看启动日志
- Line 的 RHI 路径在 Vulkan 下 no-op（需活跃 render pass，M4.2 随 Pass 迁移解决）
- 窗口 resize 的 swapchain 重建代码就绪但未实测

## 2.1 M2 验证方法与结论（重要经验）

迁移验证采用**同帧 A/B 对比法**：同一帧内先用 RHI 路径渲染一遍取全帧哈希，
再用原 GL 路径渲染一遍取哈希——场景状态完全一致，哈希相等即逐像素等价。

**结论：`MATCH (pixel-identical)`，NormalPass 差异像素 = 0。**

过程中发现并修复的两个真实 bug：

1. **GL 后端状态泄漏（架构级，已修）**：管线设置 `glDepthMask(GL_FALSE)` 后无人恢复，
   泄漏给后续渲染 → 深度清屏失效 → 下一帧用残留深度做测试 → 场景大面积绘制失败。
   修复：`GLCommandList` 在 `beginRenderPass/endRenderPass` 保存/恢复
   depthTest/depthMask/blend/cull——RHI Pass 对周边遗留代码状态中性。
   **教训：GL 后端必须把 Vulkan 语义的"Pass 边界 = 状态边界"落实为显式保存/恢复。**

2. **原引擎坐标轴从未显示过（遗留 bug，已修）**：`Line::calculate()` 结尾
   `glBindVertexArray(0)` 解绑 VAO，随后 `glDrawElements` 在核心配置下配 VAO 0
   属非法调用，静默不绘制。修复：去掉解绑。
   **教训：GL 的这类错误默认完全静默，等价性对比测试是暴露它们的有效手段。**

另注：应用启动时存在一个与本次改动无关的 GL 错误码（1282, GL_INVALID_ENUM），
在两个分支均出现，来源待查（疑似 ImGui 后端或更早代码），不影响本次验证。

## 7. Vulkan 阶段注意事项（M3 环境已就绪 ✅）

**环境（已安装并验证）**：brew 安装 molten-vk / vulkan-loader / vulkan-headers / glslang，
探针程序确认 `Apple M4 Pro | API 1.2.357` 枚举成功。

- **portability 枚举（macOS 必做，否则 vkCreateInstance 失败）**：
  instance 需设 `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`
  并启用 `VK_KHR_portability_enumeration` 扩展（MoltenVK 是 portability 驱动）
- **ICD 自动发现**：loader 默认扫描 `/opt/homebrew/etc/vulkan/icd.d`，无需环境变量；
  发布时 .app 自带 `libMoltenVK.dylib` + `MoltenVK_icd.json`（约 5MB）
- **着色器**：`glslangValidator` 可用（GLSL→SPIR-V），M3 起构建期离线编译，
  RHI 管线描述符将增加 SPIR-V 路径字段
- **几何着色器**：MoltenVK 不支持，`model_multiview.geom` 的多视口改为
  每视口 `setViewport + draw`，全平台通用
- **窗口**：GLFW surface 创建后需检查 `GLFW_KHR_vulkan` 支持
- **同步**：接口里 `beginFrame/endFrame` 已按 acquire/present 语义预留

## 8. 已知 v1 简化（后续偿还）

- 对象级销毁接口未暴露（资源随 device 生命周期回收）
- `RenderPassDesc.framebufferId` 暂为 GL FBO 直通，M3 换成 TextureHandle 数组
- 索引类型固定 uint32；blend 只支持 srcAlpha 一种模式
- Line 的 RHI buffer 首次创建后容量固定，`setData` 超容量不扩容（重建接口 M3 补）
- Line `drawRHI` 在 NormalPass 内部进行（GL 下合法；Vulkan 后端将改为向当前 Pass 录制）

## 9. M2 已完成项

- `IRHIDevice::updateBuffer()`：运行期更新 buffer（GL: `glBufferSubData`，带越界保护）
- VAO 按 (pipeline, vertexBuffer) 缓存（`GLCommandList`，替代 v1 单 VAO 重配置）
- Line 双路径迁移：`Line::drawRHI()`（`vert_color/frag_color`，LINES，脏标记按需上传）
- Pass 边界 GL 状态保存/恢复（见 2.1 节经验教训 1）
- 调试面板新增"迁移验证"分组：`Line 走 RHI` 开关，可实时切换两条路径
