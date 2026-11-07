#pragma once
// 电源键 / 音量键：这两个键不在手柄设备上（Switch 上是独立的 gpio-keys 与
// max77620_onoff），所以另开一份只读 fd，不走 EvdevGamepad。
#include <functional>
#include <string>
#include <vector>

namespace input {

// 只列我们认识的系统键；顺序与 PadButton 末尾那三个一致，方便照序转成 keymap 的下标
enum SysKey { kSysVolDown = 0, kSysVolUp, kSysPower };

// 纯映射：evdev 键码 → SysKey（114 音量− / 115 音量+ / 142 电源），其它 → -1。
// 单独抽出来是为了自检不依赖真机。
int sysKeyFromCode(int evdevCode);

class EvdevSysKeys {
public:
    ~EvdevSysKeys() { close(); }

    // 打开所有「报了音量/电源键」的 /dev/input/event*；手柄设备跳过（那些由 EvdevGamepad 读）
    int open();
    // 只读不抢独占（不开 EVIOCGRAB）：logind 还要看这些设备
    void poll(const std::function<void(int key, bool pressed)>& h);
    void close();
    int count() const { return int(fds_.size()); }
    // 掉线（换了设备节点）：上层在下次重扫时机整组重开
    bool lost() const { return lost_; }
    const std::vector<std::string>& names() const { return names_; }

private:
    std::vector<int> fds_;
    std::vector<std::string> names_;
    bool lost_ = false;
};

}  // namespace input
