//
//  DebugPanel.cpp
//  ImGui 渲染调试面板（app 层，唯一允许使用 ImGui 的文件）
//
//  顶部主菜单栏（BeginMainMenuBar）：固定横跨窗口上方，像普通软件菜单。
//
#include "DebugPanel.h"
#include "rhi/IRHIDevice.h"
#include "RenderSystem.h"
#include "scene.h"
#include "model/model.h"
#include "imgui.h"
#include "GLFW/glfw3.h"
#include <cstdlib>
#include <string>

void DebugPanel::draw(RenderSettings& settings, const rhi::IRHIDevice* device) {
    if (!ImGui::BeginMainMenuBar()) return;

    // ---- 左侧：后端信息 ----
    if (device) {
        const rhi::DeviceInfo info = device->info();
        ImGui::TextDisabled("%s | %s", info.backendName.c_str(), info.adapterName.c_str());
    } else {
        ImGui::TextDisabled("RHI 未创建");
    }

    // ---- 渲染 ----
    if (ImGui::BeginMenu("渲染")) {
        ImGui::MenuItem("RHI 冒烟三角形", nullptr, &settings.rhiSmokeTriangle);
        ImGui::MenuItem("RHI 贴图四边形", nullptr, &settings.rhiTexQuad);
        ImGui::Separator();
        ImGui::MenuItem("阴影 Pass", nullptr, &settings.drawShadowPass);
        ImGui::MenuItem("天空盒 Pass", nullptr, &settings.drawSkyPass);
        ImGui::EndMenu();
    }

    // ---- 显示 ----
    if (ImGui::BeginMenu("显示")) {
        const char* modes[] = { "着色", "线框", "着色+边线" };
        ImGui::Combo("显示模式", &settings.displayMode, modes, 3);
        ImGui::MenuItem("剖切平面", nullptr, &settings.clipPlaneEnabled);
        if (settings.clipPlaneEnabled) {
            ImGui::SliderFloat("剖切位置 X", &settings.clipPlaneDistance, -20.0f, 20.0f);
        }
        ImGui::Separator();
        ImGui::MenuItem("X-ray 透视", nullptr, &settings.xrayEnabled);
        if (settings.xrayEnabled) {
            ImGui::SliderFloat("透视不透明度", &settings.xrayOpacity, 0.05f, 0.9f, "%.2f");
        }
        ImGui::MenuItem("CPU 视锥剔除", nullptr, &settings.frustumCull);
        ImGui::Separator();
        // 选中状态
        if (settings.lastPickResult < 0)
            ImGui::TextDisabled("最近拾取: 未点击");
        else if (settings.lastPickResult == 0)
            ImGui::TextDisabled("最近拾取: 未命中");
        else
            ImGui::TextDisabled("最近拾取: 命中 id=%d", settings.lastPickResult);
        ImGui::Text("选中对象 ID: %u%s", settings.selectedId, settings.selectedId == 0 ? "（无）" : "");
        if (settings.selectedId != 0) {
            ImGui::SliderFloat("高亮强度", &settings.highlightStrength, 0.0f, 1.0f, "%.2f");
            if (ImGui::MenuItem("取消选中")) settings.selectedId = 0;
        }
        ImGui::MenuItem("悬停提示", nullptr, &settings.hoverEnabled);
        ImGui::EndMenu();
    }

    // ---- PBR 材质（选中对象时可用）----
    if (ImGui::BeginMenu("PBR 材质")) {
        auto scene = RenderSystem::getInstance().GetCurScene();
        std::shared_ptr<Model> selModel;
        if (scene && settings.selectedId != 0) {
            for (auto& obj : scene->GetObjs()) {
                if (obj->GetID() != settings.selectedId) continue;
                selModel = std::dynamic_pointer_cast<Model>(obj);
                break;
            }
        }
        if (selModel) {
            static const char* matNames[] = { "砖墙", "金属", "木墙板", "竹子", "瓷砖", "木墙板2" };
            static const char* matPrefix[] = { "Bricks102_1K-JPG", "Metal048A_1K-JPG", "WoodSiding001_1K-JPG",
                                               "Bamboo001C_1K-JPG", "Tiles001_1K-JPG", "WoodSiding006_1K-JPG" };
            static int curMat = -1;
            if (ImGui::Combo("换材质", &curMat, matNames, 6)) {
                if (curMat >= 0 && curMat < 6) {
                    const std::string base = std::string("res/textures/pbr/") + matPrefix[curMat];
                    selModel->setPbrTexture("albedo",     base + "_Color.jpg");
                    selModel->setPbrTexture("normal",     base + "_NormalGL.jpg");
                    selModel->setPbrTexture("metalrough", base + "_MetalRough.jpg");
                    selModel->setPbrTexture("ao",         base + "_AmbientOcclusion.jpg");
                    selModel->setMetallic(1.0f);
                    selModel->setRoughness(1.0f);
                }
            }
            float metallic  = selModel->getMetallic();
            float roughness = selModel->getRoughness();
            if (ImGui::SliderFloat("金属度", &metallic, 0.0f, 1.0f, "%.2f"))
                selModel->setMetallic(metallic);
            if (ImGui::SliderFloat("粗糙度", &roughness, 0.0f, 1.0f, "%.2f"))
                selModel->setRoughness(roughness);
        } else {
            ImGui::TextDisabled("点击模型选中后，可换材质 / 调金属度粗糙度");
        }
        ImGui::EndMenu();
    }

    // ---- 全局 ----
    if (ImGui::BeginMenu("全局")) {
        if (ImGui::MenuItem("垂直同步", nullptr, &settings.vsync)) {
            glfwSwapInterval(settings.vsync ? 1 : 0);
        }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();

    // 调试设施：前台红色方块（ImGui 绘制链路验证，RHI_REDTEST=1 启用）
    if (std::getenv("RHI_REDTEST")) {
        ImGui::GetForegroundDrawList()->AddRectFilled(
            ImVec2(200, 200), ImVec2(900, 900), IM_COL32(255, 0, 0, 255));
    }
}
