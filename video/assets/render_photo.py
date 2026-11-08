#!/usr/bin/env python3
"""把一段实拍按「照片外壳」合成成 1920x1080@60 视频（含按键高亮 + 字幕）。

  python3 video/assets/render_photo.py --take video/raw/t13-en.mp4 \
      --events video/raw/t13-en.events --first 1794042508.527788 \
      --in 66.6 --out 70.6 --speed 1.0 --caption "LB 上屏" --outfile video/work/test_photo.mp4
"""
import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compose_photo as cp

W, H, FPS = 1920, 1080, 60
SRC_W, SRC_H = 1280, 720

CODE = {
    304: "B", 305: "A", 307: "X", 308: "Y",
    310: "L", 311: "R", 312: "ZL", 313: "ZR",
    317: "L3", 544: "UP", 545: "DOWN", 546: "LEFT", 547: "RIGHT",
}


def load_events(path, first):
    """返回 [(t_video, key)]，t_video = 墙钟 − 首帧墙钟。"""
    out = []
    if not path or not os.path.exists(path):
        return out
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                continue
            p = line.split()
            if len(p) < 6:
                continue
            try:
                wall, typ, code, val = float(p[0]), int(p[-3]), int(p[-2]), int(p[-1])
            except ValueError:
                continue
            if typ != 1 or val != 1:
                continue
            key = CODE.get(code)
            if key:
                out.append((wall - first, key))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--take", required=True)
    ap.add_argument("--events", default="")
    ap.add_argument("--first", type=float, required=True)
    ap.add_argument("--in", dest="t_in", type=float, required=True)
    ap.add_argument("--out", dest="t_out", type=float, required=True)
    ap.add_argument("--speed", type=float, default=1.0)
    ap.add_argument("--caption", default="")
    ap.add_argument("--outfile", required=True)
    ap.add_argument("--crf", type=int, default=18)
    a = ap.parse_args()

    dur = (a.t_out - a.t_in) / a.speed
    frames = int(round(dur * FPS))
    events = load_events(a.events, a.first)

    dec = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-ss", f"{a.t_in:.3f}", "-t", f"{a.t_out - a.t_in:.3f}",
         "-i", a.take, "-vf", f"setpts=PTS/{a.speed},fps={FPS},scale={SRC_W}:{SRC_H}:flags=lanczos",
         "-pix_fmt", "rgb24", "-f", "rawvideo", "-"],
        stdout=subprocess.PIPE,
    )
    enc = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
         "-s", f"{W}x{H}", "-r", str(FPS), "-i", "-",
         "-c:v", "libx264", "-crf", str(a.crf), "-preset", "medium",
         "-pix_fmt", "yuv420p", "-movflags", "+faststart", a.outfile],
        stdin=subprocess.PIPE,
    )

    n = SRC_W * SRC_H * 3
    for i in range(frames):
        buf = dec.stdout.read(n)
        if len(buf) < n:
            break
        from PIL import Image
        frame = Image.frombytes("RGB", (SRC_W, SRC_H), buf)
        t = a.t_in + i * a.speed / FPS
        key = None
        for te, k in events:
            if 0 <= t - te < 0.19:
                key = k
        out = cp.compose(frame, a.caption, key)
        enc.stdin.write(out.convert("RGB").tobytes())
    enc.stdin.close()
    enc.wait()
    dec.wait()
    print(a.outfile, frames, "frames")


if __name__ == "__main__":
    main()
