#include "NormalPass.h"
#include "../scene.h"
#include "../object/ImageRectangle.h"
#include "../object/Line.h"
#include "../object/Sky.h"
#include "../model/model.h"

const unsigned int SCR_WIDTH = 800;
const unsigned int SCR_HEIGHT = 600;

void CNormalPass::draw(Scene* pScene) {
    if (!pScene) {
        return;
    }
    
    glViewport(0, 0, SCR_WIDTH*2, SCR_HEIGHT*2);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
    glDisable(GL_BLEND);
    
    for (auto obj : pScene->GetObjs()) {
        obj->draw();
    }
}
