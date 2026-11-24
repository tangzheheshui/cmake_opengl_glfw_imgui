# 引擎接口分层与接口设计规划（v1）

> 日期：2026-11-24
> 定位：把引擎从"自带窗口的示例程序"收口为"可嵌入宿主的渲染库"。
> 关系：本文细化 `docs/ROADMAP_ZW3D.md` 中的 C1 / C5，并新增 L0 / L1 / L2
> 三项前置工作；RHI 公共契约仍以 `docs/RHI_DESIGN.md` 的 B0 冻结版为准。
> 原则：**只加不改**；每一步 master 上 GL / Vulkan 双后端可运行；先接口后功能；
> 不改变现有渲染算法与像素结果。

> **⚠ 2026-11-24 状态对齐（master `1d70b19`，提交前必读）**
>
> 本文写于 2026-11-24 早间；同日另一台机器已把 **A4 / B4 / C2(v1) / D1(核心) /
> D2 / E1 / B6** 合入 master，并新增 `docs/RHI_V3_PLAN.md`（RHI v3 破坏性重构）。
> 因此：
>
> 1. 本文 §4.7 / §4.8 中标为“待做”的 B4 / D2 / E1 / A4 等**已过时**；
>    当前状态一律以 `docs/ROADMAP_ZW3D.md` 为准。
> 2. 本文仍然有效的部分：**L0（RHI 去 render）、C1（引擎去 GLFW）、L2（公共 API 卫生）**，
>    以及“两机分工”的方法（文件所有权、VkDevice 令牌制）。
> 3. **与 `RHI_V3_PLAN.md` 的冲突**：v3 主张先做 V3-0→V3-2（解 32-set 崩溃/绑定层），
>    并在窗口期独占 `GLDevice / VkDevice / RhiTypes.h / RenderSystem`；本文的 C1a/C1b
>    与 L0 的 `setDrawUniforms` 迁移都会与之撞文件。**两者不能同时开工**，必须二选一或串行。
> 4. 建议顺序（待拍板）：
>    - 产品更急“去 GLFW”：先做 v3 的 **V3-0 止血（约 1 天）**，再切 C1a/C1b；
>    - 更急 RHI 稳定性：先做 **V3-0→V3-2（约 1 周）**，C1 顺延；
>    - 无论哪种，**L0 的 `DrawUniforms` 迁移建议暂缓**——v3 V3-2 会重做绑定层，
>      现在搬可能白做；`GLProgram`（编译/链接）可以独立先做，不与 v3 冲突。

**文档关系（避免与旧文档冲突，四份文档都不删除）**

| 文档 | 定位 | 权威范围 |
|---|---|---|
| `docs/ROADMAP_ZW3D.md` | 路线图 / 排期 | **优先级、阶段排期、里程碑状态**以它为准 |
| `docs/ARCHITECTURE_INTERFACES.md`（本文） | 接口分层设计 | **L0 / C1 / C5 的接口签名、验收、并行工作包**以本文为准 |
| `docs/RHI_DESIGN.md` | RHI 设计与历史 | **B0 公共契约、后端约定、调试经验**以它为准；状态以 ROADMAP 为准 |
| `docs/RHI_V3_PLAN.md` | RHI v3 重构方案 | **v3 内部结构、绑定层、VkDevice 拆分**以它为准；与本文的排期冲突按上方状态对齐说明裁决 |

约定：

1. 旧文档**保留**：`RHI_DESIGN.md` 里的 M3/B3/A1 调试经验不可替代，删掉是净损失。
2. 本文只补充"接口分层 / 嵌入宿主"这一条线，**不重复** B0 契约的完整定义。
3. 同一件事只在一处展开：排期看 ROADMAP，接口看本文，契约/经验看 RHI_DESIGN。
4. 若发现三处表述不一致，按上表的"权威范围"裁决，并在对应文档里改掉旧表述。

---

## 0. 目标与非目标

### 0.1 目标

1. **engine 不依赖窗口库/UI 库**：引擎代码中不出现 GLFW、ImGui。
2. **rhi 不依赖 render/app**：反向 include 清零，RHI 可独立作为底层库。
3. **宿主用同一套调用驱动 GL / Vulkan**：主循环无后端分支。
4. **公共头不泄漏后端原生类型**：宿主不需要 include glad / Vulkan / GLFW。
5. **失败可诊断、生命周期显式**：创建设备返回错误原因；设备/窗口销毁顺序由宿主控制。

### 0.2 非目标（本轮）

- 不做多线程命令录制、VMA、bindless/BDA、完整 FrameGraph。
- 不迁移 CAD 功能 Pass（阴影/拾取/文字/SDF 等，属 D 系列）。
- 不改变渲染算法、色彩空间、坐标约定（B3a/B3b/B3c 的成果保持）。

---

## 1. 现状诊断

### 1.1 倒挂 A：engine → GLFW（窗口库）

| 位置 | 倒挂内容 |
|---|---|
| `src/rhi/RHIFactory.h` / `.cpp` | `createDevice(Backend, GLFWwindow*, path)` 是唯一实际可用的入口 |
| `src/rhi/IRHIDevice.h` | 前置声明 `struct GLFWwindow;` |
| `src/rhi/gl/GLDevice.h` | 构造函数接收并保存 `GLFWwindow*`，暴露 `window()` |
| `src/rhi/vk/VkDevice.h` | 同上 |
| `src/rhi/vk/VkDevice.cpp` | `glfwGetRequiredInstanceExtensions`、`glfwCreateWindowSurface`、`glfwGetFramebufferSize` |
| `src/rhi/gl/GLCommandList.cpp` | 视口尺寸回查 `glfwGetFramebufferSize` |
| `src/render/RenderSystem.h` / `.cpp` | `setWindow(GLFWwindow*)`、`_window`、`selfTestAB` 回查窗口 |
| `src/CMakeLists.txt` | `engine` 把 `glfw` 作为 **PUBLIC** 依赖链接 |

结论：引擎把"窗口/上下文/帧缓冲尺寸/surface"这些**宿主职责**当成了自己的职责。
B0 已冻结 `DeviceDesc` / `NativeWindow`，但 `RHIFactory::createDevice(const DeviceDesc&)`
至今返回 `nullptr`，C1 只停留在纸面。

### 1.2 倒挂 B：rhi → render（更严重）

| 位置 | 倒挂内容 |
|---|---|
| `src/rhi/gl/GLDevice.cpp` | include `render/shader.h`，复用 `Shader` 编译 GLSL |
| `src/rhi/gl/GLDevice.h` | `GLPipeline` 持有 `std::unique_ptr<Shader>`（render 层类型） |
| `src/rhi/gl/GLCommandList.cpp` | include `render/shader.h` + `render/DrawUniforms.h` |

这不是"少分一层"，而是 **RHI 依赖了构建在它之上的渲染语义**。后果：
RHI 无法独立交付/单测；Shader 一改，GL 后端跟着改；层级检查永远无法通过。

### 1.3 接口债务清单

| # | 问题 | 位置 |
|---|---|---|
| 1 | 帧生命周期两套：GL 路径不调 `beginFrame/endFrame`，VK 路径统一在 `drawRhiScene` | `src/render/RenderSystem.cpp` |
| 2 | 后端选择藏在引擎：读取 `RHI_BACKEND` 环境变量 | `RenderSystem::init` |
| 3 | `RenderSystem` 上帝对象：窗口/设备/Scene/Pass/输入/字体/纹理全在一个单例 | `src/render/RenderSystem.h` |
| 4 | Pass 接口 GL 专属：`IRenderPass` include glad、`draw(Scene*)`；VK 整体绕开 | `src/render/pass/IRenderPass.h` |
| 5 | 公共头泄漏第三方类型：`BaseDraw.h` 暴露 glad/glm；`engine` PUBLIC 链接 glad/glfw/glm/assimp/freetype | `src/render/BaseDraw.h`、`src/CMakeLists.txt` |
| 6 | GL FBO 直通进公共契约 | `RenderPassDesc::framebufferId`（`src/rhi/RhiTypes.h`） |
| 7 | 失败不可诊断：`createDevice` 只返回 `nullptr` + 日志 | `src/rhi/RHIFactory.cpp` |
| 8 | 公共类全在全局命名空间、无 DLL 导出宏 | `Scene` / `Model` / `Camera` / `Light` 等 |
| 9 | 单例不可多实例：`RenderSystem` / `TextureMng` / `ShaderCache` / `Light` 全是单例 | 全局 |
| 10 | 公共头出现 GL 类型：`IRenderPass.h`、`BaseDraw.h` 直接 include glad | 同 #4/#5 |

---

## 2. 目标分层与依赖规则

```
┌─────────────────────────────────────────────────────────────┐
│ 宿主层（ZW3D / GLFW demo app）                                │
│  窗口、GL 上下文、事件循环、ImGui、场景组装                      │
│  只认：engine 公共头 + rhi 公共头 + NativeWindow / DeviceDesc   │
└───────────────┬─────────────────────────────────────────────┘
                │ 传 NativeWindow / 尺寸 / surface 回调 / present
┌───────────────▼─────────────────────────────────────────────┐
│ engine 公共 API（namespace engine，带 ENGINE_API）             │
│  RenderSystem(facade) / Scene / Viewport / RenderSettings     │
│  禁止出现：GLFW、GL、Vk、ImGui 类型                             │
└───────────────┬─────────────────────────────────────────────┘
                │ 只通过 rhi 公共头
┌───────────────▼─────────────────────────────────────────────┐
│ rhi 公共 API（namespace rhi）                                 │
│  IRHIDevice / IRHICommandList / RhiTypes                     │
│  禁止 include：render/、app/、GLFW/、imgui                     │
└───────────────┬─────────────────────────────────────────────┘
                │ 私有实现
┌───────────────▼─────────────────────────────────────────────┐
│ rhi/gl（glad + 自带 GLSL 编译器）   rhi/vk（Vulkan + 平台 surface）│
└─────────────────────────────────────────────────────────────┘
   core（math/log/color）只被上面各层依赖，反向禁止
```

### 2.1 硬规则（L1 用脚本固化）

| 目录 | 禁止 include | 说明 |
|---|---|---|
| `src/rhi/**` | `render/`、`app/`、`GLFW/`、`imgui` | RHI 只能向下依赖 core/第三方实现 |
| `src/core/**` | `rhi/`、`render/`、`app/` | core 是最底层 |
| `src/render/**` | `app/`、`imgui`、`GLFW/` | 引擎层不含 UI/窗口 |
| `engine` target | `glfw`、`imgui` | 链接层面兜底 |

> 说明：`src/render/**` 对 `rhi/` 的依赖是**允许**的（render 在上层）；
> 反向（`src/rhi` → `src/render`）必须为零。

---

## 3. 接口设计

### 3.1 职责边界

| 事项 | 宿主 | engine | rhi |
|---|---|---|---|
| 创建窗口 / GL 上下文 | ✅ | ❌ | ❌ |
| 提供 NativeWindow / 尺寸 / surface 回调 | ✅ | ❌ | 消费 |
| 创建设备 | ✅ | ❌（借用） | 工厂实现 |
| 主循环 / present 时机 | ✅ 驱动 | 内部编排 | 执行 |
| 场景 / Pass / 相机 / 资源 | ❌ | ✅ | ❌ |
| 原生图形 API 调用 | ❌ | ❌ | ✅ |
| ImGui | ✅ | ❌ | ❌ |

核心约定：**宿主拥有窗口与设备，引擎借用设备，RHI 只认 `DeviceDesc`。**

### 3.2 `DeviceDesc` / `NativeWindow`

在 `src/rhi/RhiTypes.h` 中**追加**字段（B0 的"只加不改"）：

```cpp
struct NativeWindow {
    NativeWindowType type = NativeWindowType::None;
    void* handle  = nullptr;
    void* display = nullptr;               // 仅 Xlib：Display*

    // GL 专用：宿主持有的交换回调（C1b 生效）；Vulkan 忽略（内部 present）
    void (*swapBuffers)(void* user) = nullptr;
    void* swapUser = nullptr;

    bool valid() const { return type != NativeWindowType::None && handle != nullptr; }
};

struct DeviceDesc {
    Backend      backend = Backend::OpenGL;
    NativeWindow window;                   // offscreen 时可为空
    bool         offscreen = false;
    uint32_t     width = 0;                // 帧缓冲像素尺寸（不是窗口逻辑尺寸）
    uint32_t     height = 0;
    std::string  appName = "my_app";
    std::string  shaderRootPath;           // 以路径分隔符结尾
    bool         enableValidation = true;
    uint32_t     swapchainImageCount = 0;  // 0 = 后端默认

    // ---- 宿主可选的 Vulkan 逃生口（公共头不出现 Vk 类型）----
    // 宿主填写时优先使用；留空时 RHI 按 NativeWindowType 自行推导。
    std::vector<std::string> instanceExtensions;
    std::function<uint64_t(uint64_t instance)> createSurface; // 返回 VkSurfaceKHR
    uint64_t externalSurface = 0;          // 或直接给已创建 surface（宿主持有）

    // 所有权约定：
    //   externalSurface != 0  → RHI 不销毁，宿主负责
    //   createSurface 返回非 0 → RHI 接管并销毁
    //   RHI 自建 surface      → RHI 销毁
};
```

**为什么保留 `createSurface` 回调（macOS 关键）。**
MoltenVK 的 surface 需要 `CAMetalLayer`。最稳的做法是宿主（示例 app 的 GLFW 适配器）
在拿到 `VkInstance` 后调用 `glfwCreateWindowSurface`；引擎既不碰 GLFW，
也不需要 Objective-C++。Windows 宿主（ZW3D）留空即可，由 RHI 从 HWND 自建。

平台映射（RHI 内部）：

| NativeWindowType | 必需实例扩展 | 建面函数 |
|---|---|---|
| Win32 | `VK_KHR_surface` + `VK_KHR_win32_surface` | `vkCreateWin32SurfaceKHR` |
| Xlib | `VK_KHR_surface` + `VK_KHR_xlib_surface` | `vkCreateXlibSurfaceKHR` |
| Xcb | `VK_KHR_surface` + `VK_KHR_xcb_surface` | `vkCreateXcbSurfaceKHR` |
| Wayland | `VK_KHR_surface` + `VK_KHR_wayland_surface` | `vkCreateWaylandSurfaceKHR` |
| MetalLayer | `VK_KHR_surface` + `VK_EXT_metal_surface` | `vkCreateMetalSurfaceEXT` |
| 宿主 `createSurface` | 宿主保证 | 回调 |

CMake 需按平台加私有宏：`VK_USE_PLATFORM_WIN32_KHR`、`VK_USE_PLATFORM_METAL_EXT`、
`VK_USE_PLATFORM_XLIB_KHR`、`VK_USE_PLATFORM_WAYLAND_KHR`。

### 3.3 `IRHIDevice` 增补

```cpp
struct FramebufferSize { uint32_t width = 0; uint32_t height = 0; };

// 默认帧缓冲/离屏目标的像素尺寸。RHI 永不回查窗口系统。
virtual FramebufferSize framebufferSize() const = 0;

// 呈现。GL：调用 NativeWindow::swapBuffers；VK：vkQueuePresentKHR；离屏：no-op。
virtual void present() {}

// onResize(w, h) 已有：两个后端都必须保存尺寸；VK 额外标记 swapchain 重建。
```

同时删除 `IRHIDevice.h` 中的 `struct GLFWwindow;` 前置声明。

### 3.4 `RHIFactory`

```cpp
// 唯一入口。失败返回 nullptr 并填充 outError。
static std::unique_ptr<IRHIDevice> createDevice(const DeviceDesc& desc,
                                                std::string* outError = nullptr);

// 删除旧入口：
// static std::unique_ptr<IRHIDevice> createDevice(Backend, GLFWwindow*, const std::string&);
```

GL 后端前提：宿主已创建并把 GL 上下文设为 current；RHI 不创建上下文。

### 3.5 `RenderSystem` 门面

```cpp
struct InitDesc {
    rhi::IRHIDevice* device = nullptr;   // 宿主持有；引擎不销毁（可共享设备）
    std::string assetRoot;               // 含 res/ 的根目录
    uint32_t frameWidth = 0;
    uint32_t frameHeight = 0;
};

bool init(const InitDesc& desc, std::string* outError = nullptr);
void shutdown();                         // 只销毁引擎自己的资源，不销毁 device
void renderFrame();                      // 完整一帧：begin → passes → overlay → end → present
void onResize(uint32_t w, uint32_t h);   // 转发 device + RenderContext + camera
void setOverlayRenderer(std::function<void(rhi::IRHICommandList*)>); // UI 合成点

rhi::IRHIDevice* rhiDevice();            // 只读借用
```

**删除**：`setWindow(GLFWwindow*)`、`_window`、`RenderSystem` 内的 `RHI_BACKEND` 读取。
后端由 `device->backend()` 得到；环境变量解析上移到 app。

**生命周期约定**（宿主必须遵守）：

```
window 创建 → GL 上下文 current（GL）/ 设备创建 → renderer.init
  → 每帧 renderer.renderFrame()
  → renderer.shutdown() → device 销毁 → window 销毁
```

`shutdown()` 内先 `device->waitIdle()`，再销毁引擎资源（冒烟三角形、四边形、
Pass、字体纹理），最后清空指针；**不** `reset()` 设备。

### 3.6 统一帧流程

```
device->beginFrame()        // VK: acquire；GL: no-op
  engine passes / overlays
  overlayFn(cmd)            // UI 合成点：GL 默认帧缓冲已绑定；VK 在 pass 内
device->endFrame()          // VK: submit；GL: no-op
device->present()           // VK: present；GL: swapBuffers 回调
```

| 事项 | GL | Vulkan |
|---|---|---|
| beginFrame | no-op | acquire image / fence |
| passes | 现有 GL Pass（C1 阶段保留） | `drawRhiScene` |
| overlay | `ImGui_ImplOpenGL3_RenderDrawData` | `ImGui_ImplVulkan_RenderDrawData` |
| endFrame | no-op | submit + signal render-done |
| present | `NativeWindow::swapBuffers` | `vkQueuePresentKHR` |

宿主主循环：

```cpp
ImGui_Impl*_NewFrame();
ImGui::NewFrame();
DebugPanel::draw(...);
ImGui::Render();
renderer.renderFrame();     // 内部含 overlay + present
glfwPollEvents();
```

这样 GL / VK 的 main 循环零分支，ImGui 时序也一致。

### 3.7 RHI 内聚：Shader / DrawUniforms 下沉

**新增 `src/rhi/gl/GLProgram.{h,cpp}`**：把 `Shader` 的 GLSL 编译/链接/uniform 设置
搬进 RHI，只依赖 glad 与文件读取。

```cpp
namespace rhi {
class GLProgram {
public:
    ~GLProgram();
    bool compile(const std::string& vertPath,
                 const std::string& fragPath,
                 const std::string& geomPath = "");   // 失败返回 false 并记录日志
    void use() const;
    GLuint id() const { return _id; }

    void setBool (const char* name, bool v) const;
    void setInt  (const char* name, int v) const;
    void setFloat(const char* name, float v) const;
    void setFloat3(const char* name, const float v[3]) const;
    void setFloat4(const char* name, const float v[4]) const;
    void setMat4 (const char* name, const float m[16]) const;
    void setMat4Array(const char* name, const float* m, int count) const;

private:
    GLint loc(const char* name) const;   // 缓存 glGetUniformLocation
    GLuint _id = 0;
    mutable std::unordered_map<std::string, GLint> _locs;
};
}
```

- `GLPipeline` 改为持有 `std::unique_ptr<GLProgram>`；`GLDevice.h` 删除 `class Shader;`。
- `GLDevice::createPipeline`：`GLProgram::compile` 失败时记录错误并返回 invalid 句柄
  （不再 `assert(0)` 中止进程）。
- `GLCommandList.cpp` 删除 `shader.h`、`math/matrix.h`、glm include；
  `setMat4` 直接传 `u.data`（原始 float[16]）。

**新增 `src/rhi/BuiltinUniforms.h`**：把 160B 绘制级 uniform ABI 放到 RHI。

```cpp
namespace rhi {
struct BuiltinDrawUniforms {           // 布局与 VK 着色器 FrameData 一致
    glm::vec4 lightPosition_isOpen;
    glm::vec4 lightDirection_cosTheta;
    // ... 字段名保持不变
};
static_assert(sizeof(BuiltinDrawUniforms) == 160, "RHI builtin uniform ABI");
}
```

- `src/render/DrawUniforms.h` 保留为过渡别名：
  `namespace engine { using DrawUniforms = rhi::BuiltinDrawUniforms; }`
- 生产方 `mesh_pass.cpp`、`RenderSystem.cpp` 改用 RHI 类型。
- 该头为 **RHI 内部 ABI 头**，不进入对外契约；后续可改成纯 `float[4]` 去掉 glm。

### 3.8 Pass 接口统一（C1 之后）

```cpp
// src/render/pass/IRenderPass.h（不再 include glad）
struct RenderContext {
    rhi::IRHIDevice* device = nullptr;
    uint32_t viewportWidth = 0;
    uint32_t viewportHeight = 0;
    uint32_t frameIndex = 0;
};

class IRenderPass {
public:
    virtual ~IRenderPass() = default;
    virtual void record(RenderContext& ctx, rhi::IRHICommandList& cmd) = 0;
};
```

- `Shadow/Normal/Sky/Pick` 改为只录制 RHI 命令；GL/VK 跑**同一份 Pass 列表**。
- `RenderPassDesc::framebufferId` 在 B2/B4 接入 `RenderTargetHandle` 后删除。
- 依赖：B2（GL v2 能力）+ B4（离屏 RT / readback）。

### 3.9 公共 API 卫生（L2）

- **命名空间**：公共类收进 `namespace engine`，头文件统一 `engine/` 前缀。
  避免与宿主（ZW3D）的 `Scene` / `Model` / `Camera` / `Light` 撞名。
- **导出宏**：`ENGINE_API`（Windows DLL / 默认可见性），为 F2 打包准备。
- **第三方类型**：公共头去 glad/GLFW/Vulkan 后，`engine` 的对应依赖改 PRIVATE。
- **单例**：保留 `RenderSystem` 单例给 demo；资源管理器改由 `RenderContext` 持有，
  为多实例/共享设备让路（L2c；与 C2 同改 `RenderContext`，需串行或同人）。

### 3.10 错误与能力

```cpp
// 设备创建：失败原因走 outError，不抛异常
std::unique_ptr<IRHIDevice> dev = rhi::RHIFactory::createDevice(desc, &err);

// 引擎初始化：同上
if (!renderer.init({dev.get(), assetRoot, w, h}, &err)) { /* 宿主决定降级/退出 */ }

// 能力探测：GL 在 B2 对齐 VK；上层按 DeviceCapabilities 字段选路径
rhi::DeviceCapabilities caps = dev->capabilities();
```

---

## 4. 实施计划

> 规模：S ≈ 1–2 天，M ≈ 3–5 天（单人）。
> 每个阶段单独分支/PR，master 始终保持 GL + VK 可运行。

### 4.0 总览

图例：🔒 = 必须串行；✅ = 可与其他工作包并行；⛔ = 必须最后单独做。

| 阶段 | 内容 | 依赖 | 规模 | 并行性 | 主要文件 |
|---|---|---|---|---|---|
| **L0** | RHI 去 render：`GLProgram` + `BuiltinUniforms` 下沉 | 无 | S | 🔒 G1 内串行 | `src/rhi/gl/*`、`src/rhi/BuiltinUniforms.h` |
| **C1a** | RHI 去 GLFW：`DeviceDesc` 落地、平台 surface、尺寸推送 | L0 | M | 🔒 G1 内串行 | `RhiTypes.h`、`IRHIDevice.h`、`RHIFactory.*`、`GLDevice.*`、`VkDevice.*` |
| **C1b** | `RenderSystem` 门面：`InitDesc`、`renderFrame`、present 统一 | C1a | M | 🔒 G1 内串行 | `RenderSystem.*`、`app/main.cpp` |
| **C5** | 示例 app 重构为消费者：`GlfwHost` + `SceneDemo` | C1b | S | ✅ 可与 B2/B4/L1 并行 | `app/*` |
| **L1** | 分层检查脚本 + CMake 依赖收紧 | 无（warn-only 起步） | S | ✅ 可立即并行 | `tools/check_layers.cmake`、顶层 `CMakeLists.txt` |
| **L2** | 公共 API 卫生：命名空间/导出宏/公共头去 GL | C5 + B2/B4 | M | ⛔ 最后单独做 | 公共头、`BaseDraw.h` |
| **B2** | GL v2：sampler / render target / readback / caps | G1（或 C1a，见 §4.8.1） | M | ✅ 与 B4 并行（GL/VK 文件不重叠） | `src/rhi/gl/*` |
| **B4** | VK 离屏 RT + readback | G1（或 C1a，见 §4.8.1） | M | ✅ 与 B2 并行（GL/VK 文件不重叠） | `src/rhi/vk/*` |

> **L0 + C1a + C1b 合成串行门禁 G1**：三者共享 `RhiTypes.h` / `IRHIDevice.h` /
> `RHIFactory.*` / `VkDevice.*`，必须单机单人连续完成（建议同一分支、三个 commit），
> 合并后打 tag `rhi-c1-gate`，其他工作包从该 tag 拉分支。
> 分开合入会出现"引擎去了一半 GLFW、主循环无法运行"的中间态。
> 并行编排详见 §4.8。

### 4.1 L0 —— RHI 去 render（无行为变化）

> 并行性：🔒 G1 串行门禁，不可与其他 RHI 改动并行。

**目标**：`src/rhi` 不再 include `src/render` 的任何头。

**步骤**

1. 新建 `src/rhi/gl/GLProgram.h/.cpp`（接口见 3.7）。
   - 从 `src/render/shader.cpp` 提取编译/链接逻辑；保留日志宏。
   - uniform location 首次查询后缓存。
2. `GLDevice.h`：`GLPipeline` 的 `unique_ptr<Shader>` → `unique_ptr<GLProgram>`；
   删除 `class Shader;` 前置声明。
3. `GLDevice.cpp`：`createPipeline` 改用 `GLProgram::compile`。
4. `GLCommandList.cpp`：删除 `#include "shader.h"`、`render/DrawUniforms.h`、
   `math/matrix.h`、glm 引用；改用 `rhi/gl/GLProgram.h` 与 `rhi/BuiltinUniforms.h`。
5. 新建 `src/rhi/BuiltinUniforms.h`；更新 `mesh_pass.cpp`、`RenderSystem.cpp` 的类型引用。
6. `src/render/DrawUniforms.h` 改为别名，保留旧 include 兼容。
7. 重新 configure（`file(GLOB)` 不会自动发现新文件）：
   `cmake --preset <preset>`。

**验收**

```bash
# 反向依赖清零
rg -n 'render/|shader\.h' src/rhi
# 期望：只命中 rhi 自身头（BuiltinUniforms 说明注释除外）

# 双后端回归（macOS / Windows）
RHI_SMOKE=1 RHI_TEXQUAD=1 RHI_PROBE=1 RHI_EXIT_AFTER=120 ./my_app
RHI_BACKEND=vulkan RHI_SMOKE=1 RHI_TEXQUAD=1 RHI_PROBE=1 RHI_EXIT_AFTER=120 ./my_app
```

- `RHI_PROBE` 中心像素与改动前一致（BGRA 215,140,122 / RGB 122,140,215）。
- `RHI_TEXQUAD` 中心像素精确 180,60,220。
- 验证层 0 error。

**风险/回退**

- `GLProgram::compile` 失败从 `assert` 改为返回 false + 日志：编译失败时渲染为空白，
  不再崩溃。这是有意的行为改进，需在 PR 说明。
- 回退：L0 是纯内部重构，可按文件回退。

### 4.2 C1a —— RHI 去 GLFW

> 并行性：🔒 G1 串行门禁（与 L0/C1b 同一分支、同一负责人）。

**目标**：`src/rhi/` 中不再出现 `GLFW` / `glfw`（`src/render/RenderSystem.cpp` 的临时桥接保留到 C1b）。

**步骤**

1. `RhiTypes.h`：按 3.2 追加 `NativeWindow::swapBuffers`、`DeviceDesc::width/height/
   instanceExtensions/createSurface/externalSurface`；加 `<functional>`。
2. `IRHIDevice.h`：加 `FramebufferSize`、`framebufferSize()`、`present()`；删除 GLFW 前置声明。
3. `RHIFactory`：实现 `createDevice(const DeviceDesc&, std::string*)`；删除 GLFW 重载。
   - GL：`GLDevice(desc)`；要求宿主已绑定当前上下文。
   - VK：`VulkanDevice(desc)`。
4. `GLDevice`：
   - 构造函数改为 `GLDevice(const DeviceDesc&)`；保存 `_size`。
   - 实现 `framebufferSize()`、`onResize()`；删除 `_window` / `window()`。
   - `present()`：调 `desc.window.swapBuffers`（C1b 接线）。
5. `GLCommandList::beginRenderPass`：视口回退改为 `_device->framebufferSize()`。
6. `VulkanDevice`：
   - 构造函数改为 `VulkanDevice(const DeviceDesc&)`；保存 `_size`、`_desc`。
   - `createInstance`：优先用 `desc.instanceExtensions`；否则按 `NativeWindowType` 推导
     （见 3.2 表）；再追加 portability / debug_utils / get_properties2。
   - `createSurface`：优先 `desc.externalSurface` → `desc.createSurface(instance)` →
     按平台自建；记录所有权。
   - `createSwapchainObjects`：`currentExtent == UINT32_MAX` 时用 `_size`。
   - `onResize`：保存尺寸 + 标记重建；`framebufferSize()` 返回保存值。
   - 删除 `_window` / `window()`。
7. `src/CMakeLists.txt`：
   - `target_link_libraries(engine PUBLIC glad glm assimp freetype)`，移除 `glfw`。
   - Vulkan 平台宏：
     ```cmake
     if(WIN32)        target_compile_definitions(engine PRIVATE VK_USE_PLATFORM_WIN32_KHR)
     elseif(APPLE)    target_compile_definitions(engine PRIVATE VK_USE_PLATFORM_METAL_EXT)
     elseif(UNIX)     target_compile_definitions(engine PRIVATE VK_USE_PLATFORM_XLIB_KHR)
     endif()
     ```
8. `RenderSystem`：**临时**在 `RenderSystem.cpp` 内增加 `GLFWwindow* → DeviceDesc` 桥接
   （GLFW 仍属 render 层，C1b 再移到 app）；`app/main.cpp` 本步不改。
   GL 上下文由 main 创建并 current；桥接只负责填 `NativeWindow`、尺寸、surface 回调。

**验收**

```bash
rg -n 'GLFW|glfw' src/rhi         # 期望：无输出（RHI 不再依赖窗口库）
rg -n 'glfw' src/CMakeLists.txt   # 期望：无输出（engine 不再链接 glfw）
# 注：src/render/RenderSystem.cpp 的临时 GLFW 桥接保留到 C1b
```

- Windows / macOS 上 GL + VK 均可启动。
- resize 后画面正确（`RHI_RESIZE_AT=60`）。
- ImGui 面板可见（VK 用 `nativeHandles`，不依赖引擎窗口）。
- `RHI_PROBE` / `RHI_TEXQUAD` 与改动前一致。

**风险/回退**

- macOS surface：示例 app 必须提供 `createSurface` 回调（内部调 `glfwCreateWindowSurface`）；
  否则 VK 启动会失败。这是有意设计，文档写清楚。
- Vulkan 平台扩展/宏缺失会导致 `vulkan.h` 不声明建面函数；CMake 宏必须覆盖三平台。
- 回退：保留旧 `GLFWwindow*` 重载在分支内，确认后再删除。

### 4.3 C1b —— `RenderSystem` 门面 + 帧流程统一

> 并行性：🔒 G1 串行门禁；C1b 合并后才释放 GL / VK 两条并行线。

**目标**：引擎不持有窗口；宿主用一次 `renderFrame()` 驱动两端。

**步骤**

1. `RenderSystem.h`：新增 `InitDesc`、`init(InitDesc, error)`、`renderFrame()`、
   `onResize(w,h)`；删除 `setWindow`、`_window`。
2. `RenderSystem.cpp`：
   - 删除 `#include <GLFW/glfw3.h>` 与 `RHI_BACKEND` 解析；`_backend = device->backend()`。
   - `init` 使用宿主传入的设备，创建冒烟/四边形资源；`assetRoot` 推导 shader 根目录。
   - `draw()` 改名/重组为 `renderFrame()`：
     ```
     beginFrame → (GL: 现有 Pass / VK: drawRhiScene) → overlay → endFrame → present
     ```
   - `selfTestAB` 用 `rhiDevice()->framebufferSize()`。
   - `shutdown` 不销毁设备。
3. `app/main.cpp`：
   - 解析 `RHI_BACKEND`（默认 OpenGL）→ 构建 `DeviceDesc` → `createDevice` →
     `RenderSystem::init`。
   - ImGui `NewFrame` 移到 `renderFrame` 之前；GL 的 ImGui 绘制改为
     `setOverlayRenderer`（内部 `ImGui_ImplOpenGL3_RenderDrawData`）。
   - 删除 `glfwSwapBuffers`（由 `present()` 负责）。
   - `RHI_SMOKE` / `RHI_TEXQUAD` / `RHI_CLIP` 在 init 后写入 `settings()`。
4. `GLDevice::present()` 调 `NativeWindow::swapBuffers`；`VkDevice` 拆分
   `endFrame`（submit）与 `present`（present）。

**验收**

- GL / VK 主循环无 `if (useVulkan)` 分支（ImGui backend 初始化除外）。
- `rg -n 'GLFW|glfw' src/` 仍为空。
- 双后端冒烟/像素回归通过；resize 正常；UI 可见。
- `RHI_EXIT_AFTER` 正常退出（析构顺序正确）。

**风险/回退**

- VK `endFrame`/`present` 拆分需保持 render-done semaphore 与 image index 的一致性，
  验证层必须 0 error。
- GL 的 ImGui 从"draw 之后"改到 overlay 回调，需确保 `GL_FRAMEBUFFER_SRGB`
  在 UI 绘制时关闭（沿用现有逻辑）。
- 回退：保留 `draw()` 旧入口一个版本，内部转调 `renderFrame()`。

### 4.4 C5 —— 示例 app 重构为"引擎库的消费者"

> 并行性：✅ G1 合并后可与 B2 / B4 / L1 并行；独占 `app/*`。

**目标**：`main.cpp` 只做启动编排，窗口/ImGui/场景分别成文件。

**步骤**

1. 新建 `app/GlfwHost.{h,cpp}`：
   - 创建窗口/GL 上下文；填 `rhi::NativeWindow`；
   - GL：设置 `swapBuffers` 回调；
   - VK：提供 `createSurface` 回调（`glfwCreateWindowSurface`）；
   - ImGui 平台/渲染后端初始化与关闭；
   - 窗口事件回调转发到 `RenderSystem`。
2. 新建 `app/SceneDemo.{h,cpp}`：把 `testRenderSystem()` 的场景组装搬过去。
3. `app/main.cpp` 缩到启动 → 循环 → 关闭三段。

**验收**

- `main.cpp` ≤ ~150 行；无场景/模型/资源细节。
- `rg -n 'ImGui' src/` 为空（UI 只在 app）。
- 双后端运行正常。

### 4.5 L1 —— 分层检查固化

> 并行性：✅ 可立即开始（warn-only）；G1 合并后把规则转为 error。

**步骤**

1. 新建 `tools/check_layers.cmake`（避免 Python 依赖）：
   - 扫描 `src/rhi`、`src/core`、`src/render` 的 `#include`；
   - 命中禁止规则时 `message(SEND_ERROR ...)`；
   - 检查 `src/CMakeLists.txt` 的 `engine` 链接列表不含 `glfw` / `imgui`。
2. `CMakeLists.txt` 增加自定义目标：
   ```cmake
   add_custom_target(check_layers
       COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/tools/check_layers.cmake)
   ```
3. 在 CI / 提交前钩子中调用 `cmake --build <build> --target check_layers`。

**验收**：手动制造一条反向 include，`check_layers` 必须失败并指出文件。

### 4.6 L2 —— 公共 API 卫生

> 并行性：⛔ 最后单独做（改全部公共头），需 C5 + B2/B4 完成后冻结接口。

建议拆三小步，避免一次性大改：

| 子步 | 内容 | 验收 |
|---|---|---|
| L2a | 公共类进 `namespace engine`；加 `ENGINE_API`；`engine/` 头前缀 | demo 与宿主都能编译；无全局同名冲突 |
| L2b | 公共头去 GL：`IRenderPass`、`BaseDraw`、对象头不再 include glad；`glad` 转 PRIVATE | `rg -n 'glad' src/*.h src/**/*.h` 只允许出现在 `rhi/gl` |
| L2c | 资源管理器去单例：`TextureMng/FontManager/ShaderCache` 由 `RenderContext` 持有 | 可创建两个 RenderContext（测试用） |

L2b 依赖 Pass 统一（3.8）；如果 Pass 迁移尚未完成，可先做 L2a。

### 4.7 后续（与现有路线图衔接）

| 顺序 | 工作 | 依赖 | 说明 |
|---|---|---|---|
| 1 | B2：GL v2（sampler / render target / readback / caps） | L0/C1 | GL 回退路径必须具备与 VK 对等能力 |
| 2 | B4：离屏 RT + readback | C1（VK 尺寸接口） | 宿主嵌入、缩略图、拾取的前置 |
| 3 | Pass 统一 + `framebufferId` 删除 | B2/B4 | GL/VK 同一 Pass 列表 |
| 4 | C2 多视口 / D2 拾取 / D4 SDF 文字 | B4 | CAD 显示能力 |
| 5 | C4 设备丢失 / E1/E2 / F1/F2 | 上述 | 产品化 |

### 4.8 并行编排与多机工作包

> 目标：让不同机器/同事同时开工而**不改同一个文件**。
> 核心结论：**只有 G1（L0 + C1a + C1b）必须串行**；G1 合并后 GL 与 VK
> 可以各占一台机器并行，功能类工作（D3/D4/D5）甚至可以在 G1 之前就并行。
> **实际只有两台机器时，按 §4.8.8 的两机分工执行**；4.8.1–4.8.7 是工作包视图，
> 其中的 A/B/C/D 可映射到 M1/M2。

#### 4.8.1 时间轴

```
             G1（串行门禁，单机单人）                 G1 之后（可并行）
机器 A   [L0]──[C1a]──[C1b] ──────────────► tag rhi-c1-gate ──► [C5 app 消费者]
机器 B   [D4 SDF 文字] ────────────────────► 等 G1 ────────────► [B2 GL v2] ──► [Pass 统一]
机器 C   [D3 透明 / D5 显示模式] ──────────► 等 G1 ────────────► [B4 VK 离屏] ──► [Pass 统一]
机器 D   [L1 分层脚本 warn-only] ─[A4 资源路径] ───────────────► [F1 headless 脚手架]
```

G1 期间只允许改：`src/rhi/**`、`src/render/RenderSystem.*`、`app/main.cpp`、
`src/CMakeLists.txt`。其他机器的分支**不得触碰**这些路径。

> **可选加速（排期紧张时）**：B2 / B4 技术上只依赖 **C1a**（RHI 接口稳定），
> 不依赖 C1b。若产品优先 B2/B4，可让 P-GL / P-VK 从 C1a 合并点拉分支，与
> C1b（只改 `RenderSystem.*` / `app/*`）并行；前提是 C1a 先把设备的
> `present()` 拆分做完。默认仍按 G1 整体合并，减少跨分支协调。

#### 4.8.2 G1 合并后可同时开工

| 工作包 | 建议机器 | 起始条件 | 独占文件 | 可与谁并行 | 交付物 |
|---|---|---|---|---|---|
| **P-GL**（B2） | B | G1 | `src/rhi/gl/*`（新增 `GLRenderTarget.*`） | P-VK、P-APP、P-TOOLS | GL sampler/RT/readback/caps + 回读像素测试 |
| **P-VK**（B4） | C | G1 | `src/rhi/vk/*`（新增 `VkRenderTarget.*`） | P-GL、P-APP、P-TOOLS | VK RenderTarget/readback + 离屏冒烟 |
| **P-APP**（C5） | A 或 D | G1 | `app/*` | P-GL、P-VK、P-TOOLS | `GlfwHost` / `SceneDemo` / slim main |
| **P-TOOLS**（L1） | D | **立即**（warn-only） | `tools/`、顶层 `CMakeLists.txt` | 全部 | `check_layers` 目标；G1 后转 error |
| **P-PASS**（Pass 统一） | B **或** C（单人） | B2 + B4 完成 | `src/render/pass/*`、`IRenderPass.h` | P-APP | `record(ctx, cmd)` + GL/VK 同 Pass 列表 |

> P-PASS 需要同时读 GL 与 VK 的离屏接口，但**只能有一个负责人**改 `pass/*`；
> 另一人只提供后端接口与测试用例。

#### 4.8.3 G1 之前就能并行（不碰 RHI/RenderSystem）

| 工作包 | 建议机器 | 独占文件 | 边界 |
|---|---|---|---|
| **P-CAD-TEXT**（D4 SDF 文字） | B | `FontManager.*`、`object/Text.*`、`shaders/*font*` | 用现有 RHI 纹理/管线，不改公共头 |
| **P-CAD-UI**（D3 透明 / D5 显示模式） | C | `scene.*`、`model/mesh_pass.*`、`shaders/*` | 用现有 blend/pipeline 状态；与 P-PASS 交接 `mesh_pass` |
| **P-IO**（A4 UTF-8 路径） | D | `image.*`、`model.*`、`shader.*` | **`VkDevice` 的 pipeline cache 路径部分移交 G1**，避免冲突 |
| **P-TOOLS**（L1） | D | `tools/` | 先 warn-only，G1 后转 error |

#### 4.8.4 必须串行 / 需要协调

| 工作 | 原因 | 最早可做 |
|---|---|---|
| **D1 宽线/边线** | 需新增 `GraphicsPipelineDesc` 字段 + GL/VK 后端支持（`RhiTypes.h`/两个后端） | G1 之后 |
| **D6 MSAA** | 改 `VkDevice` 管线/附件 | G1 + B4 之后 |
| **C2 多视口** | 依赖 G1 的 `RenderContext`/尺寸接口 | G1 之后 |
| **L2a 命名空间/导出宏** | 改全部公共头 | C5 + B2/B4 公共头增量冻结后 |
| **L2c 资源去单例** | 改 `RenderSystem`/`TextureMng`/`FontManager` | C5 + D4 之后 |
| **C4 设备丢失恢复** | 改 `VkDevice` 生命周期 | G1 + B4 之后 |
| **F1 headless** | 依赖 B4 的离屏/回读 | B4 之后（脚手架可提前） |

#### 4.8.5 文件所有权（硬约束）

| 文件/目录 | 唯一负责人 |
|---|---|
| `src/rhi/RhiTypes.h`、`IRHIDevice.h`、`RHIFactory.*` | **G1**（公共头增量统一收口） |
| `src/rhi/gl/GLDevice.*`、`GLCommandList.*` | G1（L0/C1a）→ 之后 **P-GL** |
| `src/rhi/vk/VkDevice.*`、`VkCommandList.*` | G1（C1a/C1b）→ 之后 **P-VK** |
| `src/render/RenderSystem.*` | G1（C1b）→ 之后 **P-APP** |
| `app/*` | G1（C1a 的窗口适配）→ 之后 **P-APP** |
| `src/render/pass/*`、`IRenderPass.h` | **P-PASS**（G1 后） |
| `src/render/model/mesh_pass.*` | G1（L0 类型名）→ **P-PASS / P-CAD-UI** 二选一 |
| `src/render/FontManager.*`、`object/Text.*` | **P-CAD-TEXT** |
| `src/render/object/Line.*`、`shaders/*line*` | D1 负责人（G1 后） |
| `src/render/DrawUniforms.h`、`shader.*` | G1（L0）后冻结 |
| `tools/`、顶层 `CMakeLists.txt` | **P-TOOLS** |
| 公共头（namespace/export） | **L2a**，最后统一做 |

规则：

1. 同一时间每个文件只有一个负责人；跨包改动先发"接口增量 PR"（只加字段/方法），
   各包再基于新接口实现。
2. 新能力尽量落**新文件**（`GLRenderTarget.cpp`、`VkRenderTarget.cpp`、
   `check_layers.cmake`），减少对既有大文件的并发修改。
3. 公共头（`RhiTypes.h` / `IRHIDevice.h`）的"只加不改"增量由 G1 负责人统一收口；
   P-GL/P-VK 需要的新字段先提给 G1，不自行改公共头。
4. 着色器按功能前缀分配，避免同文件冲突：`*line*`（D1）、`*font*`（D4）、
   `*msaa*`（D6）、`*model*`（P-PASS 统一改）。

#### 4.8.6 分支、tag 与合并节奏

```
master
 ├─ feat/c1-deglfw              ← 含 L0 / C1a / C1b 三个 commit（G1）
 │                               合并后打 tag rhi-c1-gate
 │    ├─ feat/b2-gl-v2          ← 从 rhi-c1-gate 拉
 │    ├─ feat/b4-vk-offscreen   ← 从 rhi-c1-gate 拉
 │    ├─ feat/c5-app-consumer   ← 从 rhi-c1-gate 拉
 │    └─ feat/l1-layer-check    ← 可提前从 master 拉（warn-only）
 ├─ feat/d4-sdf-text            ← 可与 G1 并行
 ├─ feat/d3d5-transparency-modes← 可与 G1 并行
 └─ feat/l2-public-api          ← B2/B4 之后，单独冻结点
```

- **G1 是唯一串行门禁**；合并后打 tag，其他包从该 tag 拉分支。
- G1 期间其他分支合入前必须 rebase 到最新 master；冲突由后合并方解决。
- `master` 始终保持 GL + VK 冒烟通过（`RHI_EXIT_AFTER` 正常退出）。
- 每周至少合并一次；公共头冻结后不再接受"顺手改"。

#### 4.8.7 交叉验收分工

| 工作包 | 自测 | 交叉验收（另一台机器） |
|---|---|---|
| G1 | 双后端冒烟 + 像素探针 + `rg GLFW src/` 为空 | Windows + macOS 各跑一遍 |
| P-GL | GL 离屏回读像素精确 | VK 机器确认未回归 |
| P-VK | VK 验证层 0 error + 离屏 dump | GL 机器确认未回归 |
| P-PASS | 同帧 A/B 逐像素等价 | 双后端各跑一次 |
| P-CAD | 固定场景截图/目视 | 另一 GPU 用结构断言（不比逐字节） |
| L2a | 宿主 demo 编译通过 | 另一平台编译通过 |

#### 4.8.8 两机分工（执行版）

> **状态提示（2026-11-24）**：B4 / D2 / E1 / A4 已在 master 完成；本节“阶段 2/5”
> 的工作包已不再适用，需按 `RHI_V3_PLAN.md` 的 v3 窗口重新排序。
> 分工方法（M1/M2、文件所有权、VkDevice 令牌）仍然有效。

> 前提：只有 **M1、M2** 两台机器。§4.8.1–4.8.7 的 A/B/C/D 是工作包标签，
> 实际按本节合并到两台机器。**同一文件同一时间只有一个负责人。**

##### 1. 角色

| 机器 | 定位 | 主责 |
|---|---|---|
| **M1** | RHI / 接口 | G1（L0/C1a/C1b）、B2（GL v2）、Pass 统一、D1（宽线）、L2a（公共 API）、E1/E2 |
| **M2** | 功能 / 宿主 | G1 期间：A4 资源/IO、D4 SDF 文字、L1；G1 后：B4（VK 离屏）、C5（app）、C2（多视口）、D2/D8（拾取/高亮）、L2b/F1/F2 |

##### 2. 时间轴

```text
阶段 1（G1，串行门禁，M1 独占）
  M1  [L0]──[C1a]──[C1b]──────────────────────────────► 合并，tag rhi-c1-gate
  M2  [A4 资源/IO]──[D4 SDF 文字]──[L1 warn-only]──────► 随时合并（不碰 G1 文件）

阶段 2（后端分治）
  M1  [B2 GL v2]──────────────────────────►
  M2  [B4 VK 离屏/readback]───────────────►
  （src/rhi/gl/* 与 src/rhi/vk/* 不重叠）

阶段 2.5（C2 前置接口，短同步）
  M1  公共头加 setScissor + GL 实现
  M2  等 M1 头文件合入后实现 VK 侧
  （1–2 天；之后 C2 不再需要改后端）

阶段 3（应用层 / Pass）
  M1  [Pass 统一（pass/*，单人）]─────────►
  M2  [C5 app 消费者（app/*）]────────────►
  （C5 不依赖 B2/B4，可提前到阶段 2 空档做）

阶段 4（CAD 功能）
  M1  [D1 宽线（RhiTypes + GL/VK 管线）]──►   ← 此阶段 M1 持有 VkDevice
  M2  [C2 多视口（camera/RenderSystem）]──►   ← 此阶段 M2 不碰 VkDevice

阶段 5（拾取 / 高亮）
  M2  [D2 拾取（PickPass + VkDevice 回读）]►  ← VkDevice 交给 M2
  M1  [L2b 公共头去 GL / L2a 准备]────────►   ← 不碰 VkDevice

阶段 6（公共 API 冻结 / 大场景）
  M1  [L2a 命名空间/导出宏] ──────────────►   ← 冻结窗口，M2 只做本地 rebase
  M1  [E1/E2 VMA/上传环]
  M2  [F1/F2 打包/CI] / [D8 高亮]
```

##### 3. 工作包分配表

| 工作包 | 机器 | 起始条件 | 独占文件 | 可并行 | 必须串行 |
|---|---|---|---|---|---|
| G1 | M1 | 立即 | `src/rhi/**`、`RenderSystem.*`、`app/main.cpp`、`src/CMakeLists.txt` | M2 的 A4/D4/L1 | 任何 RHI/RenderSystem 改动 |
| A4 资源/IO | M2 | 立即 | `image.*`、`model/model.*`、`shader.*` | G1 | `VkDevice` 的 pipeline cache 路径部分（交给 M1） |
| D4 SDF 文字 | M2 | 立即 | `FontManager.*`、`object/Text.*`、`shaders/*font*` | G1、A4 | 公共头 |
| L1 分层脚本 | M2 | 立即 | `tools/`、顶层 `CMakeLists.txt` | 全部 | 无 |
| B2 GL v2 | M1 | G1 | `src/rhi/gl/*` | B4、C5 | G1；D1（同 GLDevice） |
| B4 VK 离屏 | M2 | G1 | `src/rhi/vk/*` | B2、C5 | G1；D1/D2/E1（同 VkDevice） |
| C5 app | M2 | G1 | `app/*` | B2、B4、Pass | 无（app 独占） |
| Pass 统一 | M1 | B2+B4 | `pass/*`、`IRenderPass.h` | C5、C2 | `mesh_pass.*` 与 D5 协调 |
| C2 多视口 | M2 | C1b + `setScissor` | `camera/*`、`RenderSystem.*` | Pass、D1 | L2a；M1 的 D1 期间不碰 VkDevice |
| D1 宽线 | M1 | B2+B4 | `Line.*`、`RhiTypes.h`、GL+VK 管线 | C2、C5 | L2a；D2/E1（同 VkDevice） |
| D2 拾取 | M2 | B4 | `PickPass.*`、`VkDevice.*` | D8、C2 | D1/E1（同 VkDevice） |
| D8 高亮 | M2 | D2 | 渲染/着色器 | D2 后续 | 无 |
| L2a 命名空间 | M1 | D1 之后（公共头稳定） | 全部公共头 | 无（冻结窗口） | 所有工作包暂停合并 |
| L2b 公共头去 GL | M1 | Pass 统一后 | `BaseDraw.h`、`object/*`、`pass/*` 头 | L2a 准备 | 公共头 |
| E1/E2 | M1 | D2 之后（VkDevice 空闲） | `VkDevice.*`、新 VMA/上传模块 | F1/F2 | D2/C4（同 VkDevice） |
| F1/F2 | M2 | B4/C4 | `tools/`、打包脚本 | E1/E2 | 无 |

##### 4. 文件所有权与交接

| 文件/目录 | 阶段 1 | 阶段 2+ |
|---|---|---|
| `RhiTypes.h`、`IRHIDevice.h`、`IRHICommandList.h` | M1 | **M1**（接口增量统一收口） |
| `src/rhi/gl/*` | M1 | M1（B2 / D1） |
| `src/rhi/vk/*` | M1（C1a/C1b） | M2（B4 / D2）↔ M1（D1 / E1），**令牌制** |
| `RenderSystem.*` | M1（C1b） | M2（C5 / C2） |
| `app/*` | M1（C1a 临时桥接） | M2（C5） |
| `pass/*` | M1 | M1（Pass 统一） |
| `mesh_pass.*` | M2（D5，可选） | M1（Pass 统一）或 M2（D5），先到先得，交接一次 |
| `FontManager.*`、`Text.*` | M2 | M2 |
| `Line.*` | — | M1（D1） |
| `PickPass.*` | — | M2（D2） |
| 公共头（namespace/export） | M1 | M1（L2a 冻结） |

**VkDevice 令牌制**：同一时间只有一人改 `src/rhi/vk/*`。顺序：

```text
G1(M1) → B4(M2) → D1(M1) → D2(M2) → E1/E2(M1)
```

交接时在 PR 描述里写明 `VkDevice token: M1 -> M2`，另一方在交接前不改该目录。

**公共头规则**：`RhiTypes.h` / `IRHIDevice.h` / `IRHICommandList.h` 只有 M1 能改。
M2 需要新字段时，发一个"只加字段/方法"的小 PR 给 M1；M1 合入后，各自实现后端
（例如 `setScissor`：M1 加签名 + GL，M2 实现 VK）。

##### 5. M2 在 G1 期间的优先级

1. **A4 资源/IO**（中文路径；只做 `image/model/shader`，`VkDevice` 路径留给 M1）
2. **D4 SDF 文字**
3. **L1 分层脚本**（warn-only，G1 后转 error）
4. 若还有空档：**D3 透明 / D5 显示模式**（不碰 RHI/RenderSystem）
5. **禁止**：`src/rhi/**`、`RenderSystem.*`、`app/main.cpp`、`src/CMakeLists.txt`、公共头

##### 6. 合并与验收

- M2 的 A4/D4/L1 分支从 master 拉，随时可合并；与 G1 文件不重叠。
- G1 合并后打 tag `rhi-c1-gate`；B2/B4/C5 从该 tag 拉分支。
- B2/B4 合并后再开 Pass 统一；C5 可在 B4 之后立即开，也可在阶段 2 空档提前。
- 每周至少一次集成；`master` 始终 GL+VK 冒烟通过。
- 交叉验收：M1 的自测由 M2 在另一台机器跑一遍，反之亦然；
  跨 GPU 不比逐字节，用 `RHI_PROBE` / `RHI_TEXQUAD` / 结构断言。

---

## 5. 回归与验收

### 5.1 构建矩阵

| 平台 | 预设 | 后端 |
|---|---|---|
| Windows | `win-msvc` | GL + Vulkan |
| macOS | `mac-clang` | GL + Vulkan (MoltenVK) |
| Linux | `linux-gcc` | GL + Vulkan |
| 回退 | `*-gl-only` | GL |

### 5.2 冒烟 / 像素回归

```bash
# GL
RHI_SMOKE=1 RHI_TEXQUAD=1 RHI_PROBE=1 RHI_EXIT_AFTER=120 ./my_app

# Vulkan
RHI_BACKEND=vulkan RHI_SMOKE=1 RHI_TEXQUAD=1 RHI_PROBE=1 RHI_EXIT_AFTER=120 ./my_app

# resize
RHI_RESIZE_AT=60 RHI_EXIT_AFTER=120 ./my_app
```

| 断言 | 期望 |
|---|---|
| `RHI_PROBE`（smoke） | BGRA 215,140,122 / RGB 122,140,215（B3c 线性工作流） |
| `RHI_TEXQUAD` 中心 | 精确 180,60,220 |
| VK 验证层 | 0 error |
| 退出 | `RHI_EXIT_AFTER` 后 exit=0，pipeline cache 正常落盘 |
| ImGui | GL/VK 面板均可见，CJK 正常 |

### 5.3 结构检查

```bash
rg -n 'GLFW|glfw' src/
rg -n 'render/|shader\.h' src/rhi
rg -n 'ImGui|imgui' src/
cmake --build build --target check_layers
```

四条都必须为空/通过。

---

## 6. 决策与风险

### 6.1 已定决策

| # | 决策 | 理由 |
|---|---|---|
| D1 | **宿主拥有设备，引擎借用** | 支持 ZW3D 共享设备；销毁顺序由宿主显式控制 |
| D2 | **macOS surface 走 `createSurface` 回调** | 引擎不碰 GLFW/ObjC++；Windows 宿主可留空由 RHI 自建 |
| D3 | **`renderFrame()` 内部完成 present** | 主循环零后端分支；离屏时再暴露 `present()` 可选入口 |
| D4 | **GL 上下文由宿主创建** | RHI 不拥有窗口系统；GLFW 只是示例宿主 |
| D5 | **错误走 `outError`，不抛异常** | 与现有代码风格一致，便于库集成 |
| D6 | **后台选择（`RHI_BACKEND`）上移 app** | 引擎不应读宿主环境 |
| D7 | **命名空间迁移放到 L2 一次性做** | 避免中途两套名字 |

### 6.2 待确认（影响后续排期）

1. ZW3D 的窗口句柄类型（HWND / Qt / MFC）与是否共享设备。
2. 是否要求引擎以 Windows DLL 交付（决定 `ENGINE_API` 与 CRT 约定）。
3. 多视口是否要共享同一个 swapchain（C2 设计）。
4. 是否需要离屏渲染到宿主提供的纹理（B4 的 `externalTexture` 扩展）。

### 6.3 风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| macOS MoltenVK surface 建面失败 | VK 无法启动 | 示例 app 提供 `createSurface`；文档写清 |
| Vulkan 平台宏缺失 | `vulkan.h` 不声明建面函数 | CMake 三平台宏 + 构建验证 |
| GL 的 present/UI 时序变化 | UI 丢失/闪烁 | C1b 用 overlay 回调统一；像素回归 |
| `DeviceDesc` 加字段 | 静态库 ABI 变化 | 本项目静态链接；发布 DLL 前 L2 定 ABI |
| `file(GLOB)` 不发现新文件 | 构建缺源文件 | 每阶段重新 configure；L1 可加 `CONFIGURE_DEPENDS` |
| 现有 `Shader` 编译失败 assert | 新 `GLProgram` 改为返回 false，行为变化 | PR 说明；失败时日志 + 空白而非崩溃 |

---

## 7. 与现有路线图的映射

| 本文 | `ROADMAP_ZW3D.md` | 关系 |
|---|---|---|
| L0 | 已同步（2026-11-24） | RHI 内聚，C1/B 系列前置 |
| C1a + C1b | C1 拆为两步 | 细化 RHI / 门面两个串行子步 |
| C5 | C5 | 原样，补充 `GlfwHost`/`SceneDemo` 拆分 |
| L1 | 已同步（F1 前置） | 分层回归固化 |
| L2 | 已同步（F2 前置） | 公共 API 命名空间/导出/DLL |
| B2 / B4 | 依赖改为 C1a | G1 后与 C1b 可选并行；彼此并行 |
| Pass 统一 | B2/B4 之后，C2/D2 前置 | 新增显式依赖 |
| C2 / D2 / D4 / C4 | 原样 | 不受影响 |
| E1 / E2 / F1 / F2 | 原样 | 不受影响 |

---

## 附录 A：目标公共头清单

```text
engine/                     # 宿主 include 根
  Renderer.h                # RenderSystem 门面（或保留 RenderSystem.h）
  Scene.h
  RenderSettings.h
  DebugPanel.h              # 仅示例 app 使用，可选
rhi/
  RhiTypes.h
  IRHIDevice.h
  IRHICommandList.h
  RHIFactory.h
```

宿主不需要、也不允许 include：

```text
GLFW/、glad/、vulkan/、imgui、(后端内部头 rhi/gl/*、rhi/vk/*)
```

## 附录 B：按阶段的文件改动清单

### L0

```text
新增  src/rhi/gl/GLProgram.h
新增  src/rhi/gl/GLProgram.cpp
新增  src/rhi/BuiltinUniforms.h
修改  src/rhi/gl/GLDevice.h          （GLPipeline 持有 GLProgram）
修改  src/rhi/gl/GLDevice.cpp        （createPipeline）
修改  src/rhi/gl/GLCommandList.cpp   （去 shader.h / DrawUniforms / glm）
修改  src/render/DrawUniforms.h      （别名过渡）
修改  src/render/model/mesh_pass.cpp （类型名）
修改  src/render/RenderSystem.cpp    （类型名）
```

### C1a

```text
修改  src/rhi/RhiTypes.h
修改  src/rhi/IRHIDevice.h
修改  src/rhi/RHIFactory.h
修改  src/rhi/RHIFactory.cpp
修改  src/rhi/gl/GLDevice.h/.cpp
修改  src/rhi/gl/GLCommandList.cpp
修改  src/rhi/vk/VkDevice.h/.cpp
修改  src/CMakeLists.txt
修改  app/main.cpp（填 DeviceDesc）
```

### C1b / C5

```text
修改  src/render/RenderSystem.h/.cpp
修改  app/main.cpp
新增  app/GlfwHost.h/.cpp
新增  app/SceneDemo.h/.cpp
修改  app/CMakeLists.txt
```

### L1

```text
新增  tools/check_layers.cmake
修改  CMakeLists.txt（check_layers target）
```

### L2

```text
修改  公共头（namespace engine / ENGINE_API）
修改  src/CMakeLists.txt（glad PRIVATE 等）
修改  src/render/BaseDraw.h、object/*、pass/IRenderPass.h
新增  src/core/EngineExport.h
```
