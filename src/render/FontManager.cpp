#include "log/LogMacros.h"
#include "FontManager.h"
#include "fs/FilePathUtil.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

FontManager::FontManager() {
    if (FT_Init_FreeType(&ft)) {
        throw std::runtime_error("ERROR::FREETYPE: Could not init FreeType library");
    }
}

FontManager::~FontManager() {
    if (face) {   // 未加载字体时（如 VK 模式）face 为空，不能调 FT_Done_Face
        FT_Done_Face(face);
    }
    FT_Done_FreeType(ft);
}

bool FontManager::LoadFont(const std::string& fontPath, unsigned int fontSize) {
    // 加载字体。A4: UTF-8 路径读入内存后 FT_New_Memory_Face
    //（Windows 上 FT_New_Face 的 char 路径按 ACP 解释，中文路径失败；
    //  缓冲与 face 同生命周期，见 FontManager.h 说明）
    if (!engine::fs::readFileBinary(engine::fs::pathFromUtf8(fontPath), _fontFileData)
        || _fontFileData.empty()) {
        ENGINE_LOG_ERROR("FREETYPE: Failed to read font: %s", fontPath.c_str());
        return false;
    }
    if (FT_New_Memory_Face(ft, _fontFileData.data(),
                           static_cast<FT_Long>(_fontFileData.size()), 0, &face)) {
        ENGINE_LOG_ERROR("FREETYPE: Failed to load font: %s", fontPath.c_str());
        return false;
    }

    // 设置字体大小
    if (FT_Set_Pixel_Sizes(face, 0, fontSize)) {
        ENGINE_LOG_ERROR("FREETYPE: Failed to set pixel size");
        return false;
    }
    return true;
}

// ==================== D4: SDF 图集 ====================
namespace {

// Felzenszwalb 1D 欧氏距离变换（精确 EDT，标准实现）：f 内 0/INF_SQ，
// 输出每格到最近 0 格的欧氏距离平方。行/列各跑一遍即得 2D 距离场。
// INF 抛物线与任何有限抛物线相交于极右，标准 INF 哨兵行为成立（int64 无溢出）。
constexpr int64_t INF_SQ = 1 << 30;

void edt1d(const std::vector<int64_t>& f, std::vector<int64_t>& d) {
    const size_t n = f.size();
    std::vector<size_t> v(n, 0);       // 抛物线下标栈
    std::vector<int64_t> z(n + 1, 0);  // 抛物线作用右边界
    size_t k = 0;
    z[0] = -INF_SQ;
    z[1] = INF_SQ;
    for (size_t qi = 1; qi < n; ++qi) {
        // 注意：全部算术必须显式 int64_t——混入 size_t（无符号）会把整个
        // 表达式提升为 uint64，负数截断为巨大正数，抛物线求交全错
        const int64_t q  = static_cast<int64_t>(qi);
        const int64_t vk = static_cast<int64_t>(v[k]);
        int64_t s = ((f[qi] + q * q) - (f[v[k]] + vk * vk)) / (2 * q - 2 * vk);
        while (s <= z[k]) {
            --k;
            const int64_t vk2 = static_cast<int64_t>(v[k]);
            s = ((f[qi] + q * q) - (f[v[k]] + vk2 * vk2)) / (2 * q - 2 * vk2);
        }
        ++k;
        v[k] = qi;
        z[k] = s;
        z[k + 1] = INF_SQ;
    }
    k = 0;
    for (size_t qi = 0; qi < n; ++qi) {
        const int64_t q = static_cast<int64_t>(qi);
        while (z[k + 1] < q) ++k;
        const int64_t dv = q - static_cast<int64_t>(v[k]);
        d[qi] = dv * dv + f[v[k]];
    }
}

// 2D EDT（行/列两趟 1D）
void edt2d(const std::vector<uint8_t>& bin, int w, int h,
           std::vector<int64_t>& dist) {
    dist.assign(static_cast<size_t>(w) * h, INF_SQ);
    // 行方向：bin=1 处距离 0
    std::vector<int64_t> f(std::max(w, h)), d(std::max(w, h));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) f[x] = bin[y * w + x] ? 0 : INF_SQ;
        edt1d(f, d);
        for (int x = 0; x < w; ++x) dist[y * w + x] = d[x];
    }
    // 列方向（对上一趟结果再做 1D）
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) f[y] = dist[y * w + x];
        edt1d(f, d);
        for (int y = 0; y < h; ++y) dist[y * w + x] = d[y];
    }
}

} // namespace

bool FontManager::buildSdfAtlas() {
    if (_sdfBuilt) return !_sdfAtlas.empty();
    _sdfBuilt = true;
    if (!face) {
        ENGINE_LOG_WARN("[SDF] no font loaded; text atlas unavailable");
        return false;
    }

    // 图集参数：ASCII 可打印 95 个，10×10 槽位、单元 48px（源 40px + 8px 边距）
    constexpr uint32_t FIRST = 32, LAST = 126;
    constexpr uint32_t COLS = 10, ROWS = 10, CELL = 48;
    constexpr int SPREAD = 8;   // 距离归一化半径（源像素）
    _sdfFontPx = 40;
    _sdfAtlasW = COLS * CELL;
    _sdfAtlasH = ROWS * CELL;
    _sdfAtlas.assign(static_cast<size_t>(_sdfAtlasW) * _sdfAtlasH, 0);   // 0 = 远离字形

    FT_Set_Pixel_Sizes(face, 0, _sdfFontPx);
    std::vector<uint8_t> bin;
    std::vector<int64_t> din, dout;
    uint32_t placed = 0;
    for (uint32_t c = FIRST; c <= LAST; ++c) {
        if (FT_Load_Char(face, c, FT_LOAD_RENDER)) continue;
        const FT_Bitmap& bm = face->glyph->bitmap;
        const int bw = static_cast<int>(bm.width);
        const int bh = static_cast<int>(bm.rows);
        if (bw <= 0 || bh <= 0) continue;
        if (bw > static_cast<int>(CELL) || bh > static_cast<int>(CELL)) {
            ENGINE_LOG_WARN_ONCE("[SDF] glyph %u exceeds cell (%dx%d), skipped", c, bw, bh);
            continue;
        }
        // 二值化（coverage ≥ 128 = 内部）
        bin.assign(static_cast<size_t>(bw) * bh, 0);
        for (int y = 0; y < bh; ++y) {
            for (int x = 0; x < bw; ++x) {
                const uint8_t cov = bm.buffer[y * bm.pitch + x];
                bin[y * bw + x] = (cov >= 128) ? 1 : 0;
            }
        }
        // 内/外两套距离场（pad 1px 避免 EDT 全 INF）
        const int pw = bw + 2, ph = bh + 2;
        std::vector<uint8_t> insideBin(static_cast<size_t>(pw) * ph, 0);
        std::vector<uint8_t> outsideBin(static_cast<size_t>(pw) * ph, 1);
        for (int y = 0; y < bh; ++y) {
            for (int x = 0; x < bw; ++x) {
                const uint8_t b = bin[y * bw + x];
                insideBin[(y + 1) * pw + (x + 1)] = b;
                outsideBin[(y + 1) * pw + (x + 1)] = b ? 0 : 1;
            }
        }
        edt2d(insideBin, pw, ph, din);
        edt2d(outsideBin, pw, ph, dout);

        // 槽位
        const uint32_t idx = placed++;
        const uint32_t cx = (idx % COLS) * CELL;
        const uint32_t cy = (idx / COLS) * CELL;
        // 写 SDF：sdf = 0.5 + 0.5*(dOut - dIn)/SPREAD，clamp 到 [0,1]，
        // 只写字形包围盒 ±pad 范围（其余槽位保持 0）
        for (int y = -1; y < bh + 1; ++y) {
            for (int x = -1; x < bw + 1; ++x) {
                const int ax = cx + 1 + x;   // 槽内 +1 对齐 pad 网格
                const int ay = cy + 1 + y;
                if (ax < 0 || ay < 0 || ax >= static_cast<int>(_sdfAtlasW)
                    || ay >= static_cast<int>(_sdfAtlasH)) continue;
                const size_t pi = static_cast<size_t>(y + 1) * pw + (x + 1);
                const double dOut = std::sqrt(static_cast<double>(dout[pi] >= INF_SQ ? INF_SQ : dout[pi]));
                const double dIn = std::sqrt(static_cast<double>(din[pi] >= INF_SQ ? INF_SQ : din[pi]));
                double sdf = 0.5 + 0.5 * (dOut - dIn) / static_cast<double>(SPREAD);
                sdf = std::max(0.0, std::min(1.0, sdf));
                _sdfAtlas[static_cast<size_t>(ay) * _sdfAtlasW + ax] =
                    static_cast<uint8_t>(sdf * 255.0 + 0.5);
            }
        }
        SdfGlyph g;
        g.u0 = static_cast<uint16_t>(cx + 1);
        g.v0 = static_cast<uint16_t>(cy + 1);
        g.u1 = static_cast<uint16_t>(cx + 1 + bw);
        g.v1 = static_cast<uint16_t>(cy + 1 + bh);
        g.bearingX = static_cast<int16_t>(face->glyph->bitmap_left);
        g.bearingY = static_cast<int16_t>(face->glyph->bitmap_top);
        g.width  = static_cast<uint16_t>(bw);
        g.height = static_cast<uint16_t>(bh);
        g.advance = static_cast<uint16_t>(face->glyph->advance.x >> 6);
        _sdfGlyphs[c] = g;
    }
    ENGINE_LOG_INFO("[SDF] atlas %ux%u built, glyphs=%zu", _sdfAtlasW, _sdfAtlasH, placed);
    return placed > 0;
}

const SdfGlyph* FontManager::getSdfGlyph(uint32_t unicode) {
    if (!_sdfBuilt) buildSdfAtlas();
    auto it = _sdfGlyphs.find(unicode);
    return it != _sdfGlyphs.end() ? &it->second : nullptr;
}
