#!/bin/sh
# 在 Switch 上一条命令搞定「录屏 + 录按键事件」，两者用同一个墙钟。
# 用法: sh cap.sh <name> [secs]   ->  /home/brejce/video/raw/<name>.{mp4,events,log,first}
#
# 关键：-use_wallclock_as_timestamps 1 让每个 fb 帧带墙钟 PTS，
# 配合 -debug_ts 把首帧的绝对时间打进日志（<name>.first，秒，epoch），
# 后期就能把 evdev 事件精确对到视频帧：t_video = t_event - first。
set -e
NAME=${1:?name}
SECS=${2:-20}
DIR=/home/brejce/video/raw
mkdir -p "$DIR"

python3 /home/brejce/video/cap/evlog.py "$DIR/$NAME.events" $((SECS + 4)) &
EV=$!
sleep 0.2
date +%s.%N > "$DIR/$NAME.t0"

ffmpeg -hide_banner -loglevel debug -debug_ts -y \
    -f fbdev -use_wallclock_as_timestamps 1 -framerate 60 -i /dev/fb0 \
    -vf transpose=1 -c:v h264_nvv4l2 -b:v 12M -pix_fmt yuv420p -t "$SECS" \
    "$DIR/$NAME.mp4" 2> "$DIR/$NAME.log" || true

kill "$EV" 2>/dev/null || true
wait "$EV" 2>/dev/null || true

python3 - "$DIR/$NAME.log" "$DIR/$NAME.first" <<'PY'
import re, sys
log, out = sys.argv[1], sys.argv[2]
first = None
with open(log, errors="ignore") as f:
    for line in f:
        m = re.search(r"pkt_pts:(\d{10,})", line)
        if m:
            first = int(m.group(1)) / 1e6
            break
if first is None:
    raise SystemExit("no pkt_pts found in log")
open(out, "w").write("%.6f\n" % first)
print("first_frame_wall=%.6f" % first)
PY
echo "done $DIR/$NAME.mp4"
