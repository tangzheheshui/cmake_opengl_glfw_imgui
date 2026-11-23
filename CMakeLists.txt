# ============================================================
# cmake_opengl_glfw_imgui —— 顶层构建脚本
#
# 三层结构（详见 docs/RHI_DESIGN.md）：
#   3rdparty/   third-party 层：各库独立 target，互不知晓
#   src/        engine 层：静态库 libengine（RHI + 渲染系统），禁含 UI
#   app/        app 层：可执行文件 my_app（窗口 + ImGui UI + 场景组装）
#
# 规则：不使用全局 include_directories，依赖全部 target-scoped，
#       依赖方向 app → engine → third-party，禁止反向
# ============================================================
cmake_minimum_required(VERSION 3.22)
project(my_app)

# C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- third-party 层 ----
add_subdirectory(3rdparty/glfw)
add_subdirectory(3rdparty/glad)
add_subdirectory(3rdparty/imgui)
add_subdirectory(3rdparty/assimp)
add_subdirectory(3rdparty/freetype)
add_subdirectory(3rdparty/glm)

# imgui 的 vendored CMakeLists 未提供 interface include，
# 在此补上 target 属性（不改动第三方文件本体）
set_target_properties(imgui PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/3rdparty/imgui"
)

# ---- engine 层 ----
add_subdirectory(src)

# ---- app 层 ----
add_subdirectory(app)
