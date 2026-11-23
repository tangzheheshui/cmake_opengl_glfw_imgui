# ============================================================
# app/CMakeLists.txt —— 应用层（my_app 可执行文件）
#
# 职责：窗口创建、ImGui UI、场景组装
# 约束：只通过 engine 的公共接口使用引擎，UI 相关代码全部在此层
# ============================================================

file(GLOB SRC_APP "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# Vulkan 可选（A2）：探测结果由顶层 CMakeLists 提供（ENGINE_HAS_VULKAN）；
# main.cpp 通过 ENGINE_HAS_VULKAN 宏裁剪 Vulkan 代码，GL-only 构建可用。
if(ENGINE_HAS_VULKAN)
    list(APPEND SRC_APP "${CMAKE_SOURCE_DIR}/3rdparty/imgui/backends/imgui_impl_vulkan.cpp")
    message(STATUS "app: ImGui Vulkan backend enabled")
endif()

add_executable(my_app
    ${SRC_APP}
)

# Windows 中文代码页（936）下的 UTF-8 源码解析（见 src/CMakeLists.txt）
if(MSVC)
    target_compile_options(my_app PRIVATE /utf-8)
endif()

target_include_directories(my_app
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"          # app 自己的头（DebugPanel.h）
        "${CMAKE_SOURCE_DIR}/3rdparty/imgui"   # ImGui 头（imgui target 未提供 interface include）
)

target_link_libraries(my_app
    PRIVATE
        engine    # 引擎库（PUBLIC 传递 glad/glfw/glm）
        imgui     # ImGui 核心 + OpenGL backend（仅 app 层链接）
        glfw      # main.cpp 直接创建/操作窗口
)

if(ENGINE_HAS_VULKAN)
    target_compile_definitions(my_app PRIVATE ENGINE_HAS_VULKAN=1)
    target_include_directories(my_app PRIVATE ${VULKAN_INCLUDE_DIR})
    target_link_libraries(my_app PRIVATE ${VULKAN_LIBRARY})
else()
    target_compile_definitions(my_app PRIVATE ENGINE_HAS_VULKAN=0)
endif()
