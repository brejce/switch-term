#pragma once
// 英文 T9：数字串 → 单词候选（data/t9en.txt，按词频排序，1 万词）
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace ime {

class T9En {
public:
    bool load(const std::string& path);
    size_t size() const { return total_; }
    std::vector<std::string> query(const std::string& digits, size_t max) const;

private:
    std::unordered_map<std::string, std::vector<std::string>> map_;
    size_t total_ = 0;
};

}  // namespace ime
