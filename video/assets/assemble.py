#!/usr/bin/env python3
"""拼装 30s 宣传片：4 段实机素材（compose.py）+ 3 张卡（cards.py）→ xfade → mp4 + srt + 封面。"""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent          # video/
WORK, OUT = ROOT / "work", ROOT / "out"
W, H, FPS = 1920, 1080, 60
XF = 0.4                                                # 转场时长

PRE = "L 魔法键：按下白块选中的键 · ZL：空格"

SEGMENTS = [
    # (名字, 类型, 参数, 字幕)
    ("title", "card", ("title.png", 3.0), None),
    ("en", "take", dict(take="t13-en", first=1794042508.527788, in_=63.6, out=77.4,
                        speed=2.0, cap=PRE), None),
    ("out", "take", dict(take="t13-en", first=1794042508.527788, in_=77.4, out=79.4,
                         speed=1.0, cap="A 回车 → 真的把命令跑起来"), None),
    ("zh", "take", dict(take="t21-zh", first=1794042822.500737, in_=32.6, out=42.2,
                        speed=1.0, cap="输入 nihao  →  候选条  →  ZL 空格确认"), None),
    ("numbers", "card", ("numbers.png", 2.8), None),
    ("outro", "card", ("outro.png", 3.4), None),
]


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"FAIL {cmd[:4]}…\n{r.stderr[-2000:]}")
    return r


def duration(path):
    r = run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
             "-of", "json", str(path)])
    return float(json.loads(r.stdout)["format"]["duration"])


def build_card(name, png, dur):
    out = WORK / f"seg-{name}.mp4"
    # 静态卡 + 极慢推近，免得画面完全静止
    vf = ("zoompan=z='min(1+0.018*on/190,1.02)':d=1:fps=60:"
          f"x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)',scale={W}:{H},setsar=1")
    run(["ffmpeg", "-v", "error", "-y", "-loop", "1", "-framerate", str(FPS),
         "-t", str(dur + 0.5), "-i", str(WORK / png), "-vf", vf, "-t", str(dur),
         "-c:v", "libx264", "-crf", "16", "-preset", "medium",
         "-pix_fmt", "yuv420p", "-r", str(FPS), str(out)])
    return out


def build_take(name, spec):
    out = WORK / f"seg-{name}.mp4"
    run([sys.executable, str(ROOT / "assets" / "render_photo.py"),
         "--take", str(ROOT / "raw" / f"{spec['take']}.mp4"),
         "--events", str(ROOT / "raw" / f"{spec['take']}.events"),
         "--first", str(spec["first"]),
         "--in", str(spec["in_"]), "--out", str(spec["out"]),
         "--speed", str(spec["speed"]),
         "--caption", spec["cap"], "--outfile", str(out)])
    return out


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    OUT.mkdir(parents=True, exist_ok=True)
    run([sys.executable, str(ROOT / "assets" / "cards.py")])

    files, caps, kinds = [], [], []
    for name, kind, spec, _ in SEGMENTS:
        p = build_card(name, *spec) if kind == "card" else build_take(name, spec)
        d = duration(p)
        files.append(p)
        kinds.append(kind)
        caps.append(None if kind == "card" else spec["cap"])
        print(f"  {name:8s} {d:6.2f}s  {p.name}")

    # xfade 链
    ins, chain, off = [], [], 0.0
    for i, f in enumerate(files):
        ins += ["-i", str(f)]
    prev, total = "[0:v]", duration(files[0])
    for i in range(1, len(files)):
        off = total - XF
        nxt = f"[v{i}]"
        # 两张静止卡之间用「淡入黑」而不是直接溶解，避免文字压文字
        tr = "fadeblack" if kinds[i] == "card" and kinds[i - 1] == "card" else "fade"
        chain.append(f"{prev}[{i}:v]xfade=transition={tr}:duration={XF}:offset={off:.3f}{nxt}")
        prev = nxt
        total += duration(files[i]) - XF
    fc = ";".join(chain)

    final = OUT / "promo-30s.mp4"
    run(["ffmpeg", "-v", "error", "-y", *ins, "-filter_complex", fc,
         "-map", prev, "-c:v", "libx264", "-crf", "17", "-preset", "slow",
         "-pix_fmt", "yuv420p", "-r", str(FPS), "-movflags", "+faststart", str(final)])
    print(f"成片 {final}  {duration(final):.2f}s")

    # 字幕（与画面内字幕一致，供无声观看/二次剪辑）
    t, lines, idx = 0.0, [], 0
    for i, (f, cap) in enumerate(zip(files, caps)):
        d = duration(f)
        start = t
        end = start + d - (XF if i < len(files) - 1 else 0)
        if cap:
            idx += 1
            lines.append(f"{idx}\n{ts(start)} --> {ts(end)}\n{cap}\n")
        t = end
    (OUT / "promo-30s.srt").write_text("\n".join(lines))
    print("字幕", OUT / "promo-30s.srt")

    run(["ffmpeg", "-v", "error", "-y", "-ss", "1.0", "-i", str(final),
         "-frames:v", "1", str(OUT / "cover.png")])
    print("封面", OUT / "cover.png")


def ts(t):
    h, r = divmod(t, 3600)
    m, s = divmod(r, 60)
    return f"{int(h):02d}:{int(m):02d}:{s:06.3f}".replace(".", ",")


if __name__ == "__main__":
    main()
