#include "term/osc_filter.h"

namespace term {
namespace {

// 未闭合的序列最多留这么多；超过就放弃识别、原样放行（正常序列远小于此）
constexpr size_t kPendCap = 512;

// 标题按 UTF-8 字符取前 10 个，超出补 …（截断点跳过延续字节，不留半个字）
std::string shortTitle(const std::string& s) {
    size_t i = 0, chars = 0;
    while (i < s.size() && chars < 10) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) ++chars;
        ++i;
    }
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i < s.size() ? s.substr(0, i) + "…" : s;
}

// 认出来的序列放一个事件，返回 true（调用方负责把字节剥掉）
bool classify(const std::string& body, std::vector<OscEvent>* out) {
    if (body.compare(0, 12, "1337;newtab=") == 0) {
        out->push_back({OscEvent::NewTab, body.substr(12)});
        return true;
    }
    if (body == "1337;quit") {
        out->push_back({OscEvent::Quit, {}});
        return true;
    }
    if (body.size() > 2 && body[1] == ';' && (body[0] == '0' || body[0] == '1' || body[0] == '2')) {
        out->push_back({OscEvent::Title, shortTitle(body.substr(2))});
        return true;
    }
    return false;
}

}  // namespace

std::string oscFilter(std::string& pend, const char* data, size_t n, std::vector<OscEvent>* out) {
    std::string buf = std::move(pend);
    pend.clear();
    buf.append(data, n);

    std::string pass;
    pass.reserve(buf.size());
    size_t i = 0;
    while (i < buf.size()) {
        if (buf[i] != '\x1b') {  // 普通字节：整段抄到下一个 ESC
            size_t j = buf.find('\x1b', i);
            if (j == std::string::npos) j = buf.size();
            pass.append(buf, i, j - i);
            i = j;
            continue;
        }
        if (i + 1 >= buf.size()) break;  // 末尾单个 ESC：等下一块再判断
        if (buf[i + 1] != ']') {         // 非 OSC（CSI/单字符）：ESC 原样透传
            pass += buf[i];
            ++i;
            continue;
        }
        size_t end = std::string::npos, termLen = 0;  // end = 序列体结尾，termLen = BEL/ST 长度
        for (size_t j = i + 2; j < buf.size(); ++j) {
            if (buf[j] == '\a') {
                end = j;
                termLen = 1;
                break;
            }
            if (buf[j] == '\x1b') {
                if (j + 1 >= buf.size()) break;
                if (buf[j + 1] == '\\') {
                    end = j;
                    termLen = 2;
                    break;
                }
                ++j;  // 序列体里的 ESC 不是结束符
            }
        }
        if (end == std::string::npos) break;  // 未闭合：连着后面的整段留到下一块
        std::string body = buf.substr(i + 2, end - i - 2);
        if (!classify(body, out)) pass.append(buf, i, end + termLen - i);
        i = end + termLen;
    }
    if (i < buf.size()) {
        pend = buf.substr(i);
        if (pend.size() > kPendCap) {  // shortcut: 超长序列不认，原样放行，免得 pend 无上限
            pass += pend;
            pend.clear();
        }
    }
    return pass;
}

}  // namespace term
