# 渲染引擎

一个 C++ 渲染引擎，同时支持 OpenGL 和 Vulkan 两种底层图形接口，中间用自研的 RHI 层统一。目标是做成一个库，嵌进 ZW3D 这类 CAD 软件（窗口由宿主软件提供）。

## 怎么构建、运行

```bash
# 构建
cmake --preset mac-clang && cmake --build --preset mac-clang

# 运行（默认 OpenGL）
./build/app/Debug/my_app

# 运行（Vulkan）
RHI_BACKEND=vulkan ./build/app/Debug/my_app
```

更多细节（三平台预设、环境变量、Vulkan SDK 探测）见 [CLAUDE.md](CLAUDE.md)。

## 文档（按顺序读）

1. [架构设计](docs/1-架构设计.md) —— 系统分几层、每层干什么
2. [类设计与时序图](docs/2-类设计与时序图.md) —— 代码怎么组织、怎么协作
3. [ADR](docs/3-ADR.md) —— 关键决策和理由
4. [RHI接口契约](docs/4-RHI接口契约.md) —— RHI 保证什么、不能依赖什么
5. [后端对照与差异](docs/5-后端对照与差异.md) —— OpenGL 和 Vulkan 哪里不同
6. [测试验证与性能基准](docs/6-测试验证与性能基准.md) —— 怎么证明做得对
7. [实现进度](docs/7-实现进度.md) —— 现在做到哪了、下一步做什么
