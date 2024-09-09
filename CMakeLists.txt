cmake_minimum_required(VERSION 3.14)
project(my_app)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)

# 添加子模块
add_subdirectory(3rdparty/glfw)
add_subdirectory(3rdparty/glad)
add_subdirectory(3rdparty/imgui)

# 包含头文件路径
include_directories(3rdparty/imgui)

add_executable(my_app main.cpp)

target_link_libraries(my_app glad)
target_link_libraries(my_app glfw)
target_link_libraries(my_app imgui)
