#pragma once
// bigram 表：候选上下文重排（格式见 tools/build_ngram.py）
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ime {

class NgramTable {
public:
    bool load(const std::string& path);
    bool loaded() const { return !m_.empty(); }
    uint32_t count(uint32_t a, uint32_t b) const {
        auto it = m_.find((uint64_t(a) << 32) | b);
        return it == m_.end() ? 0 : it->second;
    }

private:
    std::unordered_map<uint64_t, uint32_t> m_;
};

}  // namespace ime
