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

# ---- Vulkan 可选后端探测（只做一次，engine/app 共用；A2）----
# 探测顺序：环境变量 VULKAN_SDK（Windows/Linux 官方 SDK）→ brew /usr/local → 系统路径。
# 找不到时降级为仅 GL 后端（产品回退路径），不中断构建。
option(ENGINE_WITH_VULKAN "Build the Vulkan RHI backend (auto-disabled if SDK not found)" ON)
set(ENGINE_HAS_VULKAN OFF)
if(NOT ENGINE_WITH_VULKAN)
    message(STATUS "vulkan: disabled by ENGINE_WITH_VULKAN=OFF (GL backend only)")
endif()
if(ENGINE_WITH_VULKAN)
    set(_vk_hints "")
    if(DEFINED ENV{VULKAN_SDK})
        list(APPEND _vk_hints "$ENV{VULKAN_SDK}")
        message(STATUS "vulkan: VULKAN_SDK = $ENV{VULKAN_SDK}")
    endif()
    find_path(VULKAN_INCLUDE_DIR vulkan/vulkan.h
        HINTS ${_vk_hints} /opt/homebrew /usr/local /usr
        PATH_SUFFIXES include Include)
    find_library(VULKAN_LIBRARY NAMES vulkan vulkan-1 MoltenVK
        HINTS ${_vk_hints} /opt/homebrew /usr/local /usr
        PATH_SUFFIXES lib lib64 Lib)
    if(VULKAN_INCLUDE_DIR AND VULKAN_LIBRARY)
        set(ENGINE_HAS_VULKAN ON)
        message(STATUS "vulkan: backend enabled -> ${VULKAN_LIBRARY}")
    else()
        message(STATUS "vulkan: not found (set VULKAN_SDK, or -DVULKAN_INCLUDE_DIR/-DVULKAN_LIBRARY; "
                       "-DENGINE_WITH_VULKAN=OFF to silence)")
    endif()
endif()

# ---- engine 层 ----
add_subdirectory(src)

# ---- app 层 ----
add_subdirectory(app)
