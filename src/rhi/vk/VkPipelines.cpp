//
//  VkPipelines.cpp
//
#include "VkPipelines.h"
#include "VkShaderReflect.h"
#include "../FormatInfo.h"
#include "fs/FilePathUtil.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace rhi {

static std::vector<char> readSpirvFile(const std::string& path) {
    std::vector<unsigned char> data;
    if (!engine::fs::readFileBinary(engine::fs::pathFromUtf8(path), data)) {
        ENGINE_LOG_ERROR("[VK] cannot open SPIR-V: %s", path.c_str());
        return {};
    }
    return std::vector<char>(data.begin(), data.end());
}

static bool pipelineCacheHeaderMatches(const std::vector<char>& data,
                                       const VkPhysicalDeviceProperties& props) {
    if (data.size() < sizeof(VkPipelineCacheHeaderVersionOne)) return false;
    VkPipelineCacheHeaderVersionOne h{};
    std::memcpy(&h, data.data(), sizeof(h));
    return h.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE
        && h.vendorID == props.vendorID
        && h.deviceID == props.deviceID
        && std::memcmp(h.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

void VkPipelines::init(VkDevice device, const VkCaps* caps, std::string shaderRoot,
                       VkPipelineLayout layout, NameFn nameFn) {
    _device = device;
    _caps   = caps;
    _shaderRoot = std::move(shaderRoot);
    _layout = layout;
    _nameFn = std::move(nameFn);
    if (!_nameFn) _nameFn = [](VkObjectType, uint64_t, const char*) {};
    loadCache();
}

void VkPipelines::loadCache() {
    const char* env = std::getenv("RHI_VK_PIPELINE_CACHE");
    if (env && std::strcmp(env, "0") == 0) {
        ENGINE_LOG_INFO("[VK] pipeline cache disabled (RHI_VK_PIPELINE_CACHE=0)");
        return;
    }
    _cachePath = (env && *env) ? std::string(env)
                                       : _shaderRoot + "pipeline_cache.bin";

    std::vector<char> initial;
    {
        // A4: UTF-8 路径（fstream 接 path 对象，Windows 宽字符安全）
        std::ifstream f(engine::fs::pathFromUtf8(_cachePath), std::ios::binary | std::ios::ate);
        if (f.is_open()) {
            const size_t size = static_cast<size_t>(f.tellg());
            initial.resize(size);
            f.seekg(0);
            f.read(initial.data(), static_cast<std::streamsize>(size));
            if (!pipelineCacheHeaderMatches(initial, _caps->props)) {
                ENGINE_LOG_INFO("[VK] pipeline cache ignored (driver/device mismatch): %s",
                                _cachePath.c_str());
                initial.clear();
            }
        }
    }

    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = initial.size();
    ci.pInitialData    = initial.empty() ? nullptr : initial.data();
    VkResult r = vkCreatePipelineCache(_device, &ci, nullptr, &_cache);
    if (r != VK_SUCCESS && !initial.empty()) {
        // 文件损坏或驱动拒绝：丢弃后重试（缓存只是优化，不应阻塞启动）
        ENGINE_LOG_WARN("[VK] pipeline cache load failed (%d), recreating empty", static_cast<int>(r));
        initial.clear();
        ci.initialDataSize = 0;
        ci.pInitialData    = nullptr;
        r = vkCreatePipelineCache(_device, &ci, nullptr, &_cache);
    }
    if (r != VK_SUCCESS) {
        ENGINE_LOG_WARN("[VK] pipeline cache unavailable (%d), pipelines compile uncached", static_cast<int>(r));
        _cache = VK_NULL_HANDLE;
        return;
    }
    _nameFn(VK_OBJECT_TYPE_PIPELINE_CACHE, reinterpret_cast<uint64_t>(_cache), "RHI_PipelineCache");
    _cacheFromDisk = !initial.empty();
    _cacheBytes = initial.size();
    ENGINE_LOG_INFO("[VK] pipeline cache %s: %zu bytes (%s)",
                    _cacheFromDisk ? "loaded" : "empty",
                    _cacheBytes, _cachePath.c_str());
}

void VkPipelines::saveCache() {
    if (!_cache || _cachePath.empty()) return;

    size_t size = 0;
    if (vkGetPipelineCacheData(_device, _cache, &size, nullptr) != VK_SUCCESS || size == 0) {
        return;   // 本进程没有新建管线（全部命中缓存）时通常为空
    }
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(_device, _cache, &size, data.data()) != VK_SUCCESS) return;
    data.resize(size);

    // A4: _cachePath 是 UTF-8 串，构造宽 path 后做临时文件+改名
    const std::filesystem::path cachePath = engine::fs::pathFromUtf8(_cachePath);
    std::filesystem::path tmpPath = cachePath;
    tmpPath += ".tmp";
    {
        std::error_code ec;
        auto parent = cachePath.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            ENGINE_LOG_WARN("[VK] cannot write pipeline cache: %s", _cachePath.c_str());
            return;
        }
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) {
            ENGINE_LOG_WARN("[VK] pipeline cache write failed: %s", _cachePath.c_str());
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmpPath, cachePath, ec);   // 先写临时文件再改名，避免崩溃留下半截缓存
    if (ec) {
        ENGINE_LOG_WARN("[VK] pipeline cache rename failed: %s", ec.message().c_str());
        return;
    }
    ENGINE_LOG_INFO("[VK] pipeline cache saved: %zu bytes -> %s", data.size(), _cachePath.c_str());
}

void VkPipelines::saveAndDestroy() {
    if (_cache) {
        saveCache();
        vkDestroyPipelineCache(_device, _cache, nullptr);
        _cache = VK_NULL_HANDLE;
    }
}

bool VkPipelines::create(const GraphicsPipelineDesc& desc, VkPipelineRes& res) {
    // ---- 附件格式：按管线声明烘焙（MRT / MSAA 尚未实现，显式拒绝而非静默降级）----
    if (desc.attachment.colorCount != 1) {
        ENGINE_LOG_ERROR("[VK] createPipeline: MRT not implemented (colorCount=%u)",
                         desc.attachment.colorCount);
        return false;
    }
    if (desc.attachment.samples != 1) {
        ENGINE_LOG_ERROR("[VK] createPipeline: MSAA not implemented (samples=%u)",
                         desc.attachment.samples);
        return false;
    }
    const Format colorFormat = desc.attachment.colorFormats[0];
    const Format depthFormat = desc.attachment.depthFormat;
    const VkFormat colorFmt = static_cast<VkFormat>(FormatInfo::of(colorFormat).vkFormat());
    const VkFormat depthFmt = static_cast<VkFormat>(FormatInfo::of(depthFormat).vkFormat());
    if (colorFmt == VK_FORMAT_UNDEFINED || depthFmt == VK_FORMAT_UNDEFINED) {
        ENGINE_LOG_ERROR("[VK] createPipeline: unusable attachment format");
        return false;
    }
    res.colorFormat = colorFormat;
    res.depthFormat = depthFormat;

    // ---- 着色器模块：从 shaders_vk/ 读取 <name>.spv ----
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    std::vector<VkShaderModule> modules;
    for (const auto& s : desc.shaders) {
        const std::string path = _shaderRoot + s.path + ".spv";
        std::vector<char> code = readSpirvFile(path);
        if (code.empty()) {
            ENGINE_LOG_ERROR("[VK] missing SPIR-V for pipeline");
            return false;
        }
        // B6：反射 push constant 成员 → 管线 uniform 表（setUniform 按名定位）
        {
            auto refl = reflectSpirv(code.data(), code.size());
            for (const auto& m : refl.pushMembers) {
                if (m.name.empty()) continue;
                auto dup = std::find_if(res.uniformTable.begin(), res.uniformTable.end(),
                                        [&](const VkPipelineUniformInfo& u) {
                                            return std::strcmp(u.name, m.name.c_str()) == 0;
                                        });
                if (dup != res.uniformTable.end()) continue;   // 两 stage 重复声明，取首个
                VkPipelineUniformInfo ui{};
                std::snprintf(ui.name, sizeof(ui.name), "%s", m.name.c_str());
                ui.offset = m.offset;
                ui.size   = m.size;
                ui.isMat4 = m.isMat4;
                res.uniformTable.push_back(ui);
            }
        }
        VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mci.codeSize = code.size();
        mci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule mod;
        if (vkCreateShaderModule(_device, &mci, nullptr, &mod) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] vkCreateShaderModule failed for %s", path.c_str());
            for (auto m : modules) vkDestroyShaderModule(_device, m, nullptr);
            return false;
        }
        modules.push_back(mod);

        VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        st.stage  = (s.stage == ShaderStage::Vertex) ? VK_SHADER_STAGE_VERTEX_BIT
                                                     : VK_SHADER_STAGE_FRAGMENT_BIT;
        st.module = mod;
        st.pName  = "main";
        stages.push_back(st);
    }

    // ---- 顶点输入 ----
    // ---- 顶点输入：binding0 = 顶点率，binding1 = 实例率（有实例化属性时）----
    bool hasInstanced = false;
    for (const auto& a : desc.vertexLayout.attributes) hasInstanced |= a.instanced;
    VkVertexInputBindingDescription binds[2] = {};
    binds[0] = {0, desc.vertexLayout.stride, VK_VERTEX_INPUT_RATE_VERTEX};
    binds[1] = {1, desc.vertexLayout.instanceStride, VK_VERTEX_INPUT_RATE_INSTANCE};
    std::vector<VkVertexInputAttributeDescription> attrs;
    for (const auto& a : desc.vertexLayout.attributes) {
        VkVertexInputAttributeDescription va{};
        va.location = a.location;
        va.binding  = a.instanced ? 1 : 0;
        va.offset   = a.offset;
        switch (a.format) {
            case VertexFormat::Float1: va.format = VK_FORMAT_R32_SFLOAT; break;
            case VertexFormat::Float2: va.format = VK_FORMAT_R32G32_SFLOAT; break;
            case VertexFormat::Float3: va.format = VK_FORMAT_R32G32B32_SFLOAT; break;
            case VertexFormat::Float4: va.format = VK_FORMAT_R32G32B32A32_SFLOAT; break;
        }
        attrs.push_back(va);
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount   = hasInstanced ? 2 : 1;
    vi.pVertexBindingDescriptions      = binds;
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions    = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    switch (desc.primitive) {
        case PrimitiveType::Triangles:     ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        case PrimitiveType::Lines:         ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case PrimitiveType::TriangleStrip: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    }

    // ---- 动态 viewport/scissor ----
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    // D5：线框显示模式（GraphicsPipelineDesc::raster.polygonMode，B0 只加不改）
    rs.polygonMode = (desc.raster.polygonMode == PolygonMode::Line)
                         ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    if (desc.raster.polygonMode == PolygonMode::Line && !_caps->features.fillModeNonSolid) {
        ENGINE_LOG_WARN_ONCE("[VK] fillModeNonSolid unsupported; wireframe falls back to fill");
        rs.polygonMode = VK_POLYGON_MODE_FILL;
    }
    rs.cullMode    = (desc.cull == CullMode::Back) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    // B3a：负高度 viewport 把 NDC y 镜像到屏幕，绕序相对 GL 约定翻转；
    // GL 网格（CCW 正面）在此后端声明 CLOCKWISE 才能正确剔除背面
    rs.frontFace   = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    // D5：边线深度偏移（着色+边线模式防止线框与表面 z-fighting）
    rs.depthBiasEnable         = (desc.raster.depthBiasConstantFactor != 0.0f
                                  || desc.raster.depthBiasSlopeFactor != 0.0f) ? VK_TRUE : VK_FALSE;
    rs.depthBiasConstantFactor = desc.raster.depthBiasConstantFactor;
    rs.depthBiasSlopeFactor    = desc.raster.depthBiasSlopeFactor;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    // M4：render pass 已有深度附件，深度状态由管线描述符驱动
    ds.depthTestEnable  = desc.depth.depthTest  ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = desc.depth.depthWrite ? VK_TRUE : VK_FALSE;
    // B3b Reversed-Z：深度语义反转（近=1/远=0，清屏 0，见 VkCommandList::setUniform），
    // 比较算子相应改为 GREATER 族；depthLessEqual（天空盒远景）映射 GREATER_OR_EQUAL
    ds.depthCompareOp   = desc.depth.depthLessEqual ? VK_COMPARE_OP_GREATER_OR_EQUAL
                                                    : VK_COMPARE_OP_GREATER;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (desc.blend.blendEnable) {
        att.blendEnable    = VK_TRUE;
        att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.colorBlendOp        = VK_BLEND_OP_ADD;
        att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;

    VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynamics;

    // V3-3：管线布局（set0 + 4 个纹理槽 + 固定 push 布局）由 VkDescriptors 建好，
    // 所有管线共用同一份（见 VkDescriptors::createPoolsAndLayouts）

    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount          = static_cast<uint32_t>(stages.size());
    gpi.pStages             = stages.data();
    gpi.pVertexInputState   = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState      = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState   = &ms;
    gpi.pDepthStencilState  = &ds;
    gpi.pColorBlendState    = &cb;
    gpi.pDynamicState       = &dyn;
    gpi.layout              = _layout;
    // B1：dynamic rendering —— 管线经 PipelineRenderingCreateInfo 绑定附件格式，
    // 不再引用 VkRenderPass → swapchain 重建无需重建 PSO
    VkPipelineRenderingCreateInfo prci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    prci.colorAttachmentCount    = 1;
    prci.pColorAttachmentFormats = &colorFmt;   // 来自 desc.attachment（V3-12）
    prci.depthAttachmentFormat   = depthFmt;
    gpi.pNext               = &prci;
    gpi.renderPass          = VK_NULL_HANDLE;
    gpi.subpass             = 0;
    // V3-0：管线创建失败返回空句柄（不 abort）；着色器模块已销毁
    if (vkCreateGraphicsPipelines(_device, _cache, 1, &gpi, nullptr, &res.pipeline) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] vkCreateGraphicsPipelines failed at %s:%d", __FILE__, __LINE__);
        for (auto m : modules) vkDestroyShaderModule(_device, m, nullptr);
        return false;
    }
    {
        char pname[48];
        std::snprintf(pname, sizeof(pname), "RHI_Pipeline_%u", _nextId++);
        _nameFn(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(res.pipeline), pname);
    }

    for (auto m : modules) vkDestroyShaderModule(_device, m, nullptr);

    res.alive = true;
    return true;
}

} // namespace rhi
