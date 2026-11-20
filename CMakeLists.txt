cmake_minimum_required(VERSION 3.22)
project(my_app)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)

# 添加子模块
add_subdirectory(3rdparty/glfw)
add_subdirectory(3rdparty/glad)
add_subdirectory(3rdparty/imgui)
add_subdirectory(3rdparty/assimp)
add_subdirectory(3rdparty/freetype)
add_subdirectory(3rdparty/glm)

# 收集所有的头文件
file(GLOB_RECURSE HEADER_FILES
"${CMAKE_SOURCE_DIR}/src/*.h"
)

# 源文件
set(SRC_PATH ${CMAKE_CURRENT_SOURCE_DIR}/src)
file(GLOB SRC_RENDER     "${SRC_PATH}/render/*.cpp")
file(GLOB SRC_MODEL      "${SRC_PATH}/render/model/*.cpp")
file(GLOB SRC_OBJECT     "${SRC_PATH}/render/object/*.cpp")
file(GLOB SRC_PASS       "${SRC_PATH}/render/pass/*.cpp")
file(GLOB SRC_CAMERA     "${SRC_PATH}/render/camera/*.cpp")
file(GLOB SRC_MATH       "${SRC_PATH}/core/math/*.cpp")

# 按文件的物理路径分组（不影响编译）
source_group(TREE "${CMAKE_SOURCE_DIR}" FILES
${SRC_RENDER}
${SRC_MODEL}
${SRC_OBJECT}
${SRC_CAMERA}
${SRC_MATH}
${HEADER_FILES}
${SRC_PASS}
)

# 包含头文件路径
include_directories(3rdparty/imgui)
include_directories(3rdparty)
include_directories(3rdparty/freetype/include)
include_directories(src/)
include_directories(src/core/)
include_directories(src/render/)
include_directories(src/render/camera/)

# 可执行文件
add_executable(my_app main.cpp
${SRC_RENDER}
${SRC_MODEL}
${SRC_OBJECT}
${SRC_CAMERA}
${SRC_MATH}
${HEADER_FILES}
${SRC_PASS}
)

# 链接库
target_link_libraries(my_app glad)
target_link_libraries(my_app glfw)
target_link_libraries(my_app imgui)
target_link_libraries(my_app assimp)
target_link_libraries(my_app freetype)
target_link_libraries(my_app glm)
