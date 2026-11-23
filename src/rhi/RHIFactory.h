//
//  RHIFactory.h
//  RHI 抽象层 - 后端工厂
//
#ifndef RHIFactory_h
#define RHIFactory_h

#include "RhiTypes.h"
#include "IRHIDevice.h"
#include <string>

struct GLFWwindow;

namespace rhi {

class RHIFactory {
public:
    // 创建指定后端的渲染设备。
    // backend = OpenGL   : 当前可用（复用现有 GLFW 窗口与 GL 上下文）
    // backend = Vulkan   : 里程碑 M3 实现，当前返回 nullptr 并输出日志
    //
    // shaderRootPath: 着色器根目录（与 ShaderCache 使用的路径一致，
    //                 形如 ".../res/shaders/"，含末尾分隔符）
    static std::unique_ptr<IRHIDevice> createDevice(Backend backend,
                                                    GLFWwindow* window,
                                                    const std::string& shaderRootPath);
};

} // namespace rhi

#endif /* RHIFactory_h */
