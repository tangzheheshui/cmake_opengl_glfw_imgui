# CLAUDE.md

## 项目概述

C++17 OpenGL 3.3 Core Profile 渲染引擎，基于 GLFW + GLAD + Dear ImGui + Assimp + FreeType 构建。支持多 Pass 前向渲染管线，包括阴影映射、天空盒、模型加载/实例化、骨骼动画、字体渲染和实体拾取。

## 构建

### 前置要求
- CMake 3.22+
- C++17 编译器
- macOS: Xcode（命令行工具）
- Windows: Visual Studio 2019+

### 构建命令

```bash
# macOS (Xcode)
cmake -S . -B build -G Xcode
cmake --build build

# Windows (Visual Studio)
cmake -S . -B build
cmake --build build

# 仅命令行构建
cmake -S . -B build -G "Unix Makefiles"
cmake --build build
```

运行的二进制文件位于 `build/Debug/my_app`（macOS Debug 模式）。

## 架构

### 目录结构

```
src/
  core/
    math/              -- 自定义 Matrix 类、数学工具函数
    taskQueue.h        -- 多线程任务队列（目前未大量使用）
  render/
    BaseDraw.h         -- 所有可绘制对象的抽象基类（VAO/VBO/EBO 管理）
    shader.cpp/h       -- GLSL 着色器编译 + ShaderCache 单例
    image.cpp/h        -- TextureMng 单例（stb_image 纹理加载）
    Light.h            -- 全局平行光定义
    FontManager.cpp/h  -- FreeType 字体位图生成 + OpenGL 纹理缓存
    RenderSystem.cpp/h -- 顶层编排：Pass 管线、输入事件、场景管理
    scene.cpp/h        -- 场景容器：相机、天空盒、可绘制对象列表、光源 VP 矩阵
    camera/
      camera_old.cpp/h  -- 活跃的 Camera 类（轨道相机，欧拉角控制）
      CameraController.cpp/h -- 鼠标拖拽/缩放相机控制器
    model/
      model.cpp/h      -- Assimp 模型加载、骨骼动画、实例化渲染
      mesh.cpp/h       -- 网格对象，持有 RenderPass
      pass.cpp/h       -- RenderPass 基类（网格级绘制策略）
      PassColor.cpp/h  -- 纯色着色 Pass
      PassTexture.cpp/h -- 纹理着色 Pass（支持多视口、骨骼蒙皮）
      model_data.h     -- 数据定义（MeshData, Material, Animation, DrawOption）
      aabb.cpp/h       -- 包围盒
    object/
      Line.cpp/h       -- 3D 线段（坐标轴等）
      ImageRectangle.cpp/h -- 带纹理的矩形面片（视差/法线贴图）
      Sky.cpp/h        -- 立方体贴图天空盒
      Text.cpp/h       -- FreeType 文本渲染
    pass/
      RenderPass.cpp/h -- IRenderPass 接口 + RenderContext
      ShadowPass.cpp/h -- 阴影贴图生成 Pass
      NormalPass.cpp/h -- 主场景渲染 Pass（Blinn-Phong 光照）
      SkyPass.cpp/h    -- 天空盒渲染 Pass
      PickPass.cpp/h   -- 实体拾取 Pass（ID 编码）
3rdparty/              -- 第三方库（glfw, glad, imgui, assimp, freetype, glm, stb_image）
res/
  shaders/             -- GLSL 着色器（无扩展名，按 vert_* / frag_* / *.geom 命名）
  textures/            -- 纹理资源
  font/                -- 字体文件（arial.ttf, roboto.ttf）
  model/               -- 3D 模型（duck.dae）
```

### 核心类层次

**可绘制对象（均继承 BaseDraw）：**
```
BaseDraw
  ├── Model          （3D 模型，包含 N 个 Mesh，每个 Mesh 持有 RenderPass）
  ├── Line           （3D 线段）
  ├── Sky            （天空盒立方体贴图）
  ├── CText          （字体文字渲染）
  └── ImageRectangle （纹理矩形面片）
```

**帧级渲染 Pass（继承 IRenderPass）：**
```
IRenderPass
  ├── CShadowPass    （阴影贴图生成）
  ├── CNormalPass    （主场景渲染）
  ├── CSkyPass       （天空盒渲染）
  └── CPickPass      （实体拾取）
```

**网格级绘制策略（模型内部使用）：**
```
RenderPass
  ├── PassColor      （纯色着色）
  └── PassTexture    （纹理着色）
```

### 渲染管线

`RenderSystem::draw()` 按顺序执行：

1. **ShadowPass** — 从光源视角渲染深度图到 FBO（1024x1024）
2. **NormalPass** — 主渲染通道，使用 Blinn-Phong 光照模型，支持法线贴图、位移贴图
3. **SkyPass** — 天空盒渲染（关闭深度写入）
4. **PickPass** — 颜色编码的物体 ID 渲染，用于鼠标拾取

### 数据流

```
main.cpp
  └── RenderSystem::getInstance()
        ├── init(rootPath)    -- 加载纹理、着色器、字体、创建 Pass
        ├── update()          -- 每帧更新
        ├── draw()            -- 遍历 Pass → 遍历 Scene 对象 → 调用 draw/drawShadow/drawPick
        └── 鼠标/窗口事件     -- 分发到 CameraController
```

### 关键单例

| 类 | 获取方式 | 职责 |
|----|----------|------|
| `RenderSystem` | `RenderSystem::getInstance()` | 顶层编排 |
| `ShaderCache` | `ShaderCache::GetInstance()` | 着色器编译与缓存 |
| `TextureMng` | `TextureMng::getInstance()` | 纹理加载与缓存 |
| `Light::GlobalLight()` | 静态方法 | 全局光源参数 |
| `TaskQueue` | `TaskQueue::instance()` | 异步任务队列 |
| `Camera::GetCamera()` | 静态方法（deprecated 用法） | 全局相机 |

### 着色器命名约定

着色器文件无扩展名，成对使用：
- 片元着色器：`frag_<name>`
- 顶点着色器：`frag_<name>`
- 几何着色器：`<name>.geom`

示例：`vert_model` + `frag_model` 组成 Model_Texture 着色器程序。

`ShaderType` 枚举（`shader.h`）的每个成员对应一对着色器，在 `ShaderCache::init()` 中注册。

### DrawOption 位掩码

定义在 `model_data.h` 的 `DrawOption` 枚举中：
```
NONE        = 0
DRAW_SHADOW = 1<<0    -- 阴影渲染模式
LIGHT_OPEN  = 1<<1    -- 启用光照
MULTI_VIEWPORT = 1<<2 -- 多视口实例化
DRAW_PICK   = 1<<3    -- 拾取渲染模式
```

## 重要约定

- **OpenGL 上下文**：所有 OpenGL 调用必须在主线程中进行（单线程渲染）
- **资源管理**：可绘制对象使用 `std::shared_ptr`，Scene 持有所有权
- **Pass 顺序**：ShadowPass 必须在 NormalPass 之前执行（FBO 依赖）
- **BaseDraw 继承**：每个子类的 `draw()`/`drawShadow()`/`drawPick()` 方法需要自行管理着色器绑定和 VAO 绑定
- **着色器 uniform 名称**：部分遗留拼写（如 `uMaterail` 而非 `uMaterial`），修改时需同步更新 GLSL 文件

## 已知限制

- 仅支持一个全局平行光源（`Light::GlobalLight()`）
- 前向渲染，无延迟渲染支持
- 相机控制耦合在 `camera_old.h` 中，包含两个相机类遗留问题
- 部分代码注释为 GBK 编码，macOS 下显示为乱码
- 纹理资源 96MB，未使用 Git LFS
- 暂不支持 PBR 材质管线
