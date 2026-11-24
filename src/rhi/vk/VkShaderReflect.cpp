//
//  VkShaderReflect.cpp —— B6: SPIR-V 最小反射器（见头文件说明）
//
//  枚举值实测校准依据（glslangValidator 产物，vert_model.spv/vert_rhi_smoke.spv）：
//    OpName=5 OpMemberName=6 OpDecorate=71 OpMemberDecorate=72
//    OpTypeInt=21 OpTypeFloat=22 OpTypeVector=23 OpTypeMatrix=24
//    OpTypeStruct=30 OpTypePointer=32 OpConstant=43 OpVariable=59
//    StorageClass: Input=1 Uniform=2 Output=3 PushConstant=9
//    Decoration: Block=2 BuiltIn=11 Location=30 Offset=35
//                DescriptorSet=33 Binding=34
//

#include "log/LogMacros.h"
#include "VkShaderReflect.h"

#include <algorithm>
#include <functional>
#include <map>
#include <unordered_map>

namespace rhi {

namespace {

constexpr uint32_t kOpName           = 5;
constexpr uint32_t kOpMemberName     = 6;
constexpr uint32_t kOpTypeInt        = 21;
constexpr uint32_t kOpTypeFloat      = 22;
constexpr uint32_t kOpTypeVector     = 23;
constexpr uint32_t kOpTypeMatrix     = 24;
constexpr uint32_t kOpTypeStruct     = 30;
constexpr uint32_t kOpTypePointer    = 32;
constexpr uint32_t kOpMemberDecorate = 72;
constexpr uint32_t kOpVariable       = 59;

constexpr uint32_t kStoragePushConstant = 9;
constexpr uint32_t kStorageUniform      = 2;

constexpr uint32_t kDecorOffset = 35;

std::string readString(const uint32_t* words, uint32_t count) {
    std::string s;
    s.reserve(count * 4);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t w = words[i];
        for (int b = 0; b < 4; ++b) {
            const char c = static_cast<char>((w >> (b * 8)) & 0xFF);
            if (c == '\0') return s;
            s.push_back(c);
        }
    }
    return s;
}

struct TypeDesc {
    uint32_t          storageClass = 0;   // OpTypePointer
    uint32_t          pointee      = 0;   // OpTypePointer -> 目标类型
    uint32_t          width        = 0;   // Float/Int 位宽
    uint32_t          columns      = 1;   // Matrix
    uint32_t          vecSize      = 1;   // Vector
    bool              isPointer    = false;
    bool              isMatrix     = false;
    bool              isVector     = false;
    bool              isScalar     = false;   // Float/Int
    std::vector<uint32_t> members;            // Struct 成员类型 id
};

// 成员注解（OpMemberDecorate Offset）
struct MemberAnno {
    uint32_t offset = 0;
    bool     hasOffset = false;
};

} // namespace

ShaderReflectResult reflectSpirv(const char* data, size_t size) {
    ShaderReflectResult out;
    if (!data || size < 20) {
        ENGINE_LOG_WARN("[VK][reflect] empty SPIR-V");
        return out;
    }
    const uint32_t* words = reinterpret_cast<const uint32_t*>(data);
    const size_t count = size / 4;
    if (words[0] != 0x07230203u) {
        ENGINE_LOG_WARN("[VK][reflect] bad SPIR-V magic 0x%08x", words[0]);
        return out;
    }

    // ---- Pass A：全量收集 id -> 类型 / 名字 / 注解 ----
    std::unordered_map<uint32_t, TypeDesc> types;             // id -> 类型
    std::unordered_map<uint32_t, std::string> names;          // id -> OpName
    std::unordered_map<uint32_t, std::map<uint32_t, std::string>> memberNames; // struct id -> member -> name
    std::unordered_map<uint32_t, std::map<uint32_t, MemberAnno>> memberOffsets; // struct id -> member -> offset
    std::unordered_map<uint32_t, uint32_t> varStorage;        // 变量 result id -> storage class
    std::unordered_map<uint32_t, uint32_t> varPointer;        // 变量 result id -> pointer type id

    size_t i = 5;   // 跳过 header
    while (i < count) {
        const uint32_t w0   = words[i];
        const uint32_t op   = w0 & 0xFFFF;
        const uint32_t wc   = (w0 >> 16) & 0xFFFF;
        if (wc == 0) break;
        const uint32_t* a   = words + i + 1;          // 操作数起始
        const uint32_t  na  = wc - 1;                 // 操作数个数

        switch (op) {
            case kOpName:
                if (na >= 2) names[a[0]] = readString(a + 1, na - 1);
                break;
            case kOpMemberName:
                if (na >= 3) memberNames[a[0]][a[1]] = readString(a + 2, na - 2);
                break;
            case kOpTypeInt:
                if (na >= 2) types[a[0]] = TypeDesc{0, 0, a[1], 1, 1, false, false, false, true};
                break;
            case kOpTypeFloat:
                if (na >= 2) types[a[0]] = TypeDesc{0, 0, a[1], 1, 1, false, false, false, true};
                break;
            case kOpTypeVector:
                // 操作数：<id> Result, <id> Component Type, Literal Size
                if (na >= 2) types[a[0]] = TypeDesc{0, a[1], 0, 1, a[2], false, false, true, false};
                break;
            case kOpTypeMatrix:
                // 操作数：<id> Result, <id> Column Type, Literal Columns
                if (na >= 2) types[a[0]] = TypeDesc{0, a[1], 0, a[2], 1, false, true, false, false};
                break;
            case kOpTypeStruct:
                types[a[0]] = TypeDesc{0, 0, 0, 1, 1, false, false, false, false};
                for (uint32_t k = 1; k < na; ++k) types[a[0]].members.push_back(a[k]);
                break;
            case kOpTypePointer:
                if (na >= 2) {
                    TypeDesc t;
                    t.isPointer    = true;
                    t.storageClass = a[1];
                    t.pointee      = a[2];
                    types[a[0]]    = t;
                }
                break;
            case kOpMemberDecorate:
                if (na >= 3 && a[2] == kDecorOffset) {
                    MemberAnno anno;
                    anno.offset    = a[3];
                    anno.hasOffset = true;
                    memberOffsets[a[0]][a[1]] = anno;
                }
                break;
            case kOpVariable:
                if (na >= 2) {
                    varPointer[a[1]] = a[0];
                    varStorage[a[1]] = a[2];
                }
                break;
            default:
                break;
        }
        i += wc;
    }

    // ---- Pass B：变量 → 块成员展开 ----
    std::function<uint32_t(uint32_t, uint32_t)> typeSizeOf =
        [&](uint32_t typeId, uint32_t depth) -> uint32_t {
        auto it = types.find(typeId);
        if (it == types.end() || depth > 8) return 0;
        const TypeDesc& t = it->second;
        if (t.isMatrix)  return 64;                       // mat4（列主序 4×vec4）
        if (t.isVector)  return typeSizeOf(t.pointee, depth + 1) * t.vecSize;
        if (t.isScalar)  return t.width / 8;
        if (!t.members.empty()) {
            // struct：取成员最大 offset+size（offset 注解缺失则按顺序累加近似）
            uint32_t total = 0;
            auto offs = memberOffsets.find(typeId);
            for (uint32_t m = 0; m < t.members.size(); ++m) {
                const uint32_t msz = typeSizeOf(t.members[m], depth + 1);
                uint32_t mo = msz;
                if (offs != memberOffsets.end()) {
                    auto mit = offs->second.find(m);
                    if (mit != offs->second.end()) mo = mit->second.offset;
                }
                total = std::max(total, mo + msz);
            }
            return total;
        }
        return 0;
    };
    auto isMat4Type = [&](uint32_t typeId) -> bool {
        auto it = types.find(typeId);
        return it != types.end() && it->second.isMatrix;
    };

    for (const auto& kv : varStorage) {
        const uint32_t varId = kv.first;
        const uint32_t sc    = kv.second;
        if (sc != kStoragePushConstant && sc != kStorageUniform) continue;
        const uint32_t ptrId = varPointer[varId];
        auto pit = types.find(ptrId);
        if (pit == types.end() || !pit->second.isPointer) continue;
        const uint32_t structId = pit->second.pointee;
        auto stit = types.find(structId);
        if (stit == types.end() || stit->second.members.empty()) continue;

        auto& dst     = (sc == kStoragePushConstant) ? out.pushMembers : out.uboMembers;
        auto& hasFlag = (sc == kStoragePushConstant) ? out.hasPushConstant : out.hasUniformBlock;
        hasFlag = true;

        auto nameIt  = memberNames.find(structId);
        auto offsIt  = memberOffsets.find(structId);
        const TypeDesc& st = stit->second;
        for (uint32_t m = 0; m < st.members.size(); ++m) {
            ReflectMember rm;
            if (nameIt != memberNames.end()) {
                auto mit = nameIt->second.find(m);
                if (mit != nameIt->second.end()) rm.name = mit->second;
            }
            if (offsIt != memberOffsets.end()) {
                auto oit = offsIt->second.find(m);
                if (oit != offsIt->second.end()) rm.offset = oit->second.offset;
            }
            rm.size  = typeSizeOf(st.members[m], 0);
            rm.isMat4 = isMat4Type(st.members[m]);
            dst.push_back(std::move(rm));
        }
    }

    return out;
}

} // namespace rhi
