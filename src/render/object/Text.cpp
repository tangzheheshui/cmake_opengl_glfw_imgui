

#include "Text.h"
#include "shader.h"
#include "camera_old.h"
#include "image.h"
#include "FontManager.h"

CText::CText(std::weak_ptr<FontManager> fontMng) {
    _fontMng = fontMng;

    glBindVertexArray(_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, _VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 30, nullptr, GL_DYNAMIC_DRAW);  // 先分配空间

    // 位置属性 (vec3)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);

    // 纹理坐标 (vec2)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

bool CText::draw(const Matrix &mtx) {
    auto pFontMng = _fontMng.lock();
    if (!pFontMng) {
        return false;
    }

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

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float x = 0;
    float y = 0;
    float scale = 0.5f;
    // 遍历字符串并绘制每个字符
    for (char c : m_str) {
        auto ch = pFontMng->GetCharacter(c);  // 获取字符的字形信息
        
        float xpos = x + ch->bearingX * scale;
        float ypos = y - (ch->height - ch->bearingY) * scale;

        float w = ch->width * scale;
        float h = ch->height * scale;

        // 设置当前字符的顶点数据
        float z = 0.5;
        float vertices[6][5] = {
            { xpos,     ypos + h, z, 0.0f, 0.0f }, 
            { xpos + w, ypos,     z, 1.0f, 1.0f },
            { xpos,     ypos,     z, 0.0f, 1.0f },

            { xpos,     ypos + h, z, 0.0f, 0.0f },
            { xpos + w, ypos + h, z, 1.0f, 0.0f },
            { xpos + w, ypos,     z, 1.0f, 1.0f }
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

    return true;
}

void CText::SetText(const std::string &str) {
    m_str = str;
}
