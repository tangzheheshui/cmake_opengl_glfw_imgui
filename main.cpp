#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "3rdparty/imgui/imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <iostream>
#include <filesystem>
#include "camera/camera_old.h"
#include "RenderSystem.h"
#include "scene.h"
#include "object/Line.h"
#include "object/Sky.h"
#include "object/Text.h"
#include "object/ImageRectangle.h"
#include "model/model.h"
#include "Light.h"
#include "taskQueue.h"
#include "FontManager.h"

void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void mouse_callback(GLFWwindow* window, double xpos, double ypos);
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void processInput(GLFWwindow* window);
void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
void testRenderSystem(const std::string &rootPath);
// settings
const unsigned int SCR_WIDTH = 800;
const unsigned int SCR_HEIGHT = 500;

// camera
glm::vec3 cameraPos = glm::vec3(0.0f, 0.0f, 3.0f);
glm::vec3 cameraFront = glm::vec3(0.0f, 0.0f, -1.0f);
glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);

bool firstMouse = true;
float yaw = -90.0f;    // yaw is initialized to -90.0 degrees since a yaw of 0.0 results in a direction vector pointing to the right so we initially rotate a bit to the left.
float pitch = 0.0f;
float lastX = 800.0f / 2.0;
float lastY = 600.0 / 2.0;
float fov = 45.0f;

int main()
{
    // glfw: initialize and configure
    // ------------------------------
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    // glfw window creation
    // --------------------
    GLFWwindow* window = glfwCreateWindow(SCR_WIDTH, SCR_HEIGHT, "LearnOpenGL", NULL, NULL);
    if (window == NULL)
    {
        std::cout << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetKeyCallback(window, key_callback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    //glfwSetInputMode(window, GLFW_CURSOR_HIDDEN, GLFW_CURSOR_HIDDEN);
    //glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    //InputProcessMng::getInstance().setFrameBufferSize(width, height);

    // glad: load all OpenGL function pointers
    // ---------------------------------------
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        std::cout << "Failed to initialize GLAD" << std::endl;
        return -1;
    }    
    
    // ��ʼ�� ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();

    // ��ʼ�� ImGui ƽ̨/��Ⱦ����
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // ��ȡ��Ŀ¼
    std::filesystem::path current_path = std::filesystem::current_path();
    std::string proPath = current_path.string();
    char sep = std::filesystem::path::preferred_separator;
    std::string buildFolder = std::string(1, sep) + "build";
    size_t pos =  proPath.find(buildFolder);
    proPath = proPath.substr(0, pos);
    // ��ʼ����Ⱦ����
    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    RenderSystem::getInstance().init(proPath);
    RenderSystem::getInstance().onWindowSizeChanged(width, height);

    // 使用rendersysytem的各种接口
    testRenderSystem(proPath);
    while (!glfwWindowShouldClose(window))
    {
        // input
        processInput(window);

        RenderSystem::getInstance().update();
        RenderSystem::getInstance().draw();

        {
            // �����µ� ImGui ֡
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            // ʾ�� UI
            ImGui::Begin("Hello, world!");
            ImGui::Text("This is some useful text.");
            ImGui::End();

            // ��Ⱦ ImGui
            ImGui::Render();
        }

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // glfw: swap buffers and poll IO events (keys pressed/released, mouse moved etc.)
        // -------------------------------------------------------------------------------
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // ����
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    // glfw: terminate, clearing all previously allocated GLFW resources.
    // ------------------------------------------------------------------
    glfwTerminate();
    return 0;
}

// process all input: query GLFW whether relevant keys are pressed/released this frame and react accordingly
// ---------------------------------------------------------------------------------------------------------
void processInput(GLFWwindow *window)
{
    if(glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window, true);
}

// glfw: whenever the window size changed (by OS or user resize) this callback function executes
// ---------------------------------------------------------------------------------------------
void framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    // make sure the viewport matches the new window dimensions; note that width and 
    // height will be significantly larger than specified on retina displays.
    RenderSystem::getInstance().onWindowSizeChanged(width, height);
    
}

void mouse_callback(GLFWwindow* window, double xposIn, double yposIn)
{
    RenderSystem::getInstance().onMouseMove(xposIn, yposIn);
}

// glfw: whenever the mouse scroll wheel scrolls, this callback is called
// ----------------------------------------------------------------------
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
{
    RenderSystem::getInstance().onMouseMiddleScroll(xoffset, yoffset);
}

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    if (action == GLFW_PRESS)
    {
        //std::cout << "Key pressed: " << key << std::endl;
    }
    else if (action == GLFW_RELEASE)
    {
        if (GLFW_KEY_C == key)
        {
            //RenderSystem::getInstance().onKeyRelease_C();
        }
    }
}

// ������ص�����
void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    double xpos, ypos;
    glfwGetCursorPos(window, &xpos, &ypos);

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_RELEASE)
        {
            RenderSystem::getInstance().onMouseLeftUp(xpos, ypos);
        }
        else if (action == GLFW_PRESS)
        {
            RenderSystem::getInstance().onMouseLeftDown(xpos, ypos);
        }
    }

    else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        if (action == GLFW_RELEASE)
        {
            RenderSystem::getInstance().onMouseRightUp(xpos, ypos);
        }
        else if (action == GLFW_PRESS)
        {
            RenderSystem::getInstance().onMouseRightDown(xpos, ypos);
        }
    }
    else if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        if (action == GLFW_PRESS)
        {
            RenderSystem::getInstance().onMouseMiddleDown(xpos, ypos);
        }
    }
}

void testRenderSystem(const std::string &rootPath) {
    auto pScene = RenderSystem::getInstance().GetCurScene();
    if (!pScene)
    {
        return;
    }
    
    // 绘制坐标轴
    std::vector<unsigned int> indices = {0, 1};
    
    float len = 15;
    glm::vec3 pZero = {0, 0, 0};
    glm::vec3 pX = {len, 0, 0};
    glm::vec3 pY = {0, len, 0};
    glm::vec3 pZ = {0, 0, len};
    // 3个轴
    std::shared_ptr<Line> line_x = std::make_shared<Line>();
    std::shared_ptr<Line> line_y = std::make_shared<Line>();
    std::shared_ptr<Line> line_z = std::make_shared<Line>();
    line_x->setData({pZero, pX}, indices);
    line_y->setData({pZero, pY}, indices);
    line_z->setData({pZero, pZ}, indices);
    
    // 颜色
    line_x->setColor({1, 0, 0});
    line_y->setColor({0, 1, 0});
    line_z->setColor({0, 0, 1});
    
    auto start = std::chrono::high_resolution_clock::now();
    // 地面
    std::shared_ptr<ImageRectangle> objGround = std::make_shared<ImageRectangle>();
    float ground_width = 10;
    auto texPath = std::filesystem::path(rootPath) / "res" / "textures";
    objGround->setImagePath((texPath / "bricks2.jpg").string(),
        (texPath / "bricks2_normal.jpg").string(),
        (texPath / "bricks2_disp.jpg").string());
    
    objGround->setSetp(5, 5);
    glm::vec3 p1(-ground_width, 0,  ground_width);
    glm::vec3 p2(ground_width, 0,  ground_width);
    glm::vec3 p3(ground_width, 0, -ground_width);
    glm::vec3 p4(-ground_width, 0, -ground_width);
    objGround->setPoints(p1, p2, p3, p4);
    objGround->calculate();
    
    // 鸭子
    std::shared_ptr<Model> objDuck = std::make_shared<Model>();
    objDuck->LoadFile((std::filesystem::path(rootPath) / "res" / "model" / "duck.dae").string());
    TaskQueue::instance().pushTask([start](){
    }); 
    
    objDuck->setCount(4);
    objDuck->setPosition(0, {0, 0, 2});
    objDuck->setScale(0, 0.01);
    objDuck->setPosition(1, {-2, 0, 2});
    objDuck->setScale(1, 0.01);
    objDuck->setPosition(2, {2, 0, 2});
    objDuck->setScale(2, 0.01);
    objDuck->setPosition(3, {0, 0, -2});
    objDuck->setScale(3, 0.01);
    objDuck->setRotateY(3, 90);
    
    // 光源模型
    std::shared_ptr<Model> objLight = std::make_shared<Model>();
    objLight->LoadFile((std::filesystem::path(rootPath) / "res" / "model" / "duck.dae").string());
    objLight->setCount(1);
    objLight->setLightOpen(false);
    auto lightPos = Light::GlobalLight().position;
    objLight->setPosition(0, {lightPos.x, lightPos.y, lightPos.z});
    objLight->setScale(0, 0.5);
    
    //
    auto skyboxPath = texPath / "skybox";
    std::shared_ptr<Sky> objSky = std::make_shared<Sky>();
    objSky->setCubeImage({
        (skyboxPath / "right.jpg").string(),
        (skyboxPath / "left.jpg").string(),
        (skyboxPath / "top.jpg").string(),
        (skyboxPath / "bottom.jpg").string(),
        (skyboxPath / "front.jpg").string(),
        (skyboxPath / "back.jpg").string()
    });
    
    // obj
    pScene->AddObj(objGround);
    pScene->AddObj(objGround);
    pScene->AddObj(objDuck);
    pScene->AddObj(objLight);
    pScene->AddObj(line_x);
    pScene->AddObj(line_y);
    pScene->AddObj(line_z);
    
    // 天空盒
    pScene->SetSkyBox(objSky);

    // 文字
    auto& renderSys = RenderSystem::getInstance();
    auto fontMng = renderSys.GetFontMng();
    std::shared_ptr<CText> objText = std::make_shared<CText>(std::weak_ptr<FontManager>(fontMng));
    objText->SetText("test");
    pScene->AddObj(objText);
}
