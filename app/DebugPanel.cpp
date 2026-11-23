//
//  DebugPanel.cpp
//  ImGui 渲染调试面板（app 层，唯一允许使用 ImGui 的文件）
//
#include "DebugPanel.h"
#include "rhi/IRHIDevice.h"
#include "imgui.h"
#include "GLFW/glfw3.h"
#include <cstdlib>

void DebugPanel::draw(RenderSettings& settings, const rhi::IRHIDevice* device) {
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);   // 固定左上角，不悬浮居中
    ImGui::Begin("渲染调试", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    // ---- 后端信息 ----
    if (device) {
        const rhi::DeviceInfo info = device->info();
        ImGui::Text("后端: %s", info.backendName.c_str());
        ImGui::Text("GPU: %s", info.adapterName.c_str());
        ImGui::Text("API: %s", info.apiVersion.c_str());

        // ---- 设备能力/运行状态（A3；启动时同一份内容已写入日志）----
        if (ImGui::CollapsingHeader("设备能力")) {
            const rhi::DeviceCapabilities caps = device->capabilities();
            if (caps.details.empty()) {
                ImGui::TextDisabled("（后端未提供）");
            } else {
                ImGui::BeginChild("caps_child", ImVec2(0, 280), ImGuiChildFlags_Borders);
                if (ImGui::BeginTable("caps_table", 2,
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
                    for (const auto& c : caps.details) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(c.name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(c.value.c_str());
                    }
                    ImGui::EndTable();
                }
                ImGui::EndChild();
            }
        }
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "RHI 设备未创建");
    }
    ImGui::Separator();

    // ---- RHI 验证内容 ----
    ImGui::Checkbox("RHI 冒烟三角形", &settings.rhiSmokeTriangle);
    ImGui::Checkbox("RHI 贴图四边形", &settings.rhiTexQuad);

    // ---- M2/M4 迁移验证（双路径对比）----
    if (ImGui::CollapsingHeader("迁移验证", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Line 走 RHI", &settings.lineUseRHI);
        ImGui::Checkbox("模型走 RHI", &settings.modelUseRHI);
        ImGui::TextDisabled("关闭 = 原 GL 路径，打开 = RHI 路径，可实时切换对比");
    }
    ImGui::Separator();

    // ---- 帧级 Pass 开关 ----
    if (ImGui::CollapsingHeader("帧级 Pass", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("阴影 Pass", &settings.drawShadowPass);
        ImGui::Checkbox("天空盒 Pass", &settings.drawSkyPass);
        if (ImGui::Checkbox("拾取 Pass", &settings.drawPickPass)) {
            if (!settings.drawPickPass) {
                ImGui::OpenPopup("PickPassOff");
            }
        }
        if (ImGui::BeginPopupModal("PickPassOff", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("注意：关闭拾取 Pass 后鼠标拾取将失效。");
            if (ImGui::Button("知道了")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    // ---- 全局 ----
    if (ImGui::CollapsingHeader("全局", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("垂直同步", &settings.vsync)) {
            glfwSwapInterval(settings.vsync ? 1 : 0);
        }
    }

    ImGui::End();

    // 调试设施：前台红色方块（ImGui 绘制链路验证，RHI_REDTEST=1 启用）
    if (std::getenv("RHI_REDTEST")) {
        ImGui::GetForegroundDrawList()->AddRectFilled(
            ImVec2(200, 200), ImVec2(900, 900), IM_COL32(255, 0, 0, 255));
    }
}
