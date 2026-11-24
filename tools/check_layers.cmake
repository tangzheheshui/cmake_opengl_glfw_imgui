# ============================================================
# tools/check_layers.cmake —— 引擎分层检查（L1，docs/ARCHITECTURE_INTERFACES.md §4.5）
#
# 规则（§2.1 硬规则）：
#   src/rhi/**    禁止 include：render/、app/、GLFW、imgui
#   src/core/**   禁止 include：rhi/、render/、app/
#   src/render/** 禁止 include：app/、imgui、GLFW
#   engine target 链接列表不含 glfw / imgui
#
# 用法：cmake -P tools/check_layers.cmake
#   或：cmake --build <build> --target check_layers
# 命中规则 → SEND_ERROR（构建失败并指出文件）；新增合法例外必须在此登记。
# ============================================================
cmake_minimum_required(VERSION 3.22)

set(_SRC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../src")

# 已登记的合法例外（文件级；逐条注明清理归宿）
set(_ALLOWED_EXCEPTIONS
    # L0 残留：DrawUniforms ABI 迁移随 V3-2 绑定层重做
    #（见 ARCHITECTURE_INTERFACES.md 2026-11-24 状态对齐）
    "rhi/gl/GLCommandList.cpp"
)

function(_is_allowed EXC OUT)
    set(${OUT} FALSE PARENT_SCOPE)
    foreach(a ${_ALLOWED_EXCEPTIONS})
        if(EXC STREQUAL a)
            set(${OUT} TRUE PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

set(_VIOLATIONS "")

# ---- 扫描 include 依赖（只检查 #include 行，注释不误报）----
function(_scan_dir DIR PREFIX FORBIDDEN_PATTERNS LABEL)
    file(GLOB_RECURSE _files "${_SRC_ROOT}/${DIR}/*.cpp" "${_SRC_ROOT}/${DIR}/*.h")
    foreach(f ${_files})
        file(RELATIVE_PATH rel "${_SRC_ROOT}" "${f}")
        file(READ "${f}" _content)
        # 提取全部 #include 行
        string(REGEX MATCHALL "#[ \t]*include[ \t]*[<\"][^>\"]*[>\"]" _includes "${_content}")
        foreach(inc ${_includes})
            foreach(pat ${FORBIDDEN_PATTERNS})
                string(REGEX MATCH "${pat}" _m "${inc}")
                if(_m)
                    string(STRIP inc "${inc}")
                    list(APPEND _VIOLATIONS "${rel}: ${inc}")
                endif()
            endforeach()
        endforeach()
    endforeach()
    set(_VIOLATIONS ${_VIOLATIONS} PARENT_SCOPE)
endfunction()

# rhi：禁止 render/app/GLFW/imgui（例外清单兜底）
_scan_dir("rhi" "" "render/" "RHI 禁 render")
_scan_dir("rhi" "" "app/" "RHI 禁 app")
_scan_dir("rhi" "" "GLFW/glfw3.h|<GLFW/" "RHI 禁 GLFW")
_scan_dir("rhi" "" "imgui" "RHI 禁 imgui")

# core：最底层，禁止向上依赖
_scan_dir("core" "" "rhi/" "core 禁 rhi")
_scan_dir("core" "" "render/" "core 禁 render")
_scan_dir("core" "" "app/" "core 禁 app")

# render：禁止 app 层与 UI/窗口库
_scan_dir("render" "" "app/" "render 禁 app")
_scan_dir("render" "" "imgui" "render 禁 imgui")
_scan_dir("render" "" "<GLFW/|GLFW/glfw3.h|GLFWwindow" "render 禁 GLFW")

# ---- engine 链接检查 ----
file(READ "${_SRC_ROOT}/CMakeLists.txt" _src_cmake)
string(REGEX MATCH "target_link_libraries\\(engine[^\)]*\\)" _link_line "${_src_cmake}")
foreach(bad "glfw" "imgui")
    string(REGEX MATCH "(${bad}[ \t\n)]|${bad}\$)" _m "${_link_line}")
    if(_m)
        list(APPEND _VIOLATIONS "src/CMakeLists.txt: engine 链接包含 '${bad}'（引擎禁 UI/窗口库）")
    endif()
endforeach()

# ---- 裁决（对照例外清单）----
set(_FINAL "")
foreach(v ${_VIOLATIONS})
    set(_ok FALSE)
    foreach(a ${_ALLOWED_EXCEPTIONS})
        if(v MATCHES "${a}")
            set(_ok TRUE)
            break()
        endif()
    endforeach()
    if(NOT _ok)
        list(APPEND _FINAL "${v}")
    endif()
endforeach()

if(_FINAL)
    message(SEND_ERROR "check_layers: 分层违规 ${_FINAL} 条：")
    foreach(v ${_FINAL})
        message(SEND_ERROR "  ${v}")
    endforeach()
    message(SEND_ERROR "规则见 docs/ARCHITECTURE_INTERFACES.md §2.1；合法例外须在 tools/check_layers.cmake 登记")
else()
    message(STATUS "check_layers: 分层检查通过（rhi/core/render 依赖方向 + engine 链接）")
endif()
