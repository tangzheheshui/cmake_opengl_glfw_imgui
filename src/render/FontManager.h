#ifndef FONT_MANAGER_H
#define FONT_MANAGER_H

#include <ft2build.h>
#include FT_FREETYPE_H
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>
#include <cstdint>

// ---- D4: SDF 文字图集（RHI 双后端路径）----
// 有符号距离场：采样值 0.5 = 字形边缘（<0.5 外、>0.5 内），片元用
// smoothstep(0.5-w, 0.5+w, d) 抗锯齿，w 随屏幕像素密度自适应 → 任意缩放清晰。
struct SdfGlyph {
    uint16_t u0, v0, u1, v1;   // 图集像素矩形（左上/右下）
    int16_t  bearingX, bearingY;
    uint16_t width, height;    // 字形绘制尺寸（图集像素 = 世界像素基准）
    uint16_t advance;          // 前进宽度（像素）
};

class FontManager {
public:
    FontManager();
    ~FontManager();

    bool LoadFont(const std::string& fontPath, unsigned int fontSize);

    // ---- D4: SDF 图集（ASCII 可打印区；惰性构建一次）----
    // 未 LoadFont 或字体缺失时返回 nullptr。图集数据 R8 单通道，
    // 尺寸 atlasWidth() × atlasHeight()。
    const SdfGlyph* getSdfGlyph(uint32_t unicode);
    const std::vector<uint8_t>& sdfAtlasData() const { return _sdfAtlas; }
    uint32_t sdfAtlasWidth()  const { return _sdfAtlasW; }
    uint32_t sdfAtlasHeight() const { return _sdfAtlasH; }
    // 基准像素尺寸（字形 metric 以此为单位，绘制端按需缩放）
    uint32_t sdfFontPixelSize() const { return _sdfFontPx; }

private:
    FT_Library ft;               // FreeType 库对象
    FT_Face face = nullptr;      // 字体面（每个字体文件对应一个 face）。
                                 // 必须初始化为空：VK 模式不加载引擎字体时，
                                 // ~FontManager 若对未初始化指针调 FT_Done_Face 会段错误
                                 //（Windows 实机退出崩溃的根因，B3 验证期修复）
    // A4: 字体文件字节。FT_New_Memory_Face 不拷贝数据，缓冲必须与 face
    // 同生命周期（face 在析构时释放，故数据随对象常驻）
    std::vector<unsigned char> _fontFileData;

    // ---- D4: SDF 图集构建 ----
    bool buildSdfAtlas();
    bool _sdfBuilt = false;
    std::unordered_map<uint32_t, SdfGlyph> _sdfGlyphs;
    std::vector<uint8_t> _sdfAtlas;
    uint32_t _sdfAtlasW = 0;
    uint32_t _sdfAtlasH = 0;
    uint32_t _sdfFontPx = 40;    // 源栅格像素（bearing/advance 以此为单位）
};

#endif // FONT_MANAGER_H
