#pragma once
// 内置拼音输入法：组字串 + 候选（带注音）+ n-gram 上下文重排
#include <string>
#include <vector>

#include "ime/ngram.h"
#include "ime/pinyin_index.h"

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
    const std::vector<Cand>& cands() const { return cands_; }
    int page() const { return page_; }
    int pageCount() const;
    void nextPage();
    void prevPage();
    // 候选条里的选中项（页内下标，0 = 本页第一个）：R/ZR 移动它，空格上屏它
    int sel() const { return sel_; }
    void selStep(int d);
    std::string commit(int idx);   // -1 = 当前页选中的那个
    std::string commitRaw();       // 拼音原样上屏（回车键的行为：打出 nihao 本身）
    uint32_t context() const { return ctx_; }
    void setContext(uint32_t cp) {  // 自检/外部注入上下文
        ctx_ = cp;
        refresh();
    }

private:
    void refresh();
    PinyinIndex idx_;
    NgramTable ng_;
    std::string buf_;
    std::vector<Cand> cands_;
    int page_ = 0;
    int sel_ = 0;
    int pageSize_ = kMaxPageSize;
    uint32_t ctx_ = 0;  // 上一个提交字的码位
};

}  // namespace ime
