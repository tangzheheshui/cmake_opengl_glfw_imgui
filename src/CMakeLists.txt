# ============================================================
# src/CMakeLists.txt —— 引擎层（engine 静态库）
#
# 分层规则：
#   1. 引擎不依赖任何 UI 库（ImGui 等），UI 是 app 层职责
#   2. 依赖分 PUBLIC/PRIVATE：
#      PUBLIC  = 公共头文件暴露的依赖（glm/glad/glfw 类型出现在头文件中）
#      PRIVATE = 仅实现使用的依赖（assimp/freetype/stb，不对外暴露）
#   3. 不使用全局 include_directories，全部 target-scoped
#   4. 未来 Vulkan 后端加在 src/rhi/vk/，仍属本库
# ============================================================

# ---- 引擎源文件 ----
set(SRC_ENGINE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB SRC_RHI        "${SRC_ENGINE_ROOT}/rhi/*.cpp")
file(GLOB SRC_RHI_GL     "${SRC_ENGINE_ROOT}/rhi/gl/*.cpp")
file(GLOB SRC_RENDER     "${SRC_ENGINE_ROOT}/render/*.cpp")
file(GLOB SRC_MODEL      "${SRC_ENGINE_ROOT}/render/model/*.cpp")
file(GLOB SRC_OBJECT     "${SRC_ENGINE_ROOT}/render/object/*.cpp")
file(GLOB SRC_PASS       "${SRC_ENGINE_ROOT}/render/pass/*.cpp")
file(GLOB SRC_CAMERA     "${SRC_ENGINE_ROOT}/render/camera/*.cpp")
file(GLOB SRC_MATH       "${SRC_ENGINE_ROOT}/core/math/*.cpp")

# 头文件列表（仅供 IDE 分组显示）
file(GLOB_RECURSE ENGINE_HEADERS "${SRC_ENGINE_ROOT}/*.h")

# ---- Vulkan 可选后端（探测结果由顶层 CMakeLists 提供：ENGINE_HAS_VULKAN）----
if(ENGINE_HAS_VULKAN)
    file(GLOB SRC_RHI_VK "${SRC_ENGINE_ROOT}/rhi/vk/*.cpp")
else()
    set(SRC_RHI_VK "")
endif()

add_library(engine STATIC
    ${SRC_RHI}
    ${SRC_RHI_GL}
    ${SRC_RHI_VK}
    ${SRC_RENDER}
    ${SRC_MODEL}
    ${SRC_OBJECT}
    ${SRC_PASS}
    ${SRC_CAMERA}
    ${SRC_MATH}
    ${ENGINE_HEADERS}
)

# Windows 中文代码页（936）下 MSVC 默认按 GBK 解析源码；本项目源码为 UTF-8，
# 不指定时会误判中文字符串（C4819 警告，严重时 C2001「常量中有换行符」）。
if(MSVC)
    target_compile_options(engine PRIVATE /utf-8)
endif()

if(ENGINE_HAS_VULKAN)
    target_include_directories(engine PRIVATE "${VULKAN_INCLUDE_DIR}")
    target_link_libraries(engine PRIVATE "${VULKAN_LIBRARY}")
    target_compile_definitions(engine PRIVATE ENGINE_HAS_VULKAN)
endif()

# ---- Vulkan 着色器：构建期 GLSL → SPIR-V（glslangValidator）----
# 产物写入源码树 res/shaders_vk/*.spv（.gitignore 已忽略），
# 运行期 VkDevice 从 RHI 设备的 shaderRootPath（res/shaders_vk/）加载
# A2: Vulkan 后端启用时 glslang 是硬依赖——缺失时直接 configure 报错，
# 而不是静默不生成 SPIR-V（运行期才报 "missing SPIR-V"）。
find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang
    HINTS "$ENV{VULKAN_SDK}/bin" /opt/homebrew/bin /usr/local/bin)
if(ENGINE_HAS_VULKAN AND NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR
        "engine: Vulkan backend enabled but glslangValidator not found. "
        "Install glslang (or Vulkan SDK), or configure with -DENGINE_WITH_VULKAN=OFF.")
endif()
if(GLSLANG_VALIDATOR AND ENGINE_HAS_VULKAN)
    file(GLOB VK_SHADER_SRCS
        "${CMAKE_SOURCE_DIR}/res/shaders_vk/*.vert"
        "${CMAKE_SOURCE_DIR}/res/shaders_vk/*.frag")
    set(VK_SPV_OUTPUTS "")
    foreach(SH ${VK_SHADER_SRCS})
        get_filename_component(SH_NAME_WE ${SH} NAME_WE)
        set(SPV_OUT "${CMAKE_SOURCE_DIR}/res/shaders_vk/${SH_NAME_WE}.spv")
        add_custom_command(
            OUTPUT ${SPV_OUT}
            COMMAND ${GLSLANG_VALIDATOR} -V ${SH} -o ${SPV_OUT}
            DEPENDS ${SH}
            COMMENT "glslang: ${SH_NAME_WE} -> SPIR-V"
            VERBATIM)
        list(APPEND VK_SPV_OUTPUTS ${SPV_OUT})
    endforeach()
    add_custom_target(vk_shaders ALL DEPENDS ${VK_SPV_OUTPUTS})
    message(STATUS "engine: vk_shaders build rules generated (${GLSLANG_VALIDATOR})")
endif()

# ---- 头文件可见性 ----
target_include_directories(engine
    # 引擎公共头目录：app 层通过这些路径 include 引擎
    PUBLIC
        "${SRC_ENGINE_ROOT}"
        "${SRC_ENGINE_ROOT}/core"
        "${SRC_ENGINE_ROOT}/render"
        "${SRC_ENGINE_ROOT}/render/camera"
        # vendored 单头/多头库（glm 出现在引擎公共 API 中，须对外可见；
        # stb_image.h 同目录）。TODO: 未来换 vcpkg/FetchContent 时收窄
        "${CMAKE_SOURCE_DIR}/3rdparty"
    # 仅实现需要的目录
    PRIVATE
        "${CMAKE_SOURCE_DIR}/3rdparty/freetype/include"
)

# ---- 依赖 ----
# PUBLIC：公共头暴露（glad/glm/assimp/freetype 类型出现在引擎公共头中——
#         第三方类型泄漏进公共 API 是已知历史债务，M4 治理，见 docs）
target_link_libraries(engine PUBLIC glad glfw glm assimp freetype)
# PRIVATE：（暂无——待公共头去第三方类型后，assimp/freetype 移入此处）
