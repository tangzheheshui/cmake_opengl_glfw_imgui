#include "RenderSystem.h"

RenderSystem& RenderSystem::getInstance() {
	static RenderSystem instance;
	return instance;
}

RenderSystem::RenderSystem() {

}

RenderSystem::~RenderSystem() {

}

void RenderSystem::init() {
	// ��ʼ����Ⱦ����
}

void RenderSystem::update() {

}
void RenderSystem::draw() {

}