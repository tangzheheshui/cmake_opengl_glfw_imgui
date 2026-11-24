//
//  RHIFactory.h
//  RHI 抽象层 - 后端工厂
//
#ifndef RHIFactory_h
#define RHIFactory_h

#include "RhiTypes.h"
#include "IRHIDevice.h"
#include <memory>
#include <string>

namespace rhi {

class RHIFactory {
public:
    // 唯一设备创建入口（C1a 落地；宿主不经过 GLFW/原生 API 类型）。
    //   - GL：要求宿主已创建并把 GL 上下文设为 current（RHI 不创建上下文）；
    //     desc.window.swapBuffers 由宿主提供（present 用）
    //   - Vulkan：surface 来源优先级 externalSurface → createSurface(instance)
    //     → 按 NativeWindowType 平台自建；offscreen=true 不建 swapchain
    // 失败返回 nullptr 并填充 *outError（可诊断，不抛异常）。
    static std::unique_ptr<IRHIDevice> createDevice(const DeviceDesc& desc,
                                                    std::string* outError = nullptr);
};

} // namespace rhi

#endif /* RHIFactory_h */
