#include "ime/pinyin_index.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace ime {

std::string utf8(uint32_t cp) {
    std::string s;
    if (cp < 0x80) return std::string(1, char(cp));
    if (cp < 0x800) {
        s += char(0xC0 | (cp >> 6));
        s += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += char(0xE0 | (cp >> 12));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    } else {
        s += char(0xF0 | (cp >> 18));
        s += char(0x80 | ((cp >> 12) & 0x3F));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    }
    return s;
}

std::string tonify(const std::string& syl, int tone) {
    if (tone < 1 || tone > 4 || syl.empty()) return syl;
    // 声调符号位置：a > o > e > iu→u / ui→i > 最后一个 i/u/v
    auto vowels = [](char c) { return c == 'a' || c == 'o' || c == 'e' || c == 'i' || c == 'u' || c == 'v'; };
    size_t pos = std::string::npos;
    if ((pos = syl.find('a')) == std::string::npos &&
        (pos = syl.find('o')) == std::string::npos &&
        (pos = syl.find('e')) == std::string::npos) {
        size_t iu = syl.find("iu"), ui = syl.find("ui");
        if (iu != std::string::npos) pos = iu + 1;
        else if (ui != std::string::npos) pos = ui + 1;
        else
            for (size_t i = syl.size(); i > 0; --i)
                if (vowels(syl[i - 1])) { pos = i - 1; break; }
    }
    if (pos == std::string::npos || pos >= syl.size()) return syl;

    // 基字母 → 四声（UTF-8 硬编码，每个 2 字节）
    static const char* kTable[6][4] = {
        {"\xC4\x81", "\xC3\xA1", "\xC7\x8E", "\xC3\xA0"},  // a āáǎà
        {"\xC5\x8D", "\xC3\xB3", "\xC7\x92", "\xC3\xB2"},  // o ōóǒò
        {"\xC4\x93", "\xC3\xA9", "\xC4\x9B", "\xC3\xA8"},  // e ēéěè
        {"\xC4\xAB", "\xC3\xAD", "\xC7\x90", "\xC3\xAC"},  // i īíǐì
        {"\xC5\xAB", "\xC3\xBA", "\xC7\x94", "\xC3\xB9"},  // u ūúǔù
        {"\xC7\x96", "\xC7\x98", "\xC7\x9A", "\xC7\x9C"},  // v ǖǘǚǜ
    };
    int idx = -1;
    switch (syl[pos]) {
        case 'a': idx = 0; break;
        case 'o': idx = 1; break;
        case 'e': idx = 2; break;
        case 'i': idx = 3; break;
        case 'u': idx = 4; break;
        case 'v': idx = 5; break;
        default: return syl;
    }
    std::string out = syl.substr(0, pos);
    out += kTable[idx][tone - 1];
    out += syl.substr(pos + 1);
    if (idx == 5)  // ü 在带声调后按拼音惯例写作 v 的位置显示为 ü
        return out;
    return out;
}

namespace {
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
}  // namespace

bool PinyinIndex::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "pinyin: 打不开 %s\n", path.c_str());
        return false;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.size() < 12 || std::memcmp(buf.data(), "PYIX", 4) != 0) {
        std::fprintf(stderr, "pinyin: 头错误 %s\n", path.c_str());
        return false;
    }
    const uint16_t ver = rd16(&buf[4]);
    if (ver != 3) {
        std::fprintf(stderr, "pinyin: 版本 %u，需要 v3（重跑 tools/make_pinyin_index.py）\n", ver);
        return false;
    }
    const uint16_t maxCand = rd16(&buf[6]);
    const uint32_t n = rd32(&buf[8]);
    const size_t entry = 12 + 2 + size_t(maxCand) * 2 + maxCand + maxCand;
    if (buf.size() < 12 + entry * n) {
        std::fprintf(stderr, "pinyin: 文件被截断\n");
        return false;
    }
    const uint8_t* p = buf.data() + 12;
    for (uint32_t i = 0; i < n; ++i, p += entry) {
        std::string syl(reinterpret_cast<const char*>(p));
        size_t cnt = p[12];
        if (cnt > maxCand) cnt = maxCand;
        Entry e;
        for (size_t k = 0; k < cnt; ++k) {
            e.cps.push_back(rd16(p + 14 + k * 2));
            e.tones.push_back(p[14 + maxCand * 2 + k]);
        }
        map_[syl] = std::move(e);
    }
    std::fprintf(stderr, "pinyin: %s  %u 音节\n", path.c_str(), n);
    return true;
}

const PinyinIndex::Entry* PinyinIndex::lookup(const std::string& syl) const {
    auto it = map_.find(syl);
    return it == map_.end() ? nullptr : &it->second;
}

std::vector<std::string> PinyinIndex::syllableList() const {
    std::vector<std::string> out;
    out.reserve(map_.size());
    for (const auto& kv : map_) out.push_back(kv.first);
    return out;
}

namespace {
// 字母 → 电话键盘数字：abc=2 def=3 ghi=4 jkl=5 mno=6 pqrs=7 tuv=8 wxyz=9
char digitOf(char c) {
    if (c <= 'c') return '2';
    if (c <= 'f') return '3';
    if (c <= 'i') return '4';
    if (c <= 'l') return '5';
    if (c <= 'o') return '6';
    if (c <= 's') return '7';
    if (c <= 'v') return '8';
    return '9';
}
std::string digitsOf(const std::string& syl) {
    std::string d;
    for (char c : syl) d += digitOf(c);
    return d;
}
}  // namespace

std::vector<Cand> PinyinIndex::queryDigits(const std::string& digits, size_t maxCand) const {
    std::vector<Cand> out;
    if (digits.empty()) return out;

    // ① 正好是一个完整音节
    for (const auto& kv : map_) {
        if (digitsOf(kv.first) != digits) continue;
        const Entry& e = kv.second;
        for (size_t i = 0; i < e.cps.size() && out.size() < maxCand; ++i) {
            Cand c;
            c.text = utf8(e.cps[i]);
            c.annot = tonify(kv.first, i < e.tones.size() ? e.tones[i] : 0);
            c.first = e.cps[i];
            out.push_back(std::move(c));
        }
    }
    if (!out.empty()) return out;

    // ② 拆成两个音节（9 键输入词：64826 → ni|hao 之类）
    for (size_t i = 1; i + 1 < digits.size(); ++i) {
        std::string dl = digits.substr(0, i), dr = digits.substr(i);
        for (const auto& a : map_) {
            if (digitsOf(a.first) != dl) continue;
            for (const auto& b : map_) {
                if (digitsOf(b.first) != dr) continue;
                // 每个音节只取前 3 个常用字：否则一对音节就把候选吃满，
                // 真正常用的组合（ni|hao）根本轮不到
                for (size_t i1 = 0; i1 < a.second.cps.size() && i1 < 3; ++i1) {
                    uint32_t c1 = a.second.cps[i1];
                    for (size_t i2 = 0; i2 < b.second.cps.size() && i2 < 3; ++i2) {
                        uint32_t c2 = b.second.cps[i2];
                        if (out.size() >= maxCand) return out;
                        Cand c;
                        c.text = utf8(c1) + utf8(c2);
                        c.annot = a.first + " " + b.first;  // 组合音无法定声调，只给音节
                        c.first = c1;
                        c.second = c2;
                        out.push_back(std::move(c));
                    }
                }
            }
        }
    }
    return out;
}

std::vector<Cand> PinyinIndex::query(const std::string& py, size_t maxCand) const {
    std::vector<Cand> out;
    if (py.empty()) return out;

    if (const Entry* one = lookup(py)) {  // 单音节
        for (size_t i = 0; i < one->cps.size() && out.size() < maxCand; ++i) {
            Cand c;
            c.text = utf8(one->cps[i]);
            c.annot = tonify(py, one->tones[i]);
            c.first = one->cps[i];
            out.push_back(std::move(c));
        }
        return out;
    }
    // 两音节组合（nihao → 你好），注音 a b 中间留空格
    for (size_t cut = 1; cut < py.size(); ++cut) {
        const std::string sa = py.substr(0, cut), sb = py.substr(cut);
        const Entry* a = lookup(sa);
        const Entry* b = lookup(sb);
        if (!a || !b) continue;
        for (size_t i = 0; i < a->cps.size() && i < 4; ++i)
            for (size_t j = 0; j < b->cps.size() && j < 8; ++j) {
                if (out.size() >= maxCand) return out;
                Cand c;
                c.text = utf8(a->cps[i]) + utf8(b->cps[j]);
                c.annot = tonify(sa, a->tones[i]) + " " + tonify(sb, b->tones[j]);
                c.first = a->cps[i];
                c.second = b->cps[j];
                out.push_back(std::move(c));
            }
        if (!out.empty()) return out;
    }
    // 前缀匹配：半个音节（如 y）也要有候选，取每个匹配音节的第一个字
    for (const auto& kv : map_) {
        if (kv.first.size() <= py.size()) continue;
        if (kv.first.compare(0, py.size(), py) != 0) continue;
        if (kv.second.cps.empty()) continue;
        Cand c;
        c.text = utf8(kv.second.cps[0]);
        c.annot = tonify(kv.first, kv.second.tones.empty() ? 0 : kv.second.tones[0]);
        c.first = kv.second.cps[0];
        out.push_back(std::move(c));
        if (out.size() >= maxCand) break;
    }
    return out;
}

}  // namespace ime
