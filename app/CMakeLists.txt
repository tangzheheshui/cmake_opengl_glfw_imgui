# ============================================================
# app/CMakeLists.txt —— 应用层（my_app 可执行文件）
#
# 职责：窗口创建、ImGui UI、场景组装
# 约束：只通过 engine 的公共接口使用引擎，UI 相关代码全部在此层
# ============================================================

file(GLOB SRC_APP "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# Vulkan 可选：为 ImGui Vulkan backend 提供源文件与链接
find_path(VULKAN_INCLUDE_DIR vulkan/vulkan.h HINTS /opt/homebrew/include /usr/include)
find_library(VULKAN_LIBRARY NAMES vulkan HINTS /opt/homebrew/lib /usr/lib)
set(APP_IMGUI_VK_SRC "")
if(VULKAN_INCLUDE_DIR AND VULKAN_LIBRARY)
    list(APPEND SRC_APP "${CMAKE_SOURCE_DIR}/3rdparty/imgui/backends/imgui_impl_vulkan.cpp")
    message(STATUS "app: ImGui Vulkan backend enabled")
endif()

add_executable(my_app
    ${SRC_APP}
)

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

if(VULKAN_INCLUDE_DIR AND VULKAN_LIBRARY)
    target_include_directories(my_app PRIVATE ${VULKAN_INCLUDE_DIR})
    target_link_libraries(my_app PRIVATE ${VULKAN_LIBRARY})
endif()
