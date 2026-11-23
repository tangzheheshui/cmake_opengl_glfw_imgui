#ifndef FONT_MANAGER_H
#define FONT_MANAGER_H

#include <ft2build.h>
#include FT_FREETYPE_H
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>

// 字符位图信息结构
struct Character {
    unsigned int textureID;   // 字符的纹理ID
    unsigned int width;                // 字符的宽度
    unsigned int height;               // 字符的高度
    int bearingX;             // 字符的左边距
    int bearingY;             // 字符的上边距
    long advance;              // 字符的前进宽度
};

class FontManager {
public:
    FontManager();
    ~FontManager();

    bool LoadFont(const std::string& fontPath, unsigned int fontSize);
    const Character* GetCharacter(uint32_t unicode); // 支持Unicode字符

private:
    FT_Library ft;               // FreeType 库对象
    FT_Face face = nullptr;      // 字体面（每个字体文件对应一个 face）。
                                 // 必须初始化为空：VK 模式不加载引擎字体时，
                                 // ~FontManager 若对未初始化指针调 FT_Done_Face 会段错误
                                 //（Windows 实机退出崩溃的根因，B3 验证期修复）
    std::unordered_map<uint32_t, Character> characters; // 字符缓存
    std::vector<unsigned int> m_textures; // 追踪所有分配的纹理用于清理
    // A4: 字体文件字节。FT_New_Memory_Face 不拷贝数据，缓冲必须与 face
    // 同生命周期（face 在析构时释放，故数据随对象常驻）
    std::vector<unsigned char> _fontFileData;
    bool LoadCharacter(uint32_t unicode);     // 支持Unicode字符
};

#endif // FONT_MANAGER_H
