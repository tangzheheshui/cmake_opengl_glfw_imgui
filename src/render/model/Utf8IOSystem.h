//
//  Utf8IOSystem.h —— A4: Assimp UTF-8 路径 IOSystem
//
//  Assimp 的 DefaultIOSystem 用 CRT fopen 按 ACP 解释 char 路径，
//  中文安装路径下打不开模型（Windows 实测）。此实现把路径交给
//  engine::fs（UTF-8 → 宽字符 _wfopen）打开。
//
//  用法：importer.SetIOHandler(new engine::Utf8IOSystem());
//  （Importer 析构时接管并删除该 handler）
//

#ifndef ENGINE_MODEL_UTF8_IO_SYSTEM_H
#define ENGINE_MODEL_UTF8_IO_SYSTEM_H

#include <assimp/IOSystem.hpp>
#include <assimp/IOStream.hpp>
#include <cstdio>

namespace engine {

/// FILE* 包装，实现与 DefaultIOStream 相同的语义（只读加载场景）
class Utf8IOStream : public Assimp::IOStream {
public:
    explicit Utf8IOStream(std::FILE* fp) : _fp(fp) {}
    ~Utf8IOStream() override {
        if (_fp) {
            std::fclose(_fp);
        }
    }

    size_t Read(void* buf, size_t size, size_t count) override {
        return _fp ? std::fread(buf, size, count, _fp) : 0;
    }
    size_t Write(const void* buf, size_t size, size_t count) override {
        return _fp ? std::fwrite(buf, size, count, _fp) : 0;
    }
    aiReturn Seek(size_t offset, aiOrigin origin) override {
        if (!_fp) {
            return aiReturn_FAILURE;
        }
        // aiOrigin_SET/CUR/END 与 stdio SEEK_SET/CUR/END 数值一致
        return std::fseek(_fp, static_cast<long>(offset), static_cast<int>(origin)) == 0
                   ? aiReturn_SUCCESS : aiReturn_FAILURE;
    }
    size_t Tell() const override {
        return _fp ? static_cast<size_t>(std::ftell(_fp)) : 0;
    }
    size_t FileSize() const override {
        if (!_fp) {
            return 0;
        }
        const long cur = std::ftell(_fp);
        std::fseek(_fp, 0, SEEK_END);
        const long end = std::ftell(_fp);
        std::fseek(_fp, cur, SEEK_SET);
        return end > 0 ? static_cast<size_t>(end) : 0;
    }
    void Flush() override {
        if (_fp) {
            std::fflush(_fp);
        }
    }

private:
    std::FILE* _fp = nullptr;
};

class Utf8IOSystem : public Assimp::IOSystem {
public:
    bool Exists(const char* pFile) const override;
    char getOsSeparator() const override;
    Assimp::IOStream* Open(const char* pFile, const char* pMode) override;
    void Close(Assimp::IOStream* pFile) override;
    bool ComparePaths(const char* pOne, const char* pSecond) const override;
};

} // namespace engine

#endif // ENGINE_MODEL_UTF8_IO_SYSTEM_H
