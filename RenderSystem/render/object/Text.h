//
//  Sky.hpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/4/7.
//

#pragma once

#include <iostream>
#include <string>
#include <array>

#include "object/BaseDraw.h"
#include <ft2build.h>
#include FT_FREETYPE_H

class FontManager;

class CText : public BaseDraw {
public:
    CText(std::weak_ptr<FontManager> fontMng);
    ~CText() {}
    virtual bool draw(CameraPtr camera) override;
    void SetText(const std::string &str);
private:
    FT_Library ft;
    FT_Face face;
    std::weak_ptr<FontManager> _fontMng;
    std::string m_str;
};
