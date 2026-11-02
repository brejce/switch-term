#include "ime/t9_en.h"

#include <cstdio>
#include <fstream>

namespace ime {

bool T9En::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "t9en: 打不开 %s\n", path.c_str());
        return false;
    }
    std::string digits, word;
    while (f >> digits >> word) {
        map_[digits].push_back(word);  // 文件按词频排，push_back 保持顺序
        ++total_;
    }
    return total_ > 0;
}

std::vector<std::string> T9En::query(const std::string& digits, size_t max) const {
    std::vector<std::string> out;
    auto it = map_.find(digits);
    if (it == map_.end()) return out;
    for (const std::string& w : it->second) {
        if (out.size() >= max) break;
        out.push_back(w);
    }
    return out;
}

}  // namespace ime
