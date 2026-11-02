#pragma once
// pinyin.idx (PYIX v3) 读取：音节 → 候选字 + 声调
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ime {

struct Cand {
    std::string text;    // UTF-8
    std::string annot;   // 注音（带声调），如 nǐ
    uint32_t first = 0;  // 首字码位，供 n-gram 重排
    uint32_t second = 0;
    bool operator==(const Cand& o) const {
        return text == o.text && annot == o.annot;
    }
};

class PinyinIndex {
public:
    bool load(const std::string& path);
    size_t syllables() const { return map_.size(); }
    std::vector<Cand> query(const std::string& pinyin, size_t maxCand) const;
    // T9 九键：数字串（2-9）→ 候选。先把每个音节编码成数字（abc=2 … wxyz=9）再匹配
    std::vector<Cand> queryDigits(const std::string& digits, size_t maxCand) const;
    std::vector<std::string> syllableList() const;

private:
    struct Entry {
        std::vector<uint32_t> cps;
        std::vector<uint8_t> tones;
    };
    const Entry* lookup(const std::string& syl) const;
    std::map<std::string, Entry> map_;
};

std::string utf8(uint32_t cp);
std::string tonify(const std::string& syllable, int tone);  // ni,3 → nǐ

}  // namespace ime
