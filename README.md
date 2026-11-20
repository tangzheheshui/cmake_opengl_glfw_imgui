# OpenGL 渲染引擎

基于 C++17/OpenGL 3.3 Core Profile 的 3D 渲染引擎，使用 GLFW + GLAD + Dear ImGui + Assimp + FreeType 构建。

## 特性

- **多 Pass 前向渲染管线**：阴影贴图 → 主渲染 → 天空盒 → 实体拾取
- **3D 模型加载**（Assimp）：支持骨骼动画和实例化渲染
- **阴影映射**：平行光阴影（1024×1024 深度贴图）
- **字体渲染**（FreeType）：TrueType 字体实时栅格化
- **天空盒**：立方体贴图环境
- **Dear ImGui 集成**：调试 UI 覆盖层
- **鼠标交互**：拖拽旋转、滚轮缩放、点击拾取
- **Blinn-Phong 光照**：支持法线贴图和视差贴图

## 前置要求

- CMake 3.22+
- C++17 编译器（Clang / GCC / MSVC）
- OpenGL 3.3+ 支持

## 快速开始

```bash
# 克隆项目
git clone <repository-url>
cd cmake_opengl_glfw_imgui

# 配置（macOS 推荐 Xcode）
cmake -S . -B build -G Xcode

# 编译
cmake --build build

# 运行
./build/Debug/my_app
```

Windows 上：
```bash
cmake -S . -B build
cmake --build build --config Debug
```

## 项目结构

```
cmake_opengl_glfw_imgui/
├── main.cpp              # 入口点：GLFW 窗口、ImGui 初始化、场景设置
├── CMakeLists.txt         # 构建配置
├── CLAUDE.md             # 开发者架构文档
├── README.md
├── 3rdparty/             # 第三方库（glfw, glad, imgui, assimp, freetype, glm, stb_image）
├── res/
│   ├── shaders/          # GLSL 着色器（无扩展名，成对命名）
│   ├── textures/         # 纹理资源（含天空盒、PBR 材质）
│   ├── font/             # arial.ttf, roboto.ttf
│   └── model/            # duck.dae 测试模型
└── src/
    ├── core/math/        # 自定义 Matrix 类、数学工具
    └── render/
        ├── camera/       # 轨道相机 + 鼠标控制器
        ├── model/        # Assimp 模型加载、骨骼动画、网格渲染
        ├── object/       # Line, Sky, ImageRectangle, Text
        ├── pass/         # ShadowPass, NormalPass, SkyPass, PickPass
        ├── BaseDraw.h    # 可绘制对象抽象基类
        ├── scene.h       # 场景容器
        ├── shader.h      # 着色器编译与缓存
        ├── image.h       # 纹理管理（stb_image）
        ├── Light.h       # 全局光源
        └── RenderSystem.h # 渲染系统顶层编排
```

## 操作说明

| 操作 | 方式 |
|------|------|
| 旋转视图 | 鼠标左键拖拽 |
| 缩放视图 | 鼠标滚轮 |
| 退出 | ESC 键 |

## 依赖库

| 库 | 用途 |
|----|------|
| GLFW | 窗口创建、OpenGL 上下文、输入处理 |
| GLAD | OpenGL 3.3 函数加载 |
| Dear ImGui | 调试 UI |
| Assimp | 3D 模型导入（.dae, .fbx, .obj 等） |
| FreeType | TrueType 字体渲染 |
| GLM | 数学库（向量、矩阵） |
| stb_image | 图像加载（.png, .jpg, .tga） |

## 鸣谢

- [LearnOpenGL](https://learnopengl.com/) 教程
- 各开源库的作者和维护者
