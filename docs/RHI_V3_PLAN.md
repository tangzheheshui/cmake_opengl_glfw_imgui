# RHI v3 优化方案（重构执行计划）

> 状态：方案稿（2026-11-23）
> 适用：`src/rhi/**`、`src/render/**` 中经 RHI 的路径、`app/`
> 关联：`docs/RHI_DESIGN.md`（v2 契约）、`docs/ROADMAP_ZW3D.md`（阶段与协作约定）

---

## 0. 决策摘要

1. **不是修 bug，是补一层缺失的抽象**。当前 RHI 只有 8 个类，却承载了 Vulkan 25+ 个对象的职责：
   `VulkanDevice` 1991 行 / 49 个方法，`GLDevice` 578 行 / 25 个方法，`GLCommandList` 366 行。
2. **v3 做一次性破坏性收敛**（tag `rhi-v3-contract`），不用"只加不改"继续叠兼容通道——现在已经叠了五个
   （`setUniform` 字符串 / `setDrawUniforms` blob / `attachSampler` 私有扩展 / `framebufferId` 直通 / `nativeHandle` 无类型）。
3. **顺序按"先解崩溃与泄漏，再解抽象泄漏，最后解规模"**：V3-0 止血 → V3-1 基座 → V3-2 绑定层（核心）→
   V3-3 拆上帝类 → V3-4 资源信息表/上传 → V3-5 编码器/状态 → V3-6 统计与门禁。
4. **业务调用点不跟着改**：提供 `RhiCompat` 一层 inline 转发，把 5 个旧签名保留一个迭代。
   并行工作流（D1/D3/D4/D7/E*）在窗口期内只合 bugfix，不合新功能。

---

## 1. 目标与验收口径

### 1.1 目标（可度量）

| 指标 | 现状 | v3 目标 |
|---|---|---|
| `VulkanDevice` 规模 | 1991 行 / 49 方法 | ≤ 700 行 / ≤ 18 方法（swapchain/queue/frame/descriptor/allocator 移出） |
| 单个 `.cpp` 上限 | 1991 | ≤ 400 行 |
| 每帧描述符更新次数 | 每 draw 1 次 `vkUpdateDescriptorSets` | 0（CPU 侧写 set，帧末批量 flush） |
| 场景规模上限 | 31 个 mesh 触发 `std::abort()`（`maxSets=32`） | ≥ 2000 draw，无 abort、无池耗尽 |
| 设备内存分配次数 | 一资源一次 `vkAllocateMemory` | 页级子分配（≤ 每 1MB 一次） |
| 句柄失效检出 | 无（销毁后 `valid()` 仍为 true） | `alive(handle)` 全类型可用，越界必断言 |
| 业务层后端分支 | 3 处 `if (backend == OpenGL)`（sRGB / 回读行序 / pick 坐标） | 0 |
| 契约变更 | 每次改公共头要同步文档 | v3 之后回到"只加不改"，新能力走新增可选接口 |

### 1.2 回归门禁（复用已有自测开关，不新增工具）

现有 `RHI_*` 开关已覆盖全部关键路径，直接作为门禁：

| 开关 | 覆盖 | GL | VK |
|---|---|---|---|
| `RHI_SMOKE=1` | 冒烟三角形 / 管线 / uniform | ✅ | ✅ |
| `RHI_TEXQUAD=1` | 纹理 / 采样器 / sRGB | ✅ | ✅ |
| `RHI_LINE_TEST=1` | 索引缓冲 / 动态更新 | ✅ | ✅ |
| `RHI_CLIP=1` | 剖切平面 / push 数据布局 | ✅ | ✅ |
| `RHI_VIEWPORTS=2|4` | viewport / scissor 多视口 | ✅ | ✅ |
| `RHI_RT_TEST=1` | 离屏 RT + 回读（**逐字节**） | ✅ | ✅ |
| `RHI_PICK_TEST=1` | ID 渲染 + 1px 回读 | ✅ | ✅ |
| `RHI_RESIZE_AT=<帧>` | swapchain 重建 | — | ✅ |
| `RHI_EXIT_AFTER=<帧>` | 退出/析构泄漏检查 | ✅ | ✅ |
| `RHI_VK_VALIDATION=1` | 校验层（必须 0 error） | — | ✅ |

**判定规则**：每个阶段必须 GL-only 与 VK 双跑全绿；`RHI_RT_TEST` 保持"GL/VK 回读逐字节一致"
（现状已达成，作为回归基线，不许退化）。

---

## 2. 诊断（按严重度）

### 2.1 P0：崩溃 / 泄漏 / 画错

| # | 问题 | 位置 | 影响 |
|---|---|---|---|
| 1 | 描述符池 `maxSets=32` + 每 mesh 一块 uniform block | `VkDevice.cpp:1808`、`mesh_pass.cpp:299` | 第 31 个 mesh `vkAllocateDescriptorSets` 失败 → `VK_CHECK` → `abort()`（`VkCommon.h:16`） |
| 2 | 句柄无类型 / 无 generation，`nativeHandle(RhiHandle)` 先按 buffer 解析 | `GLDevice.cpp:547-556`、`GLCommandList.cpp:183`（注释已自认踩坑） | 纹理被当 buffer；销毁后句柄仍 valid；`buffer()/texture()/pipeline()` 无边界检查 |
| 3 | `bindTexture` 每 draw `vkUpdateDescriptorSets` 写"当前块" | `VkCommandList.cpp:213-228` | 热路径每 draw 一次驱动调用；被绑定的 set 可能仍在飞（VUID-03047）；槽位硬编码 4 / GL 32 |
| 4 | `~GLDevice` 不释放 FBO/RBO；VAO cache 永不失效 | `GLDevice.cpp:85-91`、`GLCommandList.cpp:207-260` | FBO 泄漏；销毁 buffer 后 VAO 仍持属性指针 → 画出垃圾 |
| 5 | `createTexture` 只收 RGBA8 紧排 + CPU 翻转，不设 `GL_UNPACK_ALIGNMENT` | `GLDevice.cpp:223-285`、`mesh_pass.cpp:390-430` | R8 / 非 4 对齐纹理错位；每贴上 CPU 转格式 |
| 6 | `beginFrame()` 返回 void，swapchain 失效在内部吞 | `IRHIDevice.h:70` | 上层录制一个不会提交的帧，状态机失配 |

### 2.2 P1：抽象泄漏（业务层已出现后端分支）

| # | 证据 | 说明 |
|---|---|---|
| 1 | 三套 uniform 机制（字符串 / blob / push 固定偏移，`VkCommandList.cpp:158-200`） | 字符串 `strcmp` 映射硬偏移；shader 改名静默丢数据；无创建期校验 |
| 2 | `setDrawUniforms` 切换"当前块"，`bindTexture` 写"当前块" → 业务建 `m_quadUB` 隔离 | `RenderSystem.cpp:745` 是抽象失败的直接证据；同类型 `mesh_pass.cpp:443` 重复 set |
| 3 | 采样器三套（`TextureDesc::sampler` / `createSampler` / GL 私有 `attachSampler`） | 契约外能力；两后端语义不一致（cube wrap、mip 档、compare） |
| 4 | 业务判后端 | `RenderSystem.cpp:398`（sRGB 开关）、`450`/`657`（回读行序）、pick 坐标 |
| 5 | `RenderPassDesc` 混装附件 + clear + viewport + `framebufferId` 直通 | `RhiTypes.h:247-260`；GL 侧靠 `saveState/restoreState` 全量快照救场（`GLCommandList.cpp:35-64`），违背"显式烘焙"原则 |
| 6 | GLFW 泄漏进抽象 | `IRHIDevice.h:13` 前向声明；`GLDevice::window()`；`GLCommandList.cpp:102` 直接 `glfwGetFramebufferSize`；`DeviceDesc` 契约入口 `return nullptr` |
| 7 | 错误不可诊断 | 静默 `return {}` / `return false`；VK 侧 `abort()` |
| 8 | 能力缺口 | 无 MRT、`IndexFormat` 硬编码 UINT32、`GraphicsPipelineDesc` 无附件格式（VK dynamic rendering 必需）、无并行 command list、readback 全同步 |

### 2.3 P2：规模与工程

| # | 问题 | 影响 |
|---|---|---|
| 1 | 一资源一次 `vkAllocateMemory`（含 256B UBO） | 大装配必踩 `maxMemoryAllocationCount`（常见 4096） |
| 2 | 无 `RhiStats` | CAD 性能回归无观测量（drawcall / PSO 切换 / 描述符更新 / 上传字节） |
| 3 | 无 headless smoke / CI | 路线图 F1 已列，未落地；多机验证靠手工 |
| 4 | 格式知识三份平行 switch（`formatBytesPerPixel` / `glFormatOf` / VK 映射） | 加格式漏一处即静默错 |

---

## 3. 目标架构

### 3.1 分层与依赖方向

```
app/                    宿主：窗口/事件/ImGui
  └── render/           引擎上层：Pass、Scene、Material、Mesh（可先留旧实现）
        └── rhi/        仅此层可见后端差异
              ├── RhiTypes / FormatInfo / Handle / Result     ← 无依赖
              ├── IRHIDevice / ISwapchain / IFrame / IQueue
              ├── ICommandList / RenderPassEncoder
              ├── ResourceInfo（TextureInfo/BufferInfo/...）+ Handle<Tag>（全部资源走句柄）
              ├── BindingLayout / DescriptorSetHandle / detail::{DescriptorRing, UniformRing}
              └── gl/ vk/     后端实现（只被 RHIFactory 引用；禁止被上层直接 include）
```

规则（写进 review checklist）：

- **R1**：`rhi/` 之外不得出现 `<glad/glad.h>`、`<vulkan/vulkan.h>`、`GLFWwindow`。
- **R2**：后端对象只由 `RHIFactory` 构造；上层只拿接口指针或值类型包装。
- **R3**：任何"加一个格式/一个 uniform/一个资源类型"的改动，触及文件 ≤ 2。
- **R4**：一个类一份可变状态；单个 `.cpp` ≤ 400 行。

### 3.2 类清单：vtable 只存在于 4 个对象

**契约层导出的多态对象只有 4 个**（每进程 / 每帧一次，不是每资源一次）：

| 类 | 必须有 vtable 的理由 |
|---|---|
| `IDevice` | 后端实现不同；每进程 1 个 |
| `ICommandList` | 每帧（或每录制线程）1 个 |
| `ISwapchain` / `IFrame` | 每帧 1 个 |
| `IAdapter` | 启动时 1 次 |

**资源层一律句柄**（POD `{id, gen}`），"资源知识"放在设备侧的只读信息表里，不导出资源类：

| 分类 | 形态 | 说明 |
|---|---|---|
| Buffer / Texture / TextureView / Sampler / ShaderModule / Pipeline / RenderTarget / Fence | `Handle<Tag>` + `Device::xxxInfo(h)` | 信息表是 POD（`TextureInfo{Extent3D size; Format fmt; uint32 mipLevels; uint32 arrayLayers; uint32 bytesPerPixel; bool cube;}`），按句柄索引，创建期算好，热路径零查询 |
| DescriptorSet / DescriptorSetLayout | `Handle<Tag>` + `cmd.bindSet(h, binding, dynamicOffsets[])` | 描述符同样是资源，同样句柄化；不开 `IDescriptorSet::setTexture()` 这类对象方法 |
| UniformRing / FrameDescriptorRing / MemoryAllocator / DeletionQueue | 设备内部**值对象**（不导出） | 纯 CPU 数据结构，无 vtable，不进契约 |
| BindingLayout / VertexLayout / RasterState / DepthState / BlendState / AttachmentLayout | 纯 `struct` Desc | 创建期入参，无行为 |
| `FormatInfo` | 静态表（无状态） | 唯一格式真相源 |

**禁止事项（review 必查）**：契约层出现 `Texture`/`Buffer` 这类带方法的资源类；出现 `tex.bind()` / `tex.width()`；
出现 `shared_ptr<Ixxx>` 形式的资源所有权。旧世界留存的 `render/shader.h`、`TextureMng` 是被迁移 away 的那一侧，
保持原样、不再新增同类用法。

### 3.3 关键接口（骨架）

```cpp
// rhi/Handle.h —— 保留字段名 id，业务侧 h.id 全部零改动
template <class Tag> struct Handle {
    uint32_t id  = 0;
    uint32_t gen = 0;               // 槽位代际，销毁/复用时 +1
    bool valid() const { return id != 0; }
};
using TextureHandle = Handle<struct TextureTag>;   // 其余同理

// rhi/Result.h
enum class RhiResult { Ok, InvalidArg, Unsupported, OutOfMemory, DeviceLost };
template <class T = void> struct Result {
    RhiResult code = RhiResult::Ok;
    T value{};                       // void 特化省略
    explicit operator bool() const { return code == RhiResult::Ok; }
};

// rhi/FormatInfo.h —— 唯一真相源，替换三份 switch
class FormatInfo {
public:
    static const FormatInfo& of(Format);          // assert 覆盖全部枚举
    uint32_t bytesPerPixel() const;
    bool     isDepth()  const;
    bool     isSRGB()   const;
    bool     isInteger()const;
    uint32_t glInternal() const;                  // GL 后端用
    uint32_t glDataFormat() const;
    uint32_t vkFormat() const;                    // 仅在包含 vulkan.h 的 TU 编译
};
```

```cpp
// rhi/IFrame.h
struct FrameInfo {
    bool     valid = false;          // false = 本帧不画（最小化 / swapchain 重建）
    uint32_t width = 0, height = 0;
    uint32_t frameIndex = 0;         // per-frame ring 槽位
    TextureHandle backbuffer;        // 默认帧缓冲当纹理，语义与 RT 统一
};
class IFrame {
public:
    virtual const FrameInfo& info() const = 0;
    virtual ICommandList&    cmd()  = 0;
    // 异步回读票据（拾取/缩略图不再阻塞管线）
    virtual ReadbackTicket   readback(TextureHandle, const ReadbackDesc&) = 0;
};
```

```cpp
// rhi/BindingLayout.h —— 描述符也是资源：句柄 + 录制期命令，不做对象方法
using DescriptorSetLayoutHandle = Handle<struct DslTag>;
using DescriptorSetHandle       = Handle<struct DsTag>;

// 设备侧分配（每帧 ring 切分，帧末整体回收；句柄不外借生命周期）
virtual Result<DescriptorSetHandle> allocDescriptorSet(DescriptorSetLayoutHandle,
                                                       const BindingLayout&) = 0;
// 写数据是 CPU 侧动作，不改已绑定的 GPU set（避免 update-in-use）
virtual void writeDescriptor(DescriptorSetHandle,
                             uint32_t binding, TextureHandle, SamplerHandle,
                             uint32_t arrayIndex = 0) = 0;
virtual void writeDescriptor(DescriptorSetHandle, uint32_t binding,
                             UniformRingAlloc) = 0;

// 录制：唯一的绑定入口（与 vkCmdBindDescriptorSets(..., dynamicOffset) 一一对应）
cmd->bindSet(DescriptorSetHandle, uint32_t firstBinding,
             const uint32_t* dynamicOffsets, uint32_t count);

// detail::UniformRing —— 设备内部值对象，不导出、不进契约
class UniformRing {
public:
    struct Alloc { uint64_t offset; uint64_t size; void* mapped; };
    Alloc allocate(uint32_t size, uint32_t align);   // 取代 createUniformBlock(256)
    void  reset();
};
```

```cpp
// rhi/ResourceInfo.h —— 句柄 + 只读信息表（替代"资源类"）
struct TextureInfo {
    Extent3D size{};
    Format   format       = Format::RGBA8_UNorm;
    uint32_t mipLevels    = 1;
    uint32_t arrayLayers  = 1;
    uint32_t bytesPerPixel= 4;      // 创建期算好，读回/上传热路径直接用
    bool     cube         = false;
    bool     sampleable   = true;
};
struct PipelineInfo {               // 取代 IPipeline 的查询方法
    BindingLayout   bindings;
    AttachmentLayout target;
    uint32_t        vertexStride = 0, instanceStride = 0;
    ShaderReflection reflection;    // attr/uniform 名 + location/offset
};

// 设备访问：越界 / 死句柄 -> 断言（不是返回空对象假装成功）
virtual const TextureInfo*  textureInfo (TextureHandle)  const = 0;
virtual const BufferInfo*   bufferInfo  (BufferHandle)   const = 0;
virtual const PipelineInfo* pipelineInfo(PipelineHandle) const = 0;

// 原生句柄按类型拆（删掉 nativeHandle(RhiHandle) 的跨类型歧义）
virtual void* nativeTexture(TextureHandle) const = 0;    // GLuint / VkImage
virtual void* nativeTextureView(TextureHandle) const = 0;// VkImageView
virtual void* nativeBuffer(BufferHandle) const = 0;
```

```cpp
// 业务侧：句柄 + 本地 record，metadata 不进热路径（MeshPass / Material / TextureMng）
struct TextureRecord {
    TextureHandle handle;
    TextureInfo   info;        // 拷贝一份；draw 时不回查设备、不虚调用
    SamplerHandle sampler;
};
// std::map<std::string /*path*/, TextureRecord>  —— 可拷贝、可序列化、无原子计数
```

`Sampler` 不再有 `attachSampler` 的对象方法，改为设备函数：
`device->attachSampler(TextureHandle, SamplerHandle)`（GL 记录在纹理槽上、VK 重建 view，语义由后端负责）。

**pipeline 创建期校验**（替代"运行时字符串 strcmp + WARN"）：`createPipeline` 时比对 `ShaderReflection`
与 `BindingLayout` / `VertexLayout`，shader 声明的 uniform/attribute 未被覆盖即返回 `InvalidArg`。

### 3.3.1 决策记录（ADR）：契约层为什么不导出资源类

> 结论：**资源一律句柄，不导出具方法/虚表的资源类**。理由与三个前提（缺一不可）。

**理由**

1. **GPU 资源生命周期 ≠ C++ 对象生命周期**。帧还在飞时不能立刻销毁；析构函数无法表达"延迟到第 N 帧"。
   把这件事藏进析构 / `shared_ptr` 的引用计数里，等于假装解决。
2. **句柄是 POD**：能按值进 `Material`、`RenderPassDesc`、`map<path, handle>`，可序列化、可批量 dump、
   无堆分配、无原子引用计数、无虚调用。
3. **命令录制语义不允许对象方法**："绑纹理"是命令（`cmd->bindTexture(h, slot)`），不是对象行为。
   `tex->bind()` 会把 GL 立即状态机偷运回契约层，正是迁移要消灭的东西。
4. **双端同构**：GL 是 `GLuint`，VK 是裸指针；句柄表 `id = 下标 + 1` 两端完全同构。虚接口则每端都要包一层实现类。
5. **契约可冻结可演进**：给 `Desc` 加字段是加法；给虚基类加虚函数改 vtable 是破坏性变更。
6. 行业参照：bgfx（本项目参照对象）、Vulkan（`VkImage`/`VkBuffer` 为非 dispatchable 句柄）。
   **注意 D3D12 / Metal 不是纯句柄**：D3D12 资源是 COM 对象（`ID3D12Resource*`，AddRef/Release），
   Metal 资源是协议对象（objc 引用计数），只有 descriptor 是 CPU 句柄。所以"句柄制"是本项目
   **对齐 Vulkan + 显式生命周期**的选择，不是全行业共识；选它就必须同时兑现下面三个前提。

**三个前提（v3 必须补齐，否则句柄制不成立）**

| 前提 | v3 落地 |
|---|---|
| 死槽位可检出 | `Handle{id, gen}` + `alive(h)` + 信息表越界断言 |
| 帧在飞时延迟回收 | `DeletionQueue`：`destroy*` 只摘句柄 + 登记 `frameIndex + kFramesInFlight`，到期批量真销毁（GL/VK 同一语义） |
| 所有权唯一 | 纹理归设备句柄表、RT 只持 `TextureHandle`、sampler 只被纹理引用（双向引用改单向） |

**边界：查询 vs 命令（最容易走样的地方）**

`tex->width()` 这类**查询**在 D3D12/Metal 里是允许的（`GetDesc()` / 属性读取）；被禁止的是
`tex->bind()` 这类**命令**。本项目的替代方案是"设备只读信息表 `textureInfo(h)` + 业务侧
`TextureRecord` 缓存"：查询可以存在（且不进热路径），但不得演变成"对象方法执行状态变更"。

**现状校正（勿误读）**：本仓库当前**并未满足**上述三个前提——`VulkanDevice::destroyBuffer/destroyTexture`
是立即 `vkDestroy*`（`deferGpuWork` 只用于上传）、句柄无 gen 校验且越界无断言、`nativeHandle(RhiHandle)`
跨类型解析会误判纹理。ADR 中"由设备统一管理死槽位、访问死句柄不崩"是**目标**而非事实，
实现落在 V3-0（`DeletionQueue` + `Handle{id,gen}` + 越界断言）。

**不适用场景（保留对象模型的边界）**：`IDevice`/`ICommandList`/`ISwapchain`/`IFrame`/`IAdapter` 这类
"每进程或每帧一个、需要后端分派"的对象继续用虚接口；`UniformRing`/`DeletionQueue` 这类纯 CPU 结构用值对象。
除此之外新增的任何"资源类"都需要先更新本 ADR。

---

## 4. 阶段方案

每阶段都按 **范围 / 新增文件 / 改动文件 / 验收 / 回滚 / 规模** 给。

### V3-0 止血（无接口变更）

- **范围**：P0 六项 + 池上限短解
  1. `~GLDevice` 释放 `_renderTargets` 的 fbo/rbo（`GLDevice.cpp:85-91`）
  2. `destroyBuffer/destroyPipeline` 时清理引用它的 VAO cache 条目
  3. `createTexture` 前 `glPixelStorei(GL_UNPACK_ALIGNMENT, 1)`；`readbackTexture` 结束恢复
  4. VK：`maxSets` 32→1024 + `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT`（短解，V3-2 会替换）
  5. `VK_CHECK` 在资源分配路径改为返回 `RhiResult`（其余路径暂留 abort 并加 `objName` 上下文）
  6. `beginFrame()` 改返回 `bool`（窗口最小化/重建时 `false`，`RenderSystem` 跳过录制）
  7. **`DeletionQueue`（句柄制前提）**：`destroy*` 不立即 `vkDestroy*`/`glDelete*`，改为摘句柄 + 登记
     `frameIndex + kFramesInFlight(2)`，`endFrame` 到期批量真销毁。GL 侧同样走队列（避免删掉仍被 VAO 引用的 buffer）
- **新增文件**：`rhi/detail/DeletionQueue.h`（GL/VK 共用值对象）
- **改动文件**：`gl/GLDevice.cpp`、`gl/GLCommandList.cpp`、`vk/VkDevice.cpp`（限制在 3 处 + 常量）、
  `rhi/IRHIDevice.h`、`render/RenderSystem.cpp`（仅 beginFrame 调用点）
- **验收**：双跑 `RHI_SMOKE` / `RHI_TEXQUAD` / `RHI_LINE_TEST` / `RHI_RT_TEST` / `RHI_PICK_TEST` /
  `RHI_VIEWPORTS=4`；VK 侧 `RHI_VK_VALIDATION=1` 零 error；构造 64 mesh 场景（脚本复制 duck）不再 abort；
  `RHI_EXIT_AFTER=120` 退出时验证层无泄漏
- **回滚**：逐项独立提交，单项 revert 即可
- **规模**：S（0.5–1 天）

### V3-1 基座：格式 / 句柄 / Result

- **范围**
  - `FormatInfo` 静态表替换 `formatBytesPerPixel`（`RhiTypes.h:110`）、`glFormatOf`（`GLDevice.cpp:18`）、VK 映射
  - `Handle<Tag>{id, gen}`（**字段名保留 `id`**，业务侧 `h.id` 零改动）+ `detail/HandleTable<T>`（含 `alive()`）
  - `Result<T>` 用于 `create*`；`IRHIDevice` 增加 `alive(...)` 四类重载
  - 把 `nativeHandle(RhiHandle)` 拆成 `nativeBuffer/nativeTexture/nativeSampler/nativePipeline`（`GLDevice.cpp:547` 歧义消除）
  - `GraphicsPipelineDesc` 增加 `AttachmentLayout`（colorFormats[]/depthFormat/samples）、`IndexFormat`、
    `RasterState`（cull/frontFace/polygonMode/lineWidth/depthBias）
- **新增文件**：`rhi/FormatInfo.h/.cpp`、`rhi/Handle.h`、`rhi/Result.h`、`rhi/detail/HandleTable.h`
- **改动文件**：`rhi/RhiTypes.h`、`rhi/IRHIDevice.h`、`gl/GLDevice.*`、`vk/VkDevice.*`、`vk/VkCaps.*`、
  `gl/GLCommandList.cpp`（格式转换调用点）、`mesh_pass.cpp`（IndexFormat 一处）
- **验收**：全门禁双跑；新增断言：非法句柄访问必触发 `RHI_ASSERT`（把现有静默 return 改断言，跑一遍门禁确认不误报）；
  VK 管线创建时若 `AttachmentLayout` 与 `pickRT`（`BGRA8_SRGB`）不匹配必须报错（现为隐含约定，`RenderSystem.cpp:524`）
- **回滚**：`Handle` 加 `gen` 是纯增量，可单独回退；`AttachmentLayout` 为新增字段带默认值
- **规模**：M（1.5–2 天）

### V3-2 绑定层（核心，消除 32-set 与"当前块"）

- **范围**
  - `BindingLayout`（desc）+ `DescriptorSetLayoutHandle` + `DescriptorSetHandle` + 设备内部 `FrameDescriptorRing` / `UniformRing`
  - VK：一帧一个 1MB UBO（`VK_MEMORY_PROPERTY_HOST_VISIBLE`）+ 一帧一个 pool（`reset` 复用）+ cached set；
    `vkCmdBindDescriptorSets(..., dynamicOffset)`，`bindTexture` 只写 CPU 侧 set，首绑批量 flush
  - 接口形态是 `cmd->bindSet(DescriptorSetHandle, uint32_t firstBinding, const uint32_t* dynamicOffsets, uint32_t count)`，
    **不开 `setTexture()` 之类的对象方法**（描述符仍是资源，遵循同一句柄规则）
  - `setDrawUniforms(block, data, size)` 变适配器：`UniformRing::allocate(size)` → dynamic offset（`block` 参数忽略并告警一次）
  - 删除 GL 私有 `attachSampler` 的"契约外"身份：转为设备函数 `attachSampler(TextureHandle, SamplerHandle)`，GL/VK 同语义
  - 纹理槽上限改为 `kMaxTextureBindings = 16` 常量 + `DeviceCapabilities::maxSampledImagesPerSet` clamp
- **新增文件**：`rhi/BindingLayout.h`（含 `DescriptorSetHandle`/`DescriptorSetLayoutHandle` 的 Tag）、
  `rhi/detail/DescriptorRing.h/.cpp`、`rhi/detail/UniformRing.h/.cpp`
- **改动文件**：`vk/VkCommandList.cpp`（bindTexture/setDrawUniforms/applyPushConstants）、`vk/VkDevice.cpp`（pool/layout 创建）、
  `gl/GLCommandList.cpp`（只加槽位常量与断言）、`render/RenderSystem.cpp`（删 `m_quadUB` 补丁）、`mesh_pass.cpp`（删每 mesh block）
- **验收**：
  - 256 mesh 场景 VK 无 abort、无池耗尽；`RHI_VK_VALIDATION=1` 零 error（重点看 descriptor 相关 VUID）
  - `RHI_RT_TEST` 保持 GL/VK 逐字节一致；`RHI_TEXQUAD` 与 `RHI_CLIP` 视觉不变（用 `RHI_DUMP` 出 PPM 比对）
  - 帧内 `vkUpdateDescriptorSets` 计数 = 0（用 `RhiStats` 临时打点，V3-6 转正）
  - `RenderSystem` 中不再存在"为避免污染而单独建 block"的代码
- **回滚**：保留 `RHI_LEGACY_UNIFORM_BLOCK=1` 环境变量切回旧路径一个迭代，验证后删除
- **规模**：L（3–5 天）——这是整个方案的技术核心

### V3-3 拆上帝类：Queue / Swapchain / Frame / Allocator

- **范围**
  - `IQueue::submit/present/wait`，`ISwapchain::acquire/present/resize`，`IFrame`
  - `detail::MemoryAllocator`：页级（1MB）子分配 + 线性回收，所有 buffer/texture（含 UBO）改走它
  - `VulkanDevice` 只留 adapter/device/资源表 + `create*`；swapchain/queue/frame 移出为独立类（friend 访问内部句柄）
  - `beginFrame() → IFrame*`（`nullptr` = 本帧跳过）；`endFrame(IFrame*)`
  - `VkDevice.cpp` 里 4 个 barrier 方法迁出到 `IStateTracker`（`VkStateTracker`）
- **新增文件**：`rhi/IQueue.h`、`rhi/ISwapchain.h`、`rhi/IFrame.h`、`vk/VkQueue.*`、`vk/VkSwapchain.*`、
  `vk/VkFrame.*`、`vk/VkStateTracker.*`、`rhi/detail/MemoryAllocator.h/.cpp`（GL 侧给 no-op 实现）
- **改动文件**：`vk/VkDevice.h/.cpp`（本轮需独占窗口）、`gl/GLDevice.*`（`GLFrame` 空实现 + `beginFrame` 语义）、
  `render/RenderSystem.cpp`、`app/main.cpp`（swap 交给 `ISwapchain::present`）
- **验收**：`RHI_RESIZE_AT` 连续 resize + 最小化恢复；`RHI_VK_VALIDATION=1` 零 error；`RHI_EXIT_AFTER` 无泄漏；
  `VkDevice.cpp` ≤ 700 行（用 `wc -l` 作为客观门禁）；GL 侧 `beginFrame` 不再依赖 `glfwGetFramebufferSize`
- **回滚**：`RHIFactory` 保留旧构造重载一个迭代
- **规模**：L（5–8 天）

### V3-4 资源信息表 + 上传路径（句柄不变）

- **范围**
  - 新增 `TextureInfo/BufferInfo/SamplerInfo/PipelineInfo` 只读信息表 + `textureInfo()/bufferInfo()/pipelineInfo()`
    访问器（越界即断言）；`nativeTexture()/nativeBuffer()` 按类型拆分
  - `TextureViewHandle`（mip/layer/aspect 子资源视图；VK 必需，GL 退化为原纹理）
  - `TextureDesc` 增 `rowPitch`、`flipV`；上传接受 R8/RGBA8/BGRA8，CPU 不再逐像素转格式
  - `MeshPass` 的 CPU 翻转 + RGBA8 转换循环删除（`mesh_pass.cpp:390-430`）
  - `PipelineInfo::reflection` 承载 shader 反射（attr/uniform 名 + location/offset），创建期校验
  - `RhiCompat.h`：旧 `createTexture/createBuffer/createPipeline/bindTexture/setDrawUniforms` 签名 inline 转发
- **新增文件**：`rhi/ResourceInfo.h`、`rhi/RhiCompat.h`、`rhi/detail/UploadContext.h/.cpp`、
  `gl/GLResourceTable.h/.cpp`、`vk/VkResourceTable.h/.cpp`
  （设备内部仍是表 + POD 槽位结构，不新增任何虚资源类）
- **改动文件**：`mesh_pass.cpp`、`object/Sky.cpp`、`RenderSystem.cpp`（`m_quadTex` 等）、`PassTexture.*`/`PassColor.*`、
  `rhi/RhiTypes.h`（Descriptor 增字段）
- **验收**：全门禁；新增 `RHI_TEX_R8=1` 用例（R8 字体图集宽度非 4 倍数上传正确）；
  `RHI_RT_TEST` 逐字节一致；`RHI_TEXQUAD` 视觉不变；`RenderSystem`/`MeshPass` 中不再出现 `_device->texture(h).id`
- **回滚**：信息表是纯增量（旧访问器保留一个迭代）；上传路径可按 `RHI_UPLOAD_LEGACY=1` 回切
- **规模**：M–L（3–5 天）

### V3-5 编码器 / 多线程录制（可延后到 C1 之后）

- **范围**：`RenderPassEncoder`（pass 状态子集）、`ICommandPool`、`ICommandList` 细分 Graphics/Copy、
  `submit(cmd*, count)`；`setViewport/setScissor` 从 `RenderPassDesc` 拆出
- **新增文件**：`rhi/RenderPassEncoder.h`、`rhi/ICommandPool.h`、`vk/VkCommandPool.*`
- **改动文件**：`VkCommandList.*`（拆编码器）、`RenderSystem.cpp`（多视口循环改用 encoder）
- **验收**：`RHI_VIEWPORTS=4` 视觉不变；可选 `RHI_MT_RECORD=1`（双线程录制）无校验错误
- **规模**：M（3–5 天）

### V3-6 统计与工程化

- **范围**：`RhiStats`（drawcall / PSO 切换 / descriptor flush / upload 字节 / barrier 数），调试面板展示；
  `tools/rhi_smoke`（路线图 4.4 已列）+ `tools/rhi_gate.sh`（见附录）；CI 跑 GL-only 与 VK（headless）
- **验收**：`tools/rhi_gate.sh` 在两台机器 exit 0；面板能看到 v3 计数
- **规模**：S–M（1–2 天）

---

## 5. 契约与协作规则

### 5.1 v3 契约窗口（与现有"B0 只加不改"规则共存）

1. 建 tag `rhi-v3-contract`，在 `docs/RHI_DESIGN.md` 新增「v3 契约」章节：v2→v3 变更表（新增/删除/语义变更）。
2. 窗口期 **1 周（连续 5 个工作日）**：`master` 只合 bugfix；D1/D3/D4/D7/E* 不推新功能；冲突由 V3 负责人解。
3. 窗口结束后恢复"只加不改"，且**新增能力必须走可选接口**（默认不支持的虚方法只允许出现在 `detail/`，不允许出现在 `ResourceInfo` 这类资源契约）。
4. `RhiCompat.h`：把 5 个旧签名（`createTexture`/`createBuffer`/`createPipeline`/`bindTexture`/`setDrawUniforms`）
   做 inline 转发，保留**一个迭代**（约 2 周），之后删除。

### 5.2 文件所有权（窗口期分配，避免两台机器撞车）

| 阶段 | 独占文件 | 其他工作流可碰 |
|---|---|---|
| V3-0/V3-1 | `gl/GLDevice.*`、`vk/VkDevice.*`（≤3 处/天）、`rhi/RhiTypes.h` | 其他全部 |
| V3-2 | `vk/VkCommandList.*`、`vk/VkDevice.*`（descriptor 部分） | `gl/**`、`render/**` |
| V3-3 | `vk/VkDevice.*`（整文件）、`vk/VkSwapchain.*`、`vk/VkQueue.*` | `render/**`（仅 `RenderSystem` 由 V3 负责） |
| V3-4 | `vk/VkResourceTable.*`、`gl/GLResourceTable.*`、`mesh_pass.cpp` | `render/**` 其余 |
| V3-5 | `vk/VkCommandPool.*`、`rhi/RenderPassEncoder.h` | `gl/**` |

新代码一律落新文件（`/docs/ROADMAP_ZW3D.md` 4.2 已定），把 `VkDevice.cpp` 的"减行"作为客观验收指标。

---

## 6. 排期

| 阶段 | 单人 | 双人并行 | 依赖 |
|---|---|---|---|
| V3-0 止血 | 1 天 | — | 无 |
| V3-1 基座 | 2 天 | 1 天（格式 + 句柄/Result 分开） | V3-0 |
| V3-2 绑定层 | 5 天 | 3 天（VK ring / GL 适配并行） | V3-1 |
| V3-3 拆上帝类 | 8 天 | 5 天（VK 主拆 + GL/Frame 并行） | V3-2 |
| V3-4 资源信息表 | 4 天 | 3 天（GL/VK 表并行） | V3-3（部分可并行） |
| V3-5 编码器 | 4 天 | 3 天 | V3-4 |
| V3-6 统计/CI | 2 天 | 1 天 | V3-3 起即可并行 |
| **合计** | **约 26 人日（5.5 周）** | **约 3.5 周** | |

可提前收手的节点：**V3-2 完成**即可解决 32-set 崩溃与描述符抽象泄漏（业务层最痛的两点），
此时可暂停 V3-3~V3-5 先行 ZW3D 接入（C1/C2），代价是 `VkDevice.cpp` 仍然偏大。

---

## 7. 风险与不做

| 风险 | 影响 | 应对 |
|---|---|---|
| 描述符模型改动导致 VK 画面回归 | 高 | `RHI_LEGACY_UNIFORM_BLOCK` 开关回切；`RHI_RT_TEST` 逐字节 + `RHI_DUMP` PPM 比对 |
| `VkDevice.cpp` 独占窗口阻塞并行 | 中 | 窗口只 1 周；其他流改 shader/资源目录（`shaders_vk/*line*`、`*font*`，路线图已分配） |
| 句柄失效与延迟回收引入的生命周期风险（device 先死 / 句柄悬空） | 中 | 资源由设备 `HandleTable` 独占；`alive(h)` 校验 + `DeletionQueue` 延迟 N 帧；不引入 `shared_ptr` |
| GL"模拟器"层复杂度上升（descriptor 模型在 GL 上是 no-op） | 中 | GL 端 `bindSet` 只展开成 `glBindTexture/glBindSampler`，不做假 pool；`DescriptorRing` 在 GL 上退化为空操作 |
| Reversed-Z / viewport 负高度等既有约定被顺手改掉 | 高 | v3 明令不动坐标/深度约定；改动只限绑定与生命周期，`RHI_RT_TEST`/`RHI_CLIP` 守住 |

**明确不做（沿用路线图第 5 节）**：bindless/BDA、GPL、VRS/FSR、Ray Query、HDR、完整 RT 管线、
Vulkan Video / Mesh Shader / Shader Object。v3 不引入 RenderGraph 与 Material 系统（留 Phase 3/4）。

---

## 8. 立即可执行的第一步（V3-0 checklist）

1. `git checkout -b feat/rhi-v3-0-hotfix`
2. `GLDevice::~GLDevice` 补 `_renderTargets` 释放（fbo / depthRbo），并加 `RHI_EXIT_AFTER=120` 下的泄漏自检日志
3. `destroyBuffer` / `destroyPipeline` 后清理 `GLCommandList::_vaoCache` 中含该句柄的条目
4. `createTexture` 入口 `glPixelStorei(GL_UNPACK_ALIGNMENT, 1)`，`readbackTexture` 出口恢复为 4
5. VK 描述符池 `maxSets` 32 → 1024，加 `FREE_DESCRIPTOR_SET_BIT`；`createUniformBlock` 分配失败返回 `{}` 而非 abort
6. `beginFrame()` 返回 `bool`；`RenderSystem::drawRhiScene` 在 `false` 时直接 return
7. 落 `detail/DeletionQueue.h`：`destroy*` 只摘句柄 + 登记，`endFrame` 到期批量销毁（GL/VK 共用同一实现）
8. 跑门禁：
   ```bash
   cmake --preset mac-gl-only && cmake --build --preset mac-gl-only
   RHI_BACKEND=gl RHI_SMOKE=1 RHI_TEXQUAD=1 RHI_RT_TEST=1 RHI_PICK_TEST=1 RHI_EXIT_AFTER=120 ./build-gl/Debug/my_app
   cmake --preset mac-clang && cmake --build --preset mac-clang
   RHI_VK_VALIDATION=1 RHI_BACKEND=vk RHI_SMOKE=1 RHI_TEXQUAD=1 RHI_RT_TEST=1 RHI_PICK_TEST=1 RHI_VIEWPORTS=4 RHI_EXIT_AFTER=120 ./build/Debug/my_app
   ```
9. 64 mesh 规模验证（临时脚本复制 `duck` 节点，或 `RHI_SCENE_SCALE=64` 一次性开关），确认不再 abort
10. 连续创建/销毁 1000 次纹理（`RHI_CHURN=1000`），验证层 0 泄漏
11. 提交信息：`fix(B0): GL FBO 泄漏 / VAO 悬空 / VK 描述符池上限 / beginFrame 失败语义`

---

## 附录 A：门禁脚本（可直接落 `tools/rhi_gate.sh`）

```bash
#!/usr/bin/env bash
set -euo pipefail

APP_GL="${APP_GL:-build-gl/Debug/my_app}"
APP_VK="${APP_VK:-build/Debug/my_app}"

run() {   # run <bin> <label> <env...>
  local bin="$1" label="$2"; shift 2
  echo "== $label =="
  env "$@" "$bin" >/dev/null 2>&1 || { echo "FAIL: $label"; exit 1; }
}

if [[ -x "$APP_GL" ]]; then
  run "$APP_GL" "GL smoke"     RHI_BACKEND=gl RHI_SMOKE=1    RHI_EXIT_AFTER=120
  run "$APP_GL" "GL texquad"   RHI_BACKEND=gl RHI_TEXQUAD=1  RHI_EXIT_AFTER=120
  run "$APP_GL" "GL rt-readback" RHI_BACKEND=gl RHI_RT_TEST=1 RHI_EXIT_AFTER=120
  run "$APP_GL" "GL pick"      RHI_BACKEND=gl RHI_PICK_TEST=1 RHI_EXIT_AFTER=120
  run "$APP_GL" "GL line"      RHI_BACKEND=gl RHI_LINE_TEST=1 RHI_EXIT_AFTER=120
fi

if [[ -x "$APP_VK" ]]; then
  run "$APP_VK" "VK smoke+validation" RHI_BACKEND=vk RHI_VK_VALIDATION=1 RHI_SMOKE=1 RHI_EXIT_AFTER=120
  run "$APP_VK" "VK texquad"          RHI_BACKEND=vk RHI_TEXQUAD=1 RHI_EXIT_AFTER=120
  run "$APP_VK" "VK rt-readback"      RHI_BACKEND=vk RHI_RT_TEST=1 RHI_EXIT_AFTER=120
  run "$APP_VK" "VK pick"             RHI_BACKEND=vk RHI_PICK_TEST=1 RHI_EXIT_AFTER=120
  run "$APP_VK" "VK viewports4"       RHI_BACKEND=vk RHI_VIEWPORTS=4 RHI_EXIT_AFTER=120
  run "$APP_VK" "VK resize"           RHI_BACKEND=vk RHI_RESIZE_AT=60 RHI_EXIT_AFTER=120
fi

echo "ALL GREEN"
```

## 附录 B：v2 → v3 变更速查（写进 RHI_DESIGN.md 用）

| v2 | v3 | 处置 |
|---|---|---|
| `setUniform(name, value)` | `UniformRing` + `cmd.bindSet()` / `pushConstants` | 适配器保留 1 个迭代；字符串映射删除 |
| `createUniformBlock(size)` | `UniformRing::allocate(size, align)` | 删除；业务块改为 ring offset |
| `setDrawUniforms(block, data, size)` | `cmd.setUniformBlock(binding, data, size)` | 适配器保留 |
| `bindTexture(tex, slot)` | `cmd.bindTexture(binding, tex, sampler)`（槽位写 CPU 侧 set） | 适配器保留 |
| `attachSampler`（GL 私有） | `device->attachSampler(TextureHandle, SamplerHandle)`（双端设备函数） | 转正 |
| `IDescriptorSet` 对象方法 | `DescriptorSetHandle` + `cmd.bindSet(h, firstBinding, offsets, n)` | 句柄化 |
| 资源类（`ITexture` / `Texture` 包装 / `tex.bind()`） | **不引入**：`Handle` + `textureInfo(h)` | 见 ADR 3.3.1 |
| `destroy*` 立即销毁 | `DeletionQueue`：登记 `frameIndex + kFramesInFlight` 后到期销毁 | 语义修正 |
| `nativeHandle(RhiHandle)` | `nativeTexture/nativeBuffer/...` + `*Info(h)` 断言 | 删除旧签名 |
| `RenderPassDesc::framebufferId` | `RenderPassDesc::renderTarget`（`RenderTargetHandle`） | 删除 |
| `RenderPassDesc::viewportWidth/Height` | `cmd.setViewport()` | 删除字段 |
| `handle.id`（`uint32`） | `handle.id + handle.gen` | 增量，零改动 |
| `create*` 返回句柄 | `Result<Handle>` | 适配器保留 |
| `GraphicsPipelineDesc`（无附件格式） | `+AttachmentLayout / IndexFormat / RasterState` | 新增字段带默认值 |
| 每资源一次 `vkAllocateMemory` | `detail::MemoryAllocator` 页级子分配 | 内部实现替换，契约不变 |
