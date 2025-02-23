#include "FontManager.h"
#include <iostream>
#include <stdexcept>
#include <glad/glad.h>

FontManager::FontManager() {
    if (FT_Init_FreeType(&ft)) {
        throw std::runtime_error("ERROR::FREETYPE: Could not init FreeType library");
    }
}

FontManager::~FontManager() {
    FT_Done_Face(face);
    FT_Done_FreeType(ft);
}

bool FontManager::LoadFont(const std::string& fontPath, unsigned int fontSize) {
    // 加载字体
    if (FT_New_Face(ft, fontPath.c_str(), 0, &face)) {
        std::cerr << "ERROR::FREETYPE: Failed to load font: " << fontPath << std::endl;
        return false;
    }

    // 设置字体大小
    if (FT_Set_Pixel_Sizes(face, 0, fontSize)) {
        std::cerr << "ERROR::FREETYPE: Failed to set pixel size" << std::endl;
        return false;
    }

    // 加载常用字符（包括英文和部分符号）
    for (uint32_t c = 32; c < 128; ++c) {
        if (!LoadCharacter(c)) {
            std::cerr << "ERROR::FREETYPE: Failed to load character: " << c << std::endl;
            return false;
        }
    }

    return true;
}

const Character* FontManager::GetCharacter(uint32_t unicode) {
    auto it = characters.find(unicode);
    if (it != characters.end()) {
        return &it->second;
    }

    // 如果未找到该字符，尝试加载它
    if (!LoadCharacter(unicode)) {
        std::cerr << "ERROR::FREETYPE: Failed to load character: " << unicode << std::endl;
        return nullptr;
    }

    return &characters[unicode]; // 返回加载后的字符
}

bool FontManager::LoadCharacter(uint32_t unicode) {
    // 如果该字符已加载，则直接返回
    if (characters.find(unicode) != characters.end()) {
        return true;
    }

    // 加载字符的位图
    if (FT_Load_Char(face, unicode, FT_LOAD_RENDER)) {
        std::cerr << "ERROR::FREETYPE: Failed to load Glyph for Unicode: " << unicode << std::endl;
        return false;
    }

    FT_Bitmap& bitmap = face->glyph->bitmap;

    // 创建纹理
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, bitmap.width, bitmap.rows, 0, GL_RED, GL_UNSIGNED_BYTE, bitmap.buffer);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // 存储字符数据
    Character character = {
        texture,
        bitmap.width,
        bitmap.rows,
        face->glyph->bitmap_left,
        face->glyph->bitmap_top,
        face->glyph->advance.x >> 6 // 转换为像素
    };

    characters[unicode] = character;
    return true;
}
