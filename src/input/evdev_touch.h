#pragma once
// evdev 触摸屏。原来这套逻辑焊在 fbdev 后端里，X 下走 SDL 就没有触屏；
// 搬到输入层后 fbdev 与 SDL 共用同一份，app 每帧取事件（跟 evdev_gamepad/evdev_keys 同层次）。
#include <linux/input.h>

#include <string>
#include <vector>

#include "platform/display_backend.h"

namespace input {

class EvdevTouch {
public:
    ~EvdevTouch() { close(); }

    // 打开第一台报了 ABS_MT_POSITION_X/Y 的 /dev/input/event*；返回打开的 fd 数（0/1）
    int open();
    void close();
    int count() const { return fd_ >= 0 ? 1 : 0; }
    // 掉线（read 返回 ENODEV）：上层下次重扫时机整组重开
    bool lost() const { return lost_; }
    const std::string& name() const { return name_; }
    // engine.display.rotate：面板原生竖装时触摸坐标要跟着转（与后端 setRotation 同值）
    void setRotation(int deg) { rot_ = ((deg % 360) + 360) % 360; }

    // 读一帧事件：clicks 追加轻点（画布坐标），wheel 累加翻页行数（正 = 向上回看）。
    // canvasW/canvasH = 画布尺寸（跟后端 setCanvasSize 同值），触摸坐标换算到它上面
    void pump(std::vector<disp::ClickEvent>& clicks, int& wheel, int canvasW, int canvasH);

private:
    disp::Point point(int canvasW, int canvasH) const;
    static bool bitSet(const unsigned long* bits, int bit);

    int fd_ = -1;
    std::string name_;
    input_absinfo xInfo_{}, yInfo_{};
    int x_ = 0, y_ = 0, remainder_ = 0;
    bool active_ = false, ending_ = false, havePoint_ = false, moved_ = false, lost_ = false;
    disp::Point start_{}, last_{};
    int rot_ = 0;
};

}  // namespace input
