#pragma once
// 终端输出里的应用层标记：OSC 1337 newtab/quit + 标准标题序列（OSC 0/1/2）。
// 纯函数，不认 PTY/UI；跨 read 的残余由调用方持有。
#include <string>
#include <vector>

namespace term {

struct OscEvent {
    enum Kind { Title, NewTab, Quit } kind;
    std::string arg;  // Title：标题文本（已截到 10 个字符）；NewTab：名字（可空）
};

// pend 是调用方持有的跨 read 残余；返回应喂给 vterm 的字节（认识的序列被剥掉，其余原样）
std::string oscFilter(std::string& pend, const char* data, size_t n, std::vector<OscEvent>* out);

}  // namespace term
