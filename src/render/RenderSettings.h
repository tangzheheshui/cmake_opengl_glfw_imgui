//
//  RenderSettings.h
//  渲染调试设置（ImGui 面板读写，渲染流程消费）
//
//  原则：只放"渲染管线级"开关。业务层（相机/光照）参数下一阶段再接入。
//
#include <cstdint>

#ifndef RenderSettings_h
#define RenderSettings_h

struct RenderSettings {
    // ---- RHI ----
    bool rhiSmokeTriangle = false;  // RHI 冒烟测试三角形（验证抽象层链路）
    bool rhiTexQuad = false;        // RHI 贴图四边形（M4：验证纹理采样 + 描述符链路）
    bool lineUseRHI = true;         // Line 走 RHI 绘制（M2 迁移验证，已通过逐像素等价性对比）
    bool modelUseRHI = true;        // 模型走 RHI 绘制（M4.2 迁移，GL 模式下默认走 RHI；可切回 false 对比）
    bool textUseRHI = true;         // 文字走 RHI 绘制（D4 SDF；VK 后端强制走 RHI）

    // ---- 帧级 Pass 开关 ----
    bool drawShadowPass = true;
    bool drawSkyPass    = true;
    bool drawPickPass   = true;     // 关闭后鼠标拾取失效（PickPass 提供 ID 图）

    // ---- CAD 剖切平面（D7）----
    // 世界空间平面，保留满足 dot(normal, p) + distance >= 0 的一侧
    bool  clipPlaneEnabled  = false;
    float clipPlaneNormal[3] = {1.0f, 0.0f, 0.0f};
    float clipPlaneDistance  = 0.0f;

    // ---- CAD 显示模式（D5）：0=着色 1=线框 2=着色+边线 ----
    int displayMode = 0;

    // ---- X-ray / 幻影模式：全部模型半透明透视（复用 D3 透明管线）----
    bool  xrayEnabled = false;
    float xrayOpacity = 0.35f;

    // ---- E3：CPU 视锥剔除（模型级 AABB 保守测试；像素结果不变，只省 draw）----
    bool frustumCull = true;

    // ---- LOD（E4）：远处模型换用顶点聚簇简化网格 ----
    bool  lodEnabled  = false;   // 默认关；RHI_LOD=1 或面板打开
    float lodDistance = 12.0f;   // 相机到模型中心的距离阈值
    float lodRatio    = 0.06f;   // 聚簇格子大小 = 包围盒对角线 × 该比例（越小越精细）

    // ---- CAD 选中高亮（D8）----
    // selectedId = 拾取命中的对象 ID（BaseDraw::GetID，0 = 无选中）；
    // 高亮颜色固定淡蓝，强度面板可调（0 = 关）
    uint16_t selectedId        = 0;
    float    highlightStrength = 0.40f;
    // 悬停高亮（D8 扩展）：鼠标悬停对象淡提示色；hoverEnabled=false 关闭拾取开销
    bool     hoverEnabled      = true;
    uint16_t hoverId           = 0;
    float    hoverStrength     = 0.18f;

    // ---- 全局 ----
    bool vsync          = true;
    // 注：UI 相关状态（如 ImGui Demo 窗口开关）不属于引擎设置，由 app 层面板自持

    // TODO 下一阶段接入（业务层）：
    // bool lightOpen / camera 相关 / 材质参数等
};

#endif /* RenderSettings_h */
