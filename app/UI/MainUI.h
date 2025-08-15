#pragma once
#include "imgui.h"

void RenderUI()
{
    ImGui::Begin("功能列表", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    // 核心渲染功能
    if (ImGui::TreeNode("1. 核心渲染")) {
        ImGui::BulletText("基础渲染管线");
        ImGui::BulletText("顶点/片段着色器支持");
        ImGui::BulletText("多重采样抗锯齿(MSAA)");
        ImGui::BulletText("视口管理");
        ImGui::TreePop();
    }
    
    // 材质系统
    if (ImGui::TreeNode("2. 材质系统")) {
        ImGui::BulletText("材质资源管理");
        ImGui::BulletText("Shader变体支持");
        ImGui::BulletText("Uniform缓冲区");
        
        if (ImGui::TreeNode("材质类型")) {
            ImGui::BulletText("PBR材质");
            ImGui::BulletText("Phong材质");
            ImGui::BulletText("自定义Shader材质");
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }
    
    // 光照系统
    if (ImGui::TreeNode("3. 光照系统")) {
        ImGui::BulletText("方向光");
        ImGui::BulletText("点光源");
        ImGui::BulletText("聚光灯");
        ImGui::BulletText("阴影映射");
        ImGui::BulletText("环境光遮蔽");
        ImGui::TreePop();
    }
    
    // 几何体管理
    if (ImGui::TreeNode("4. 几何体管理")) {
        ImGui::BulletText("网格加载与缓存");
        ImGui::BulletText("实例化渲染");
        ImGui::BulletText("LOD系统");
        ImGui::TreePop();
    }
    
    // 后期处理
    if (ImGui::TreeNode("5. 后期处理")) {
        ImGui::BulletText("HDR色调映射");
        ImGui::BulletText("Bloom效果");
        ImGui::BulletText("屏幕空间反射");
        ImGui::BulletText("景深效果");
        ImGui::TreePop();
    }
    
    // 调试工具
    if (ImGui::TreeNode("6. 调试工具")) {
        ImGui::BulletText("帧分析器");
        ImGui::BulletText("GPU计时器");
        ImGui::BulletText("调试绘制");
        ImGui::BulletText("性能统计");
        ImGui::TreePop();
    }
    ImGui::End();
}
