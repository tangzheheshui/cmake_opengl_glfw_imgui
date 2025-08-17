cmake_minimum_required(VERSION 3.14)
project(my_app)

if (MSVC)
    ADD_COMPILE_OPTIONS(/source-charset:utf-8)
endif()

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)

# 添加子模块
add_subdirectory(3rdparty/glfw)
add_subdirectory(3rdparty/glad)
add_subdirectory(3rdparty/imgui)
add_subdirectory(3rdparty/assimp)
add_subdirectory(3rdparty/freetype)

# 收集所有的头文件
file(GLOB_RECURSE SOURCES 
    "src/*.cpp"
    "src/*.h"
)

# 按文件的物理路径分组（不影响编译）

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" 
    FILES ${SOURCES}
)

# 包含头文件路径
include_directories(3rdparty/imgui)
include_directories(3rdparty)
include_directories(3rdparty/freetype/include)
include_directories(src/)
include_directories(src/core/)
include_directories(src/render/)

# 可执行文件
add_executable(my_app app/main.cpp 
${SOURCES} 
)

# 链接库
target_link_libraries(my_app glad)
target_link_libraries(my_app glfw)
target_link_libraries(my_app imgui)
target_link_libraries(my_app assimp)
target_link_libraries(my_app freetype)
