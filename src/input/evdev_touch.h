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

    // 打开所有报了 ABS_MT_POSITION_X/Y 的 /dev/input/event*；返回打开的台数。
    // 每台带自己的 abs 量程（换算要按来源那台的），触摸手势仍按单点处理
    int open();
    void close();
    int count() const { return int(devs_.size()); }
    // 掉线（read 返回 ENODEV）：上层下次重扫时机整组重开
    bool lost() const { return lost_; }
    const std::string& name() const { return devs_.empty() ? emptyName_ : devs_[0].name; }
    // engine.display.rotate：面板原生竖装时触摸坐标要跟着转（与后端 setRotation 同值）
    void setRotation(int deg) { rot_ = ((deg % 360) + 360) % 360; }

    // 读一帧事件：clicks 追加轻点（画布坐标），wheel 累加翻页行数（正 = 向上回看）。
    // canvasW/canvasH = 画布尺寸（跟后端 setCanvasSize 同值），触摸坐标换算到它上面
    void pump(std::vector<disp::ClickEvent>& clicks, int& wheel, int canvasW, int canvasH);

private:
    disp::Point point(int canvasW, int canvasH) const;
    static bool bitSet(const unsigned long* bits, int bit);

    struct Dev {
        int fd = -1;
        std::string name;
        input_absinfo x{}, y{};
    };
    std::vector<Dev> devs_;
    int cur_ = 0;              // 当前这次触摸来自哪台设备（换算用它的量程）
    std::string emptyName_;
    int x_ = 0, y_ = 0, remainder_ = 0;
    bool active_ = false, ending_ = false, havePoint_ = false, moved_ = false, lost_ = false;
    disp::Point start_{}, last_{};
    int rot_ = 0;
};

}  // namespace input
