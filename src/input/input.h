//
//  input.hpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/3/2.
//

#ifndef input_hpp
#define input_hpp

// 鼠标键盘管理的单例类
class InputProcessMng {
public:
    static InputProcessMng& getInstance();
    InputProcessMng(const InputProcessMng&) = delete;
    InputProcessMng& operator=(const InputProcessMng&) = delete;

    void setFrameBufferSize(int w, int h);
    
#pragma region mouse_event
    // 左键up
    void onMouseLeftUp(double x, double y);

    void onMouseRightDown(double x, double y);

    // 滚轮滚动
    void onMouseMiddleScroll(double xOffset, double yOffset);

    // 滚轮点击
    void onMouseMiddleDown(double x, double y);
    
    // 鼠标移动
    void onMouseMove(double x, double y);
#pragma endregion

#pragma region keyboard_event
    void onKeyRelease_C();
#pragma endregion

private:
    InputProcessMng() {}
    ~InputProcessMng() {}
private:
    int _frameWidth = 0;
    int _frameHeight = 0;
};

#endif /* input_hpp */
