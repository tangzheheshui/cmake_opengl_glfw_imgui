//
//  FilePathUtil.cpp —— A4: 跨平台 UTF-8 路径（见头文件说明）
//

#include "fs/FilePathUtil.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace engine {
namespace fs {

#if defined(_WIN32)

std::filesystem::path pathFromUtf8(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    // 严格 UTF-8 -> UTF-16（MB_ERR_INVALID_CHARS：非法序列报错而不是吞字节）
    const int wlen = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           utf8.data(), static_cast<int>(utf8.size()),
                                           nullptr, 0);
    if (wlen > 0) {
        std::wstring wide(static_cast<size_t>(wlen), L'\0');
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                              utf8.data(), static_cast<int>(utf8.size()),
                              &wide[0], wlen);
        return std::filesystem::path(std::move(wide));
    }
    // 非法 UTF-8（历史 ACP 编码串等）：按系统 ACP 解释，不比旧行为差
    return std::filesystem::path(std::string(utf8));
}

std::string utf8FromPath(const std::filesystem::path& p) {
    const std::wstring& wide = p.native();
    if (wide.empty()) {
        return {};
    }
    const int len = ::WideCharToMultiByte(CP_UTF8, 0,
                                          wide.data(), static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (len > 0) {
        std::string utf8(static_cast<size_t>(len), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0,
                              wide.data(), static_cast<int>(wide.size()),
                              &utf8[0], len, nullptr, nullptr);
        return utf8;
    }
    return p.string();   // 理论不可达；兜底不比旧行为差
}

std::FILE* openRead(const std::filesystem::path& p) {
    return ::_wfopen(p.c_str(), L"rb");
}

std::FILE* openWrite(const std::filesystem::path& p) {
    return ::_wfopen(p.c_str(), L"wb");
}

#else // POSIX

std::filesystem::path pathFromUtf8(std::string_view utf8) {
    return std::filesystem::path(std::string(utf8));   // 字节路径透传
}

std::string utf8FromPath(const std::filesystem::path& p) {
    return p.string();
}

std::FILE* openRead(const std::filesystem::path& p) {
    return std::fopen(p.c_str(), "rb");
}

std::FILE* openWrite(const std::filesystem::path& p) {
    return std::fopen(p.c_str(), "wb");
}

#endif

bool readFileBinary(const std::filesystem::path& p, std::vector<unsigned char>& out) {
    out.clear();
    std::FILE* f = openRead(p);
    if (!f) {
        return false;
    }
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }
    const long size = std::ftell(f);
    if (size < 0 || std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    const size_t got = size > 0 ? std::fread(out.data(), 1, out.size(), f) : 0;
    std::fclose(f);
    if (got != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

} // namespace fs
} // namespace engine
