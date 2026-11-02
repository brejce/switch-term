#!/usr/bin/env -S uv run --quiet
"""从中文纯文本构建 bigram 表，给候选做上下文重排用。

    uv run tools/build_ngram.py <输出.bin> <文本文件...>

格式：b"NGRM" + u16 版本 + u32 条数 + N×(u16 prev, u16 next, u32 计数)
只保留汉字对，按计数降序截断，够 IME 重排用。
"""
import collections
import struct
import sys

TOP_N = 300_000


def is_hanzi(ch: str) -> bool:
    return 0x4E00 <= ord(ch) <= 0x9FFF


def main() -> None:
    if len(sys.argv) < 3:
        print(__doc__)
        return
    out_path, files = sys.argv[1], sys.argv[2:]

    bigram = collections.Counter()
    total = 0
    for path in files:
        try:
            text = open(path, encoding="utf-8", errors="ignore").read()
        except OSError as e:
            print(f"跳过 {path}: {e}")
            continue
        total += len(text)
        prev = ""
        for ch in text:
            if is_hanzi(ch):
                if prev:
                    bigram[(prev, ch)] += 1
                prev = ch
            else:
                prev = ""
    print(f"语料 {total/1e6:.1f}M 字符，bigram {len(bigram)} 条")

    items = bigram.most_common(TOP_N)
    with open(out_path, "wb") as f:
        f.write(b"NGRM")
        f.write(struct.pack("<HI", 1, len(items)))
        for (a, b), n in items:
            f.write(struct.pack("<HHI", ord(a), ord(b), min(n, 0xFFFFFFFF)))
    import os
    print(f"✓ {out_path}  {os.path.getsize(out_path)/1e6:.2f} MB  {len(items)} 条")
    for probe in ("你好", "我们", "什么", "桌面"):
        print(f"   {probe}: {bigram.get((probe[0], probe[1]), 0)}")


if __name__ == "__main__":
    main()
