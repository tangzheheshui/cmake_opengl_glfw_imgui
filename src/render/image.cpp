//
//  image.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/1.
//

#include "log/LogMacros.h"
#include "image.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <array>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

const GLuint SHADOW_WIDTH = 1024, SHADOW_HEIGHT = 1024;

TextureMng::TextureMng() {
    // 注意：不在构造函数里创建 GL 资源——引擎需支持无 GL 上下文的后端（Vulkan），
    // 阴影深度纹理改为惰性创建（getTexture 首次查询时）
}

void TextureMng::loadTexture(const std::string &filename) {
    if (filename.empty()) {
        return;
    }
    
    {
        std::lock_guard<std::mutex> gurad(m_mutex);
        auto iter = m_map_textures.find(filename);
        if (iter != m_map_textures.end()) {
            return;
        }
    }
    
    Texture tex;
    unsigned char *data = stbi_load(filename.c_str(), &tex.width, &tex.height, &tex.format, 0);
    ImageBuffer pData = std::make_shared<std::vector<unsigned char>>();
    pData->assign(data, data + tex.width * tex.height * tex.format);
    tex.data = std::move(pData);
    stbi_image_free(data);
    tex.filepath = filename;
    {
        std::lock_guard<std::mutex> gurad(m_mutex);
        m_map_tex_data[filename] = tex;
        ENGINE_LOG_INFO("add filename = %s", filename.c_str());
    }
}

ImageBuffer TextureMng::getTextureData(const std::string &filename, int &width, int &height, int &channels) {
    std::lock_guard guard(m_mutex);
    auto iter = m_map_tex_data.find(filename);
    if (iter == m_map_tex_data.end()) {
        width = height = channels = 0;
        return nullptr;
    }
    width = iter->second.width;
    height = iter->second.height;
    channels = iter->second.format;
    return iter->second.data;
}

unsigned int TextureMng::getTexture(const std::string &filename, bool srgbColor) {
    // 阴影深度纹理惰性创建（首次使用时才触碰 GL）
    if (filename == STR_DEPTH_TEXTURE && m_map_textures.find(STR_DEPTH_TEXTURE) == m_map_textures.end()) {
        createShadowTexture();
    }
    // 同一图像可能既作颜色贴图又作数据贴图（罕见）：按色彩空间分键缓存
    const std::string cacheKey = srgbColor ? filename + "\x01srgb" : filename;
    std::lock_guard guard(m_mutex);
    auto iter = m_map_textures.find(cacheKey);
    if (iter != m_map_textures.end()) {
        return iter->second;
    }

    auto iter1 = m_map_tex_data.find(filename);
    if (iter1 != m_map_tex_data.end()) {
        auto tex = genTexture(iter1->second, srgbColor);
        
        // M4.2b：不再擦除原始数据——RHI 路径需要用它创建后端纹理对象
        //（原实现用完即删，导致 GL 路径先取数后 RHI 路径拿到空数据）。
        // 内存代价：预载纹理数据常驻（约数十 MB）；淘汰策略 M6 再做。
        return tex;
    }
    return -1;
}

void TextureMng::createShadowTexture() {
    // 创建阴影贴图
    GLuint _depthTexture;
    glGenTextures(1, &_depthTexture);
    glBindTexture(GL_TEXTURE_2D, _depthTexture);
    glTexImage2D(GL_TEXTURE_2D, 
                 0,
                 GL_DEPTH_COMPONENT, 
                 SHADOW_WIDTH, 
                 SHADOW_HEIGHT,
                 0, 
                 GL_DEPTH_COMPONENT, 
                 GL_FLOAT, 
                 NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER); 
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_GREATER);
    GLfloat borderColor[] = { 1.0, 1.0, 1.0, 1.0 };
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    
    std::lock_guard guard(m_mutex);
    m_map_textures[STR_DEPTH_TEXTURE] = _depthTexture;
}

void TextureMng::createSkyTexture() {
    
}

unsigned int TextureMng::genTexture(const Texture& tex, bool srgbColor) {
    unsigned int textureID = -1;
    if (!tex.data->empty()) {
        glGenTextures(1, &textureID);

        GLenum format = 0;
        if (tex.format == 1)
            format = GL_RED;
        else if (tex.format == 3)
            format = GL_RGB;
        else if (tex.format == 4)
            format = GL_RGBA;

        // B3c：颜色贴图用 sRGB 内部格式（采样时硬件解码到线性）。
        // 单通道是数据/遮罩，不参与颜色转换。
        GLenum internalFormat = format;
        if (srgbColor && tex.format >= 3) {
            internalFormat = GL_SRGB8_ALPHA8;
        }

        glBindTexture(GL_TEXTURE_2D, textureID);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, tex.width, tex.height, 0,
                     format, GL_UNSIGNED_BYTE, tex.data->data());
        glGenerateMipmap(GL_TEXTURE_2D);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        const std::string cacheKey = srgbColor ? tex.filepath + "\x01srgb" : tex.filepath;
        m_map_textures[cacheKey] = textureID;
    }
    return textureID;
}

unsigned int TextureMng::getCubeTexture(std::array<std::string, 6> faces) {
    unsigned int textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_CUBE_MAP, textureID);

    int width, height, nrChannels;
    for (unsigned int i = 0; i < faces.size(); i++)
    {
        auto& file = faces[i];
        unsigned char *data = stbi_load(file.c_str(), &width, &height, &nrChannels, 0);
        if (data)
        {
            // B3c：天空盒是颜色贴图，sRGB 内部格式随交换链 sRGB 编码形成往返
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_SRGB8_ALPHA8,
                         width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
            stbi_image_free(data);
        }
        else
        {
            assert(0);
            ENGINE_LOG_ERROR("Cubemap texture failed to load at path: %s", faces[i].c_str());
            stbi_image_free(data);
            glDeleteTextures(1, &textureID);
            return -1;
        }
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    return textureID;
}
