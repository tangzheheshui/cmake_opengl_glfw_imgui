#ifndef FONT_MANAGER_H
#define FONT_MANAGER_H

#include <ft2build.h>
#include FT_FREETYPE_H
#include <unordered_map>
#include <string>
#include <memory>
#include <codecvt>

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
    FT_Face face;                // 字体面（每个字体文件对应一个 face）
    std::unordered_map<uint32_t, Character> characters; // 字符缓存
    bool LoadCharacter(uint32_t unicode);     // 支持Unicode字符
};

#endif // FONT_MANAGER_H
