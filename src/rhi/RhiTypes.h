//
//  RhiTypes.h
//  RHI 抽象层 - 公共类型定义
//
//  设计原则（重要，改动前先读）：
//  1. 本接口按 Vulkan 的抽象层级设计（显式、状态预先烘焙），
//     而不是按 OpenGL 的便捷习惯设计。
//     - Vulkan 后端 = 直译（薄封装）
//     - OpenGL 后端 = 模拟器（把显式调用翻译回 GL 状态机）
//     - 未来 Metal 后端 = 直译
//  2. 所有状态在创建期确定（管线烘焙），运行期只做 bind + draw。
//  3. uniform 采用 bgfx 风格"每次绘制更新的小数据块"，
//     而非 Vulkan descriptor set——由各后端自行攒成自己的绑定方式。
//
#ifndef RhiTypes_h
#define RhiTypes_h

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rhi {

// RHI v2 契约版本（B0 冻结，2026-11-23）。改动公共头必须同步 docs/4-RHI接口契约.md。
// v2 相比 v1 新增：DeviceDesc（宿主窗口/离屏）、RenderTarget、SamplerDesc、
// TextureDesc(mip/sRGB/sampler)、readback、对象级销毁、结构化 Capabilities。
constexpr int RhiContractVersion = 2;

constexpr uint32_t InvalidId = 0;

// ---- 句柄：强类型包装，防止不同资源句柄互相误用 ----
// V3-1：追加 gen（槽位代际，销毁/复用时 +1）。当前为纯增量字段，
// HandleTable::alive() 接入后才做校验；业务侧 h.id 用法零改动。
struct RhiHandle {
    uint32_t id  = InvalidId;
    uint32_t gen = 0;
    bool valid() const { return id != InvalidId; }
};

struct BufferHandle   : RhiHandle {};
struct TextureHandle  : RhiHandle {};
struct UniformBlockHandle : RhiHandle {};   // 绘制级 uniform 块（GL: 名字式直传；VK: 独立 UBO+描述符集）
struct PipelineHandle : RhiHandle {};
struct SamplerHandle  : RhiHandle {};
struct RenderTargetHandle : RhiHandle {};   // 离屏渲染目标（B4 实现；invalid = 默认帧缓冲）

// ---- 渲染后端 ----
enum class Backend : int {
    OpenGL,
    Vulkan,
};

// ---- 原生窗口句柄（v2：宿主嵌入；C1 接入，引擎不硬依赖 GLFW）----
enum class NativeWindowType : int {
    None = 0,
    Win32,       // handle = HWND
    Xlib,        // handle = Window, display = Display*
    Xcb,         // handle = xcb_window_t
    Wayland,     // handle = wl_surface*
    Cocoa,       // handle = NSView*
    MetalLayer,  // handle = CAMetalLayer*（MoltenVK 离屏/嵌入）
};

struct NativeWindow {
    NativeWindowType type = NativeWindowType::None;
    void* handle  = nullptr;
    void* display = nullptr;   // 仅 Xlib 用

    // C1a：GL 专用的宿主交换回调（present() 调用；Vulkan 忽略——present 走
    // vkQueuePresentKHR，离屏为 no-op）。宿主（如 GLFW demo）在窗口创建后填写。
    void (*swapBuffers)(void* user) = nullptr;
    void* swapUser = nullptr;

    bool valid() const { return type != NativeWindowType::None && handle != nullptr; }
};

// ---- 设备描述（v2；C1 接入宿主窗口/离屏）----
struct DeviceDesc {
    Backend      backend = Backend::OpenGL;
    NativeWindow window;              // valid() = 渲染到宿主窗口；否则配合 offscreen
    bool         offscreen = false;   // true：不创建 swapchain，仅渲染到 RenderTarget
    std::string  appName = "my_app";
    std::string  shaderRootPath;      // 后端变体着色器根目录（以路径分隔符结尾）
    bool         enableValidation = true;   // 后端/构建允许时启用；环境变量可覆盖
    uint32_t     swapchainImageCount = 0;   // 0 = 后端默认

    // ---- C1a 追加：帧缓冲尺寸 / Vulkan surface 接管 ----
    // 帧缓冲像素尺寸（不是窗口逻辑尺寸）。RHI 永不回查窗口系统：GL 后端
    // 保存此尺寸（onResize 更新）；VK 用于 swapchain currentExtent==UINT32_MAX
    //（组合器未定）时的回退。
    uint32_t     width  = 0;
    uint32_t     height = 0;

    // 宿主可选的 Vulkan 逃生口（公共头不出现 Vk 类型）：
    // 宿主填写 instanceExtensions/createSurface 时优先使用；留空时 RHI 按
    // NativeWindowType 自行推导平台扩展并建面。
    std::vector<std::string> instanceExtensions;
    // 在拿到 VkInstance 后由宿主创建 surface（macOS 上 GLFW 宿主内部调
    // glfwCreateWindowSurface，引擎不碰 GLFW/ObjC++）；参数/返回值按 uint64
    // 传递（VkInstance / VkSurfaceKHR）。返回 0 = 创建失败。
    std::function<uint64_t(uint64_t instance)> createSurface;
    // 或直接给已创建的 surface（宿主持有并负责销毁，RHI 不接管）
    uint64_t externalSurface = 0;
    // 所有权约定：
    //   externalSurface != 0  → RHI 不销毁，宿主负责
    //   createSurface 返回非0 → RHI 接管并在设备析构时销毁
    //   RHI 自建 surface      → RHI 销毁
};

// ---- Buffer 用途（位掩码）。Vulkan: 映射 VkBufferUsageFlags ----
enum BufferUsage : uint32_t {
    BUF_VERTEX  = 1 << 0,
    BUF_INDEX   = 1 << 1,
    BUF_UNIFORM = 1 << 2,
    // CPU 可写。GL: glBufferData(GL_DYNAMIC_DRAW)；Vulkan: HOST_VISIBLE
    BUF_CPU_WRITE = 1 << 3,
};

enum TextureUsage : uint32_t {
    TEX_SAMPLED          = 1 << 0,
    TEX_COLOR_ATTACHMENT = 1 << 1,
    TEX_DEPTH_ATTACHMENT = 1 << 2,
};

// ---- 纹理格式（每个后端负责映射到自己的枚举；枚举值已冻结，只允许追加）----
enum class Format : int {
    RGBA8_UNorm = 0,
    Depth32F    = 1,     // 深度附件
    RGBA8_SRGB,          // sRGB 采样（B3）
    BGRA8_UNorm,         // 交换链原生格式（B1/B3）
    BGRA8_SRGB,
    R8_UNorm,            // 单通道（遮罩/字体图集）
    R16F,                // 高动态范围中间缓冲
    RGBA16F,
    RGBA32F,
    D24S8,               // 深度+模板
    R32_UInt,            // 拾取 Pass ID（D2）
};

// 格式知识（字节数/深度/sRGB/GL/VK）统一收口到 rhi/FormatInfo（V3-1），见 FormatInfo::of()。

// ---- 采样器描述（v2；GL 后端 B2 落地，Vulkan 在 createTexture 内建）----
enum class FilterMode : int { Nearest, Linear };
enum class WrapMode   : int { Repeat, ClampToEdge, Mirror };

struct SamplerDesc {
    FilterMode minFilter = FilterMode::Linear;
    FilterMode magFilter = FilterMode::Linear;
    FilterMode mipFilter = FilterMode::Linear;
    WrapMode   wrapU = WrapMode::Repeat;
    WrapMode   wrapV = WrapMode::Repeat;
    WrapMode   wrapW = WrapMode::Repeat;
    float      minLod = 0.0f;
    float      maxLod = 1000.0f;
    float      maxAnisotropy = 1.0f;   // 1 = 关闭
    bool       compareEnable = false;  // 深度比较采样（阴影 PCF，后续）
};

// ---- 顶点布局（显式描述，替代 GL 隐式的 glVertexAttribPointer 顺序约定）----
enum class VertexFormat : int {
    Float1, Float2, Float3, Float4,
};

struct VertexAttributeDesc {
    uint32_t     location;   // GL: layout(location=N)；Vulkan: 与 GLSL 对齐
    VertexFormat format;
    uint32_t     offset;     // 在顶点内的字节偏移
    bool         instanced = false;  // true = 每实例更新（实例矩阵等），GL: divisor=1；VK: INSTANCE 输入率
};

struct VertexLayoutDesc {
    uint32_t stride = 0;          // 顶点率数据的单顶点字节数（binding0）
    uint32_t instanceStride = 0;  // 实例率数据的单实例字节数（binding1，有实例化属性时使用）
    std::vector<VertexAttributeDesc> attributes;
};

// ---- 着色器 ----
enum class ShaderStage : int {
    Vertex,
    Fragment,
    // Geometry: 注意 MoltenVK (macOS) 不支持几何着色器，
    // 业务侧应避免依赖（现有 model_multiview.geom 需要用多视口循环绘制替代）。
};

struct ShaderFileDesc {
    ShaderStage stage;
    // 相对于 shaderRootPath 的文件名（遵循项目约定：vert_xxx / frag_xxx）
    std::string path;
};

// ---- 图形管线：全部渲染状态在创建期烘焙 ----
enum class PrimitiveType : int { Triangles, Lines, TriangleStrip };
enum class CullMode : int { None, Back };

struct DepthState {
    bool depthTest  = true;
    bool depthWrite = true;
    bool depthLessEqual = false;  // true = LEQUAL（天空盒等远景绘制）；false = LESS
};

struct BlendState {
    bool blendEnable = false;   // true 时后端固定使用 srcAlpha/oneMinusSrcAlpha
};

// ---- 光栅状态 / 附件布局 / 索引格式（管线烘焙字段，带默认值）----
struct AttachmentLayout {
    Format   colorFormat = Format::BGRA8_SRGB;   // 颜色附件（MRT 前单色）；默认 = 交换链 sRGB
    Format   depthFormat = Format::Depth32F;
    uint32_t samples     = 1;                     // MSAA 预留（D6）
};

enum class IndexFormat : int { UINT16, UINT32 };

enum class PolygonMode : int { Fill, Line };   // Line = 线框（需 fillModeNonSolid）

struct RasterState {
    bool frontFaceCCW = false;                   // false = CW（Vulkan 约定；GL 网格 CCW 正面）
    PolygonMode polygonMode = PolygonMode::Fill; // D5：线框（需 fillModeNonSolid）
    // D5：着色+边线模式的边线深度偏移（防止线框与表面 z-fighting）
    float depthBiasConstantFactor = 0.0f;
    float depthBiasSlopeFactor    = 0.0f;
    // lineWidth 留待 D1（宽线）
};

struct GraphicsPipelineDesc {
    std::vector<ShaderFileDesc> shaders;
    VertexLayoutDesc vertexLayout;
    PrimitiveType primitive = PrimitiveType::Triangles;
    CullMode cull           = CullMode::Back;
    DepthState depth;
    BlendState blend;
    // 附件格式 / 索引格式 / 光栅状态（带默认值）
    AttachmentLayout attachment;
    IndexFormat indexFormat = IndexFormat::UINT32;
    RasterState raster;
};

// ---- Buffer / Texture 描述 ----
struct BufferDesc {
    uint32_t    size = 0;
    uint32_t    usage = BUF_VERTEX;
    const void* initialData = nullptr;  // 创建时上传（v1 不支持运行期部分更新，后续加 updateBuffer）
};

struct TextureDesc {
    uint32_t    width = 0;
    uint32_t    height = 0;
    Format      format = Format::RGBA8_UNorm;
    uint32_t    usage = TEX_SAMPLED;
    const void* initialData = nullptr;
    // cubemap = true 时：initialData 为 6 个面连续存放（+X,-X,+Y,-Y,+Z,-Z），
    // 每面 width*height*FormatInfo::of(format).bytesPerPixel() 字节
    bool        cubemap = false;

    // ---- v2 追加（B0 冻结；后端在 B2/B3 生效）----
    uint32_t    mipLevels = 1;      // 0 = 完整 mip 链（floor(log2(max(w,h)))+1）
    bool        generateMips = false;  // 创建后生成 mipmap（B3）
    uint32_t    arrayLayers = 1;    // cubemap 固定 6；普通纹理默认 1
    SamplerDesc sampler;            // 采样参数（GL: B2 独立采样器对象；VK: 创建时烘焙）
};

// ---- 离屏渲染目标（v2；B4 实现。color/depth 由后端按 desc 创建）----
struct RenderTargetDesc {
    uint32_t width  = 0;
    uint32_t height = 0;
    Format   colorFormat = Format::RGBA8_UNorm;
    bool     hasDepth    = true;
    Format   depthFormat = Format::Depth32F;
    uint32_t mipLevels   = 1;
    bool     sampleable  = true;   // 颜色附件可作纹理采样（缩略图/后处理）
};

// ---- 回读区域（v2；宽高为 0 = 全图。B4 实现，同步语义）----
struct ReadbackDesc {
    uint32_t mipLevel  = 0;
    uint32_t arrayLayer = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width  = 0;
    uint32_t height = 0;
};

// ---- 帧级渲染 Pass 描述 ----
// 对应现有 CShadowPass/CNormalPass/CSkyPass/CPickPass 的帧结构。
// Vulkan 语义：这是一个 VkRenderPass 的 load/store + clear 操作描述。
struct RenderPassDesc {
    // v2：渲染目标句柄；invalid = 默认帧缓冲（屏幕/swapchain）。
    // Vulkan：B1 起映射为 dynamic rendering 的 attachment；GL：B2 绑定对应 FBO。
    RenderTargetHandle renderTarget;

    bool     clearColor = true;
    float    clearColorValue[4] = {0.2f, 0.3f, 0.3f, 1.0f};
    bool     clearDepth = true;
    float    depthClearValue = 1.0f;
    uint32_t viewportWidth = 0;   // 0 = 使用窗口大小
    uint32_t viewportHeight = 0;
};

// ---- 每次绘制的 uniform 更新（bgfx 风格小数据块）----
// GL 后端：翻译成 glUniform* 按名字设置；
// Vulkan 后端：攒进当前 draw 的 push constant / UBO descriptor。
// 这层抽象是 GL/Vulkan 资源绑定语义差异的"缝合点"。
struct UniformValue {
    enum class Type : int { Float, Vec3, Vec4, Mat4, Int };

    Type  type = Type::Float;
    float data[16] = {};   // 最大容纳 mat4

    static UniformValue fromFloat(float v) {
        UniformValue u; u.type = Type::Float; u.data[0] = v; return u;
    }
    static UniformValue fromVec3(const float v[3]) {
        UniformValue u; u.type = Type::Vec3;
        u.data[0] = v[0]; u.data[1] = v[1]; u.data[2] = v[2];
        return u;
    }
    static UniformValue fromVec4(const float v[4]) {
        UniformValue u; u.type = Type::Vec4;
        for (int i = 0; i < 4; ++i) u.data[i] = v[i];
        return u;
    }
    static UniformValue fromMat4(const float m[16]) {
        UniformValue u; u.type = Type::Mat4;
        for (int i = 0; i < 16; ++i) u.data[i] = m[i];
        return u;
    }
    static UniformValue fromInt(int v) {
        UniformValue u; u.type = Type::Int; u.data[0] = static_cast<float>(v); return u;
    }
};

// ---- 设备信息（调试面板展示用）----
struct DeviceInfo {
    Backend     backend;
    std::string backendName;
    std::string adapterName;   // GPU 名称，如 "Apple M4 Pro"
    std::string apiVersion;    // 如 "OpenGL 4.1" / "Vulkan 1.2 (MoltenVK)"
};

// 一条设备能力/状态（后端自述，名字+值的纯文本，便于面板与日志直接输出）
struct DeviceCapability {
    std::string name;
    std::string value;
};

// ---- 结构化设备能力（v2；0/false = 未知或不支持）----
// 与 DeviceInfo（标识信息）和 details（人类可读表）互补：
// 上层按字段做路径选择（如 dynamicRendering ? 新路径 : 旧路径）。
struct DeviceCapabilities {
    uint32_t maxTexture2D            = 0;
    uint32_t maxPushConstantSize     = 0;
    uint32_t maxUniformBufferRange   = 0;
    uint32_t maxSampledImagesPerSet  = 0;
    uint64_t deviceMemoryBytes       = 0;

    bool dynamicRendering     = false;   // VK_KHR_dynamic_rendering（B1）
    bool synchronization2     = false;   // VK_KHR_synchronization2（上传环）
    bool timelineSemaphore    = false;   // VK_KHR_timeline_semaphore（E2）
    bool descriptorIndexing   = false;   // VK_EXT_descriptor_indexing
    bool memoryBudget         = false;   // VK_EXT_memory_budget（E1）
    bool wideLines            = false;   // 宽线（D1；CAD 边线质量）
    bool geometryShader       = false;   // MoltenVK 为 false
    bool samplerAnisotropy    = false;
    bool sRGBSwapchain        = false;   // 交换链含 sRGB 格式（B3）

    std::vector<DeviceCapability> details;   // 人类可读能力表（日志/面板）
};

} // namespace rhi

#endif /* RhiTypes_h */
