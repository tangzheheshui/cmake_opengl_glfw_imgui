//
//  SceneDemo.h
//  app 层示例场景组装（C5：场景/模型/资源细节不进引擎）
//
#ifndef SceneDemo_h
#define SceneDemo_h

#include <string>

namespace SceneDemo {
// 组装示例场景：坐标轴/鸭子群/光源模型/天空盒（GL 模式另有地面与文字）。
// rootPath = 项目根（UTF-8）。
void buildScene(const std::string& rootPath, bool useVulkan);
}

#endif /* SceneDemo_h */
