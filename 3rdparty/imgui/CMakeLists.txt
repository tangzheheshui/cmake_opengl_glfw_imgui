cmake_minimum_required(VERSION 3.0)
project(Imgui)

# ImGui 源文件
set(IMGUI_SRC
    imgui.cpp
    imgui_draw.cpp
    imgui_demo.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    backends/imgui_impl_glfw.cpp  
    backends/imgui_impl_opengl3.cpp  
)

# 添加 imgui 头文件目录
include_directories(${CMAKE_CURRENT_SOURCE_DIR})  # 包含 imgui 头文件目录
include_directories(${CMAKE_SOURCE_DIR}/3rdparty/glfw/include)

add_library(imgui ${IMGUI_SRC})