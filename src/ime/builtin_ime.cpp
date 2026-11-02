#include "ime/builtin_ime.h"

#include <algorithm>

namespace ime {
namespace {
constexpr int kPageSize = 7;

uint32_t lastCodepoint(const std::string& s) {
    if (s.empty()) return 0;
    size_t i = s.size();
    while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) --i;
    unsigned char b = static_cast<unsigned char>(s[i]);
    uint32_t cp = b < 0x80 ? b : (b & 0xE0) == 0xC0 ? (b & 0x1F) : (b & 0xF0) == 0xE0 ? (b & 0x0F)
                                                                                    : (b & 0x07);
    for (size_t k = i + 1; k < s.size(); ++k)
        cp = (cp << 6) | (static_cast<unsigned char>(s[k]) & 0x3F);
    return cp;
}
}  // namespace

bool BuiltinIme::load(const std::string& idxPath, const std::string& ngramPath) {
    if (!idx_.load(idxPath)) return false;
    ng_.load(ngramPath);  // 没有语料表也能用，只是不重排
    // 英文 T9 词表放在 pinyin.idx 同目录
    std::string dir = idxPath.substr(0, idxPath.find_last_of('/') + 1);
    en_.load(dir + "t9en.txt");
    return true;
}

void BuiltinIme::push(char c) {
    // 字母（全拼 / 弹窗多击）或数字（T9 九键）都进同一个缓冲；
    // refresh() 按「全是数字」自动选查询方式
    bool letter = (c >= 'a' && c <= 'z');
    bool digit = (c >= '2' && c <= '9');
    if (!letter && !digit) return;
    if (buf_.size() >= 24) return;
    buf_ += c;
    refresh();
}

void BuiltinIme::backspace() {
    if (buf_.empty()) return;
    buf_.pop_back();
    refresh();
}

void BuiltinIme::clear() {
    buf_.clear();
    cands_.clear();
    page_ = 0;
}

void BuiltinIme::refresh() {
    page_ = 0;
    cands_.clear();
    if (buf_.empty()) return;
    bool allDigits = std::all_of(buf_.begin(), buf_.end(),
                                 [](char c) { return c >= '0' && c <= '9'; });
    // 九键会生成很多组合：先多取一些，按 bigram 排完再截断，否则常用组合排不进来
    size_t want = allDigits ? 400 : size_t(kPageSize) * 8;
    if (allDigits && !chinese && en_.size()) {  // 英文态：查英文词表
        cands_.clear();
        for (const std::string& w : en_.query(buf_, want)) {
            Cand c;
            c.text = w;
            cands_.push_back(std::move(c));
        }
        if (cands_.size() > size_t(kPageSize) * 8) cands_.resize(size_t(kPageSize) * 8);
        return;
    }
    cands_ = allDigits ? idx_.queryDigits(buf_, want) : idx_.query(buf_, want);
    if (ng_.loaded()) {  // 词组合靠 bigram 排序：没有上下文时只算候选内部 bigram

        std::stable_sort(cands_.begin(), cands_.end(), [&](const Cand& a, const Cand& b) {
            auto sc = [&](const Cand& c) {
                uint32_t s = ctx_ ? ng_.count(ctx_, c.first) * 8 : 0;
                if (c.second) s += ng_.count(c.first, c.second) * 2;
                return s;
            };
            return sc(a) > sc(b);
        });
    }
    if (cands_.size() > size_t(kPageSize) * 8) cands_.resize(size_t(kPageSize) * 8);
}

int BuiltinIme::pageCount() const {
    if (cands_.empty()) return 0;
    return int((cands_.size() + kPageSize - 1) / kPageSize);
}

void BuiltinIme::nextPage() {
    if (page_ + 1 < pageCount()) ++page_;
}

void BuiltinIme::prevPage() {
    if (page_ > 0) --page_;
}

std::string BuiltinIme::commitRaw() {
    std::string out = buf_;
    clear();
    return out;
}

std::string BuiltinIme::commit(int idx) {
    if (cands_.empty()) return {};
    int i = idx < 0 ? page_ * kPageSize : idx;
    if (i < 0 || i >= int(cands_.size())) return {};
    std::string out = cands_[size_t(i)].text;
    ctx_ = lastCodepoint(out);  // 更新上下文
    clear();
    return out;
}

}  // namespace ime
