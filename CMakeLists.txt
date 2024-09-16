cmake_minimum_required(VERSION 3.14)
project(my_app)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)

# 添加子模块
add_subdirectory(3rdparty/glfw)
add_subdirectory(3rdparty/glad)
add_subdirectory(3rdparty/imgui)
add_subdirectory(3rdparty/assimp)

# 包含头文件路径
include_directories(3rdparty/imgui)
include_directories(3rdparty/)
include_directories(src/)
include_directories(src/core/)
include_directories(src/render/)

# 源文件
set(SRC_PTAH ${CMAKE_CURRENT_SOURCE_DIR}/src)
aux_source_directory(${SRC_PTAH}/render SRC_LIST1)
aux_source_directory(${SRC_PTAH}/render/model SRC_LIST2)
aux_source_directory(${SRC_PTAH}/render/object SRC_LIST3)
aux_source_directory(${SRC_PTAH}/input SRC_LIST4)
aux_source_directory(${SRC_PTAH}/core/math SRC_LIST5)

# 按文件的物理路径分组（不影响编译）
source_group(TREE "${CMAKE_SOURCE_DIR}" FILES 
${SRC_LIST1} 
${SRC_LIST2} 
${SRC_LIST3} 
${SRC_LIST4}
${SRC_LIST5})

# 可执行文件
add_executable(my_app main.cpp ${SRC_LIST1} ${SRC_LIST2} ${SRC_LIST3} ${SRC_LIST4} ${SRC_LIST5})

# 链接库
target_link_libraries(my_app glad)
target_link_libraries(my_app glfw)
target_link_libraries(my_app imgui)
target_link_libraries(my_app assimp)
