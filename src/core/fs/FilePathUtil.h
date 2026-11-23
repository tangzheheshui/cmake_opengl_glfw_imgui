//
//  FilePathUtil.h —— A4: 跨平台 UTF-8 路径
//
//  约定：引擎接口中的路径 std::string 一律按 UTF-8 解释
//  （RenderSystem::init / preloadTextures / Model::LoadFile /
//   FontManager::LoadFont / RHI shaderRootPath / RHI_VK_PIPELINE_CACHE）。
//
//  Windows 的 char 串是 ANSI 代码页（中文系统为 936），std::filesystem::path
//  构造、CRT fopen 都按 ACP 解释——中文安装路径只要编码链路有一处不一致
//  （UTF-8 源码串 / Assimp 内部转换）就会打不开文件。本模块在 OS 边界统一：
//  UTF-8 → 宽字符 path / _wfopen。POSIX 上 char 串本身就是字节路径，
//  行为与原先完全一致（macOS 的字节路径即 UTF-8）。
//
//  纯 CRT/filesystem，无 GL/UI 依赖，可在任意后端模式下使用。
//

#ifndef ENGINE_FS_FILE_PATH_UTIL_H
#define ENGINE_FS_FILE_PATH_UTIL_H

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace engine {
namespace fs {

/// UTF-8 字符串 → 原生 path（Windows: UTF-16；非法 UTF-8 回退 ACP 解释，
/// 保证不比旧行为差；POSIX: 字节透传）
std::filesystem::path pathFromUtf8(std::string_view utf8);

/// 原生 path → UTF-8 字符串。注意 Windows 上不能用 path::string()（ACP）。
std::string utf8FromPath(const std::filesystem::path& p);

/// 等价 fopen(p, "rb"/"wb")，Windows 下走 _wfopen
std::FILE* openRead(const std::filesystem::path& p);
std::FILE* openWrite(const std::filesystem::path& p);

/// 整文件二进制读入；失败返回 false 且 out 为空（调用方负责打日志）
bool readFileBinary(const std::filesystem::path& p, std::vector<unsigned char>& out);

} // namespace fs
} // namespace engine

#endif // ENGINE_FS_FILE_PATH_UTIL_H
