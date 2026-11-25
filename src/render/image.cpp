//
//  image.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/1.
//

#include "log/LogMacros.h"
#include "image.h"
#include "fs/FilePathUtil.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <array>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

TextureMng::TextureMng() {
}

void TextureMng::loadTexture(const std::string &filename) {
    if (filename.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> gurad(m_mutex);
        auto iter = m_map_tex_data.find(filename);
        if (iter != m_map_tex_data.end()) {
            return;
        }
    }

    Texture tex;
    // A4: UTF-8 路径读入内存后解码（Windows 宽路径安全），顺带修复
    // 原实现在文件打开失败时对空指针 assign 的隐患
    std::vector<unsigned char> fileData;
    if (!engine::fs::readFileBinary(engine::fs::pathFromUtf8(filename), fileData)) {
        ENGINE_LOG_WARN("TextureMng: cannot open texture file: %s", filename.c_str());
        return;
    }
    unsigned char *data = stbi_load_from_memory(fileData.data(),
                                                static_cast<int>(fileData.size()),
                                                &tex.width, &tex.height, &tex.format, 0);
    if (!data) {
        ENGINE_LOG_WARN("TextureMng: cannot decode texture file: %s", filename.c_str());
        return;
    }
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
