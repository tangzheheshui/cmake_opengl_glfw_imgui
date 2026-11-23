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
#include <string>
#include <vector>

namespace rhi {

constexpr uint32_t InvalidId = 0;

// ---- 句柄：强类型包装，防止不同资源句柄互相误用 ----
struct RhiHandle {
    uint32_t id = InvalidId;
    bool valid() const { return id != InvalidId; }
};

struct BufferHandle   : RhiHandle {};
struct TextureHandle  : RhiHandle {};
struct UniformBlockHandle : RhiHandle {};   // 绘制级 uniform 块（GL: 名字式直传；VK: 独立 UBO+描述符集）
struct PipelineHandle : RhiHandle {};
struct SamplerHandle  : RhiHandle {};

// ---- 渲染后端 ----
enum class Backend : int {
    OpenGL,
    Vulkan,
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

// ---- 纹理格式（按需扩充；每个后端负责映射到自己的枚举）----
enum class Format : int {
    RGBA8_UNorm,
    Depth32F,
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

struct GraphicsPipelineDesc {
    std::vector<ShaderFileDesc> shaders;
    VertexLayoutDesc vertexLayout;
    PrimitiveType primitive = PrimitiveType::Triangles;
    CullMode cull           = CullMode::Back;
    DepthState depth;
    BlendState blend;
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
    // 每面 width*height*4 字节 RGBA8
    bool        cubemap = false;
};

// ---- 帧级渲染 Pass 描述 ----
// 对应现有 CShadowPass/CNormalPass/CSkyPass/CPickPass 的帧结构。
// Vulkan 语义：这是一个 VkRenderPass 的 load/store + clear 操作描述。
struct RenderPassDesc {
    // framebuffer 的正式抽象（TextureHandle 数组）在 M3 Vulkan 后端时引入。
    // GL 阶段：0 = 默认帧缓冲（屏幕），其它值 = FBO 对象名（阴影 Pass 现有 FBO 可直通）。
    uint32_t framebufferId = 0;
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

} // namespace rhi

#endif /* RhiTypes_h */
