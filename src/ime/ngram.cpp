#include "ime/ngram.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace ime {

bool NgramTable::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "ngram: 没有 %s（跳过上下文重排）\n", path.c_str());
        return false;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.size() < 10 || std::memcmp(buf.data(), "NGRM", 4) != 0) return false;
    uint32_t n = uint32_t(buf[6]) | (uint32_t(buf[7]) << 8) | (uint32_t(buf[8]) << 16) |
                 (uint32_t(buf[9]) << 24);
    if (buf.size() < 10 + size_t(n) * 8) {
        std::fprintf(stderr, "ngram: 文件被截断\n");
        return false;
    }
    m_.reserve(n * 2);
    const uint8_t* p = buf.data() + 10;
    for (uint32_t i = 0; i < n; ++i, p += 8) {
        uint32_t a = uint32_t(p[0] | (p[1] << 8));
        uint32_t b = uint32_t(p[2] | (p[3] << 8));
        uint32_t c = uint32_t(p[4]) | (uint32_t(p[5]) << 8) | (uint32_t(p[6]) << 16) |
                     (uint32_t(p[7]) << 24);
        m_[(uint64_t(a) << 32) | b] = c;
    }
    std::fprintf(stderr, "ngram: %u 条\n", n);
    return true;
}

}  // namespace ime
