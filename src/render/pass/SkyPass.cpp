#include "SkyPass.h"
#include "../scene.h"
#include "../object/Sky.h"

void CSkyPass::draw(Scene* pScene) {
    if (!pScene) {
        return;
    }
    
    if (auto sky = pScene->GetSkyBox()) {
        sky->draw();
    }
}
