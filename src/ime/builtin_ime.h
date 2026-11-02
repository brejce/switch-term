#pragma once
// 内置拼音输入法：组字串 + 候选（带注音）+ n-gram 上下文重排
#include <string>
#include <vector>

#include "ime/ngram.h"
#include "ime/pinyin_index.h"
#include "ime/t9_en.h"

namespace ime {

class BuiltinIme {
public:
    bool load(const std::string& idxPath, const std::string& ngramPath);

    bool chinese = true;
    bool composing() const { return !buf_.empty(); }

    // 切语言。正在组的拼音原样交回给调用方上屏（fcitx 也是这么干的，不能吞字）
    std::string toggle() {
        std::string raw = buf_;
        chinese = !chinese;
        clear();
        return raw;
    }
    void push(char c);  // a-z
    void backspace();
    void clear();

    static constexpr int kMaxPageSize = 7;  // 底部候选条最多放得下几个
    int pageSize() const { return pageSize_; }
    // engine.ime.cand_per_page 落到 IME 内部翻页；越界值忽略，不改现有状态
    void setPageSize(int n) {
        if (n < 1 || n > kMaxPageSize) return;
        pageSize_ = n;
        if (page_ >= pageCount()) page_ = pageCount() > 0 ? pageCount() - 1 : 0;
    }

    const std::string& preedit() const { return buf_; }
    bool digitsBuffer() const {  // 当前组字串是不是 T9 数字序列
        if (buf_.empty()) return false;
        for (char c : buf_)
            if (c < '0' || c > '9') return false;
        return true;
    }
    const std::vector<Cand>& cands() const { return cands_; }
    int page() const { return page_; }
    int pageCount() const;
    void nextPage();
    void prevPage();
    std::string commit(int idx);   // -1 = 当前页第一个
    std::string commitRaw();       // 拼音原样上屏（回车键的行为：打出 nihao 本身）
    uint32_t context() const { return ctx_; }
    void setContext(uint32_t cp) {  // 自检/外部注入上下文
        ctx_ = cp;
        refresh();
    }

private:
    void refresh();
    T9En en_;
    PinyinIndex idx_;
    NgramTable ng_;
    std::string buf_;
    std::vector<Cand> cands_;
    int page_ = 0;
    int pageSize_ = kMaxPageSize;
    uint32_t ctx_ = 0;  // 上一个提交字的码位
};

}  // namespace ime
