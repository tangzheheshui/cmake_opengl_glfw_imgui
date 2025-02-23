

#include "Text.h"
#include "shader.h"
#include "camera_old.h"
#include "image.h"
#include "FontManager.h"

CText::CText(std::weak_ptr<FontManager> fontMng) {
    _fontMng = fontMng;
}

bool CText::draw(const Matrix &mtx) {
    auto shader = ShaderCache::GetInstance().GetShader(ShaderType::Text);
    if (!shader) {
        return false;
    }
    
    shader->use();
    // 矩阵
    shader->setMat4("uMatrixMVP", mtx);
    
    // 颜色
    shader->setFloat3("uColor", 1, 0, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(_VAO);

    auto pFontMng = _fontMng.lock();
    if (!pFontMng)
    {
        return false;
    }
    
    float x = 0;
    float y = 0;
    float scale = 3.f;
    // 遍历字符串并绘制每个字符
    for (char c : m_str) {
        auto ch = pFontMng->GetCharacter(c);  // 获取字符的字形信息
        
        float xpos = x + ch->bearingX * scale;
        float ypos = y - (ch->height - ch->bearingY) * scale;

        float w = ch->width * scale;
        float h = ch->height * scale;

        // 设置当前字符的顶点数据
        float vertices[6][4] = {
            { xpos,     ypos + h,   0.0f, 0.0f }, 
            { xpos + w, ypos,       1.0f, 1.0f }, 
            { xpos,     ypos,       0.0f, 1.0f },

            { xpos,     ypos + h,   0.0f, 0.0f }, 
            { xpos + w, ypos + h,   1.0f, 0.0f }, 
            { xpos + w, ypos,       1.0f, 1.0f }
        };

        // 更新VBO数据
        glBindBuffer(GL_ARRAY_BUFFER, _VBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        // 绑定字符的纹理并绘制
        glBindTexture(GL_TEXTURE_2D, ch->textureID);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // 更新下一个字符的x位置
        x += (ch->advance >> 6) * scale;  // 以像素为单位更新位置
    }

    glBindVertexArray(0);
    return false;
}

void CText::SetText(const std::string &str) {
    m_str = str;
}
