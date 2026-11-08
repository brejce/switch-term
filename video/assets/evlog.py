#!/usr/bin/env python3
"""在 Switch 上录按键事件（evdev），给后期做"按键按动效果"用。

不依赖 python-evdev：直接 ioctl/read。事件按「读到的墙钟时间」记录，和
ffmpeg 录屏用同一个时钟，后期按 t0 对齐即可。

用法: evlog.py <out.tsv> [duration_sec]
输出行: <wall_epoch> <dev_name> <type> <code> <value>
"""
import array
import fcntl
import os
import select
import struct
import sys
import time

EVIOCGNAME256 = (2 << 30) | (256 << 16) | (0x45 << 8) | 0x06
FMT = "llHHI"          # struct input_event (64-bit)
SIZE = struct.calcsize(FMT)
EV_KEY, EV_ABS, EV_SYN = 1, 3, 0


def dev_name(fd):
    buf = array.array("B", b"\0" * 256)
    try:
        fcntl.ioctl(fd, EVIOCGNAME256, buf)
    except OSError:
        return ""
    return buf.tobytes().split(b"\0")[0].decode("utf-8", "replace")


def main():
    out = sys.argv[1]
    dur = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
    fds, names = [], {}
    for i in range(64):
        path = "/dev/input/event%d" % i
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        except OSError:
            continue
        nm = dev_name(fd)
        if "Joy-Con" not in nm or "Serial" in nm or "IMU" in nm:
            os.close(fd)
            continue
        fds.append(fd)
        names[fd] = nm

    t_end = time.time() + dur
    with open(out, "w") as f:
        f.write("# start %.6f\n" % time.time())
        for fd in fds:
            f.write("# dev %s\n" % names[fd])
        f.flush()
        while time.time() < t_end:
            r, _, _ = select.select(fds, [], [], 0.2)
            for fd in r:
                while True:
                    try:
                        data = os.read(fd, SIZE * 64)
                    except BlockingIOError:
                        break
                    except OSError:
                        break
                    if not data:
                        break
                    now = time.time()
                    for off in range(0, len(data) - SIZE + 1, SIZE):
                        _s, _us, typ, code, val = struct.unpack_from(FMT, data, off)
                        if typ == EV_SYN:
                            continue
                        f.write("%.6f %s %d %d %d\n" % (now, names[fd], typ, code, val))
                    f.flush()
                    if len(data) < SIZE:
                        break
    for fd in fds:
        os.close(fd)


if __name__ == "__main__":
    main()
