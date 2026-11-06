#pragma once
// 脏矩形集合：纯逻辑，不碰像素。上层把「这一帧变了哪几块」丢进来，
// 合并成少量矩形；块数/面积超阈值就退化成整屏一块（碎片多时局部拷贝反而不划算）。
#include <algorithm>

#include "platform/display_backend.h"

namespace disp {

class DamageList {
public:
    // 超过这个块数就退化成整屏：再多付的查交集/绘制开销抵不过省下的拷贝
    static constexpr int kMaxRects = 8;

    void push(const Rect& r);
    // 裁到画布内；块数超限或总面积超过屏幕 60% 时改成「整屏一块」
    void clip(int w, int h);
    void clear() {
        n_ = 0;
        overflow_ = false;
    }
    bool empty() const { return n_ == 0; }
    int size() const { return n_; }
    const Rect* data() const { return rects_; }

private:
    static long area(const Rect& r) { return r.empty() ? 0 : long(r.w) * long(r.h); }

    Rect rects_[kMaxRects];
    int n_ = 0;
    bool overflow_ = false;  // 放不下新的一块：clip 时整屏兜底
};

inline void DamageList::push(const Rect& r) {
    if (r.empty()) return;
    for (int i = 0; i < n_; ++i) {
        const Rect& a = rects_[i];
        const int x0 = std::min(a.x, r.x), y0 = std::min(a.y, r.y);
        const Rect uni{x0, y0, std::max(a.right(), r.right()) - x0,
                       std::max(a.bottom(), r.bottom()) - y0};
        // 只有明显重叠（并集面积 < 两块之和）才合；只是挨着合了会多画中间的缝隙
        if (area(uni) < area(a) + area(r)) {
            rects_[i] = uni;
            return;
        }
    }
    if (n_ < kMaxRects) {
        rects_[n_++] = r;
        return;
    }
    overflow_ = true;
}

inline void DamageList::clip(int w, int h) {
    if (n_ == 0) return;
    long total = 0;
    int m = 0;
    for (int i = 0; i < n_; ++i) {
        Rect r = rects_[i];
        if (r.x < 0) {
            r.w += r.x;
            r.x = 0;
        }
        if (r.y < 0) {
            r.h += r.y;
            r.y = 0;
        }
        if (r.right() > w) r.w = w - r.x;
        if (r.bottom() > h) r.h = h - r.y;
        if (r.empty()) continue;
        rects_[m++] = r;
        total += long(r.w) * long(r.h);
    }
    n_ = m;
    if (n_ == 0) return;
    // 超过 8 块或面积过半屏：局部拷贝省的带宽不如多画几遍亏，直接整屏
    if (overflow_ || total * 100 > long(w) * long(h) * 60) {
        rects_[0] = Rect{0, 0, w, h};
        n_ = 1;
    }
    overflow_ = false;
}

}  // namespace disp
