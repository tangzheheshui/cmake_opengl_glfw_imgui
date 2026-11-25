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
#include <array>

// 去后端化：本文件只负责「磁盘图片 → 内存解码缓存」。GL 上传已删
// （两端统一走 RHI createTexture），GPU 侧资源由各绘制对象自持。

typedef std::shared_ptr<std::vector<unsigned char>> ImageBuffer;

struct Texture {
    std::string name; // 直接用作uniform纹理的名字
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
    // 取已预载纹理的原始数据（不擦除，供 RHI 后端创建自己的纹理对象）
    ImageBuffer getTextureData(const std::string &filename, int &width, int &height, int &channels);
private:
    TextureMng();
    ~TextureMng() {}
    TextureMng(const TextureMng& mng) = delete;
    TextureMng& operator = (const TextureMng& mng) = delete;
    TextureMng(TextureMng&& mng) = delete;
    TextureMng& operator = (TextureMng&& mng) = delete;
private:
    std::map<std::string, Texture> m_map_tex_data;
    mutable std::mutex m_mutex;
    std::string m_root_path;
};

#endif /* image_hpp */
