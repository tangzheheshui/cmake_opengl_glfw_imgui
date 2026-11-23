//
//  DebugPanel.h
//  ImGui 渲染调试面板
//
//  在主循环的 ImGui 帧内调用 draw()（见 main.cpp / RenderSystem::renderDebugUI）
//
#ifndef DebugPanel_h
#define DebugPanel_h

#include "RenderSettings.h"

namespace rhi {
class IRHIDevice;
}

class DebugPanel {
public:
    // device 可为空（RHI 未启用时面板降级为纯 Pass 开关）
    static void draw(RenderSettings& settings, const rhi::IRHIDevice* device);
};

#endif /* DebugPanel_h */
