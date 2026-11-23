//
//  Utf8IOSystem.cpp —— A4: Assimp UTF-8 路径 IOSystem（见头文件说明）
//

#include "Utf8IOSystem.h"
#include "fs/FilePathUtil.h"

#include <cstring>
#include <filesystem>
#include <system_error>

namespace engine {

bool Utf8IOSystem::Exists(const char* pFile) const {
    if (!pFile) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(fs::pathFromUtf8(pFile), ec);
}

char Utf8IOSystem::getOsSeparator() const {
    return static_cast<char>(std::filesystem::path::preferred_separator);
}

Assimp::IOStream* Utf8IOSystem::Open(const char* pFile, const char* pMode) {
    if (!pFile || !pMode) {
        return nullptr;
    }
    // Assimp 只用 "r"/"rb"（读模型）；写模式仅导出器使用，一并支持
    const bool writeMode = std::strchr(pMode, 'w') != nullptr || std::strchr(pMode, 'a') != nullptr;
    std::FILE* fp = writeMode ? fs::openWrite(fs::pathFromUtf8(pFile))
                              : fs::openRead(fs::pathFromUtf8(pFile));
    return fp ? new Utf8IOStream(fp) : nullptr;
}

void Utf8IOSystem::Close(Assimp::IOStream* pFile) {
    delete pFile;   // Utf8IOStream 析构 fclose
}

bool Utf8IOSystem::ComparePaths(const char* pOne, const char* pSecond) const {
    return pOne && pSecond && std::strcmp(pOne, pSecond) == 0;
}

} // namespace engine
