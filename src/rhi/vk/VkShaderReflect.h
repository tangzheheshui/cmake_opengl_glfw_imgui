//
//  VkShaderReflect.h —— B6: SPIR-V 最小反射器
//
//  目标：从 glslangValidator 产物中解析出
//    · push constant 块成员（名字 / 字节偏移 / 类型），替换
//      VkCommandList::setUniform 的 "uMatrixMVP@0 / uColor@64" 字符串硬编码——
//      此前每加一个 uniform 名都要改映射表，漏改即静默丢失（D2 实踩）
//    · 顶层 uniform（UBO）块成员（FrameData，供后续绑定层演进）
//  只解析注解指令（OpName/OpMemberName/OpDecorate/OpMemberDecorate）与类型图，
//  不做全量 SPIR-V 语义解析。操作码/装饰枚举已对 glslangValidator 实测校准
//  （vert_model.spv：PC 块 uViewProj@0 / uClipPlane@64 与硬编码一致）。
//
//  mat4/vec4/标量外的类型（数组/嵌套 struct/采样器）不在解析范围，跳过。
//

#ifndef VkShaderReflect_h
#define VkShaderReflect_h

#include <cstdint>
#include <string>
#include <vector>

namespace rhi {

struct ReflectMember {
    std::string name;
    uint32_t    offset = 0;   // 字节偏移（PC 内 / UBO 内）
    uint32_t    size   = 0;   // 字节数（mat4=64, vec4=16, float/int=4；未知 = 0）
    bool        isMat4 = false;
};

struct ShaderReflectResult {
    std::vector<ReflectMember> pushMembers;    // push constant 块成员
    std::vector<ReflectMember> uboMembers;     // set? Uniform 块成员（FrameData）
    bool hasPushConstant = false;
    bool hasUniformBlock = false;
};

/// 解析 SPIR-V（readSpirvFile 的产物）；失败返回空结果并打日志
ShaderReflectResult reflectSpirv(const char* data, size_t size);

} // namespace rhi

#endif // VkShaderReflect_h
