#include "ime/builtin_ime.h"

#include <algorithm>

namespace ime {
namespace {

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
    return true;
}

void BuiltinIme::push(char c) {
    if (c < 'a' || c > 'z') return;  // 只有全拼字母进组字串
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
    sel_ = 0;
    cands_.clear();
    if (buf_.empty()) return;
    cands_ = idx_.query(buf_, size_t(pageSize_) * 8);
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
    if (cands_.size() > size_t(pageSize_) * 8) cands_.resize(size_t(pageSize_) * 8);
}

int BuiltinIme::pageCount() const {
    if (cands_.empty()) return 0;
    return int((cands_.size() + pageSize_ - 1) / pageSize_);
}

void BuiltinIme::nextPage() {
    if (page_ + 1 < pageCount()) ++page_;
    sel_ = 0;
}

void BuiltinIme::prevPage() {
    if (page_ > 0) --page_;
    sel_ = 0;
}

// 页内step：走到本页末尾就绕回本页开头（翻页归 Tab）
void BuiltinIme::selStep(int d) {
    int n = int(cands_.size()) - page_ * pageSize_;
    if (n > pageSize_) n = pageSize_;
    if (n <= 0) return;
    sel_ = ((sel_ + d) % n + n) % n;
}

std::string BuiltinIme::commitRaw() {
    std::string out = buf_;
    clear();
    return out;
}

std::string BuiltinIme::commit(int idx) {
    // 组字串不合法（dd/xyz/混进数字）时没有候选：拼音原样上屏，别让用户白打的字凭空消失
    if (cands_.empty()) return commitRaw();
    int i = idx < 0 ? page_ * pageSize_ + sel_ : idx;
    if (i < 0 || i >= int(cands_.size())) return {};
    std::string out = cands_[size_t(i)].text;
    ctx_ = lastCodepoint(out);  // 更新上下文
    clear();
    return out;
}

}  // namespace ime
