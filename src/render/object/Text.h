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

#include "../BaseDraw.h"

class FontManager;

class CText : public BaseDraw {
public:
    CText(std::weak_ptr<FontManager> fontMng);
    ~CText() {}
    virtual bool draw(const Matrix &mtx) override;
    void SetText(const std::string &str);
private:
    std::weak_ptr<FontManager> _fontMng;
    std::string m_str;
};
