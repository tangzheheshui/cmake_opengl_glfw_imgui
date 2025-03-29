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

// SDF 生成函数
std::vector<uint8_t> GenerateSDF(const FT_Bitmap& bitmap, float spread = 2)
{
    const int width = bitmap.width;
    const int height = bitmap.rows;

    std::vector<uint8_t> sdfData(width * height, 0);

    // 定义最大搜索半径 (可调)
    const float maxDist = std::sqrt(static_cast<float>(spread * spread + spread * spread));

    // 遍历每个像素
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {

            // 获取当前像素的灰度值 (0-255)
            const bool inside = bitmap.buffer[y * bitmap.pitch + x] > 0;

            float minDist = maxDist;

            // 扫描邻域 (计算最短距离)
            for (int dy = -spread; dy <= spread; ++dy) {
                for (int dx = -spread; dx <= spread; ++dx) {
                    int nx = x + dx;
                    int ny = y + dy;

                    if (nx < 0 || nx >= width || ny < 0 || ny >= height)
                        continue;

                    const bool neighborInside = bitmap.buffer[ny * bitmap.pitch + nx] > 0;

                    if (inside != neighborInside) {
                        float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                        minDist = std::min(minDist, dist);
                    }
                }
            }

            // 距离场归一化为 [0, 255]
            float normalizedDist = (minDist / maxDist) * 128.0f;
            sdfData[y * width + x] = inside ?
                static_cast<uint8_t>(128 + normalizedDist) : // 字体内
                static_cast<uint8_t>(128 - normalizedDist);  // 字体外
        }
    }

    return sdfData;
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

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); //禁用字节对齐限制

    auto data = GenerateSDF(bitmap, 3);
    unsigned char* tmpBuffer = data.data();
    if (0) {
        tmpBuffer = bitmap.buffer;
    }
    // 创建纹理
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, bitmap.width, bitmap.rows, 0, GL_RED, GL_UNSIGNED_BYTE, tmpBuffer);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // 存储字符数据
    Character character = {
        texture,
        bitmap.width,
        bitmap.rows,
        face->glyph->bitmap_left,
        face->glyph->bitmap_top,
        face->glyph->advance.x // 转换为像素
    };

    characters[unicode] = character;
    return true;
}
