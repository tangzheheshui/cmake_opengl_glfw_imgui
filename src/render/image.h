//
//  image.hpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/1.
//

#ifndef image_hpp
#define image_hpp

#include <string>
#include <mutex>
#include <map>
#include "BaseDraw.h"
#include "shader.h"

static const std::string STR_DEPTH_TEXTURE = "depth_texture";
static const std::string STR_PICK_TEXTURE = "pick_texture";
typedef std::shared_ptr<std::vector<unsigned char>> ImageBuffer;

struct Texture {
    std::string name; // 直接用作uniform纹理的名�?
    std::string filepath;
    ImageBuffer data;
    int width = 0;
    int height = 0;
    int format = 0;
};

class TextureMng {
public:
    static TextureMng& getInstance() {
        static TextureMng instance;
        return instance;
    }
    void setRootPath(const std::string &path) { m_root_path = path; }
    void loadTexture(const std::string &filename);
    // srgbColor=true：颜色贴图（diffuse/specular/天空盒等），用 sRGB 内部格式
    // 让硬件在采样时解码到线性空间；法线/高度/遮罩等数据贴图传 false。
    // 同一文件以两种色彩空间使用时分别缓存。
    unsigned int getTexture(const std::string &filename, bool srgbColor = false);
    // 取已预载纹理的原始数据（不擦除，供 RHI 后端创建自己的纹理对象）
    ImageBuffer getTextureData(const std::string &filename, int &width, int &height, int &channels);
    unsigned int getCubeTexture(std::array<std::string, 6> faces);
private:
    TextureMng();
    ~TextureMng() {}
    TextureMng(const TextureMng& mng) = delete;
    TextureMng& operator = (const TextureMng& mng) = delete;
    TextureMng(TextureMng&& mng) = delete;
    TextureMng& operator = (TextureMng&& mng) = delete;
private:
    void createShadowTexture();
    void createSkyTexture();
    unsigned int genTexture(const Texture& tex, bool srgbColor);
private:
    std::map<std::string, unsigned int> m_map_textures;
    std::map<std::string, Texture> m_map_tex_data;
    mutable std::mutex m_mutex;
    std::string m_root_path;
};

#endif /* image_hpp */
