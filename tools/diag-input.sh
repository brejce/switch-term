#!/usr/bin/env bash
# 手柄输入排障：跑 N 秒（默认 20），边按边收日志，最后只打几行结论——不用复制命令，也不用贴长日志。
# 用法：bash tools/diag-input.sh [秒数]
set -u
cd "$(dirname "$0")/.." || exit 1
BIN=./build/switch-ime-term
[ -x "$BIN" ] || { echo "还没编译：cmake --build build -j8"; exit 1; }
SECS=${1:-20}
LOG=$(mktemp /tmp/sit-diag.XXXXXX.log)

echo "① 系统里的输入设备（带 Virtual / X-Box 的那块就是 KDE 之类建的虚拟手柄）"
for f in /sys/class/input/event*/device/name; do
    [ -r "$f" ] && printf '   %-26s %s\n' "${f%/device/name}" "$(cat "$f" 2>/dev/null)"
done

echo
echo "   谁在往 /dev/uinput 打字（有名字 = 有程序在把手柄翻译成键码注入，会和我们抢着打字）"
{ for p in /proc/[0-9]*; do
      for f in "$p"/fd/*; do
          t=$(readlink "$f" 2>/dev/null) || continue
          case "$t" in
              *uinput*) tr '\0' ' ' < "$p/cmdline" 2>/dev/null | cut -c1-70
                        echo ;;
          esac
      done
  done 2>/dev/null | sort -u | head -5 | sed 's/^/   /'
} || true
echo "   （上面空的就是没有——KDE 那个「手柄映射」只是显示，没在注入键码）"

echo
echo "② 我们认成手柄的设备"
"$BIN" --list-gamepad 2>&1 | sed -n '1,10p'

echo
echo "③ 输出口"
"$BIN" --print-config 2>&1 | grep -oE 'sink=[a-z]+' | head -1 |
    sed 's/^/   /; s/$/   （pty=只写自己的终端；uinput=打给前台焦点窗口）/'

echo
echo "④ ${SECS} 秒内请按：先 LB 上屏几下 → 推摇杆换字母再 LB → 最后按 +（会弹 onboard）"
echo "   时间到自动结束…"
timeout "$SECS" env SIT_DEBUG=1 "$BIN" 2>"$LOG" >/dev/null
echo

pads=$(grep -c '\[pad\]' "$LOG" 2>/dev/null || true)
drop=$(grep -c '\[magic\] 丢弃' "$LOG" 2>/dev/null || true)
comp=$(grep -c '\[route\].*consumed=1 bytes=0' "$LOG" 2>/dev/null || true)
sent=$(grep -cE '\[route\].*bytes=[1-9]' "$LOG" 2>/dev/null || true)
zh=$(grep -oE 'zh=[0-9]' "$LOG" 2>/dev/null | sort | uniq -c | tr '\n' ' ')

echo "──────── 结论（把这 6 行念给我就行）────────"
printf '  1) 收到的手柄键事件      %s 条\n' "$pads"
printf '  2) LB 在键缝里被丢        %s 次（日志含 "snapped=-1"）\n' "$drop"
printf '  3) 字母进了组字          %s 次\n' "$comp"
printf '  4) 真正写给终端          %s 次\n' "$sent"
printf '  5) 中英态分布            %s\n' "${zh:-（没数据）}"
printf '  6) 完整日志              %s\n' "$LOG"
echo
if [ "$pads" -eq 0 ]; then
    echo "  → 判读：我们一个手柄键都没收到。看第 ① 段里有没有多余的虚拟手柄，和第 ② 段我们选中的设备。"
elif [ "$sent" -eq 0 ] && [ "$comp" -gt 0 ]; then
    echo "  → 判读：键都收到了、也进了组字，但没上屏 —— 按空格或数字选候选（这是输入法的设计）。"
elif [ "$sent" -eq 0 ] && [ "$drop" -gt 0 ]; then
    echo "  → 判读：LB 收到了却上不了屏（指针在两键之间的缝里，snapped=-1）。"
elif [ "$sent" -eq 0 ]; then
    echo "  → 判读：键收到了、也没进组字、也没上屏 —— 看日志里 [pad] 后面 act= 是什么（none 说明没绑动作）。"
else
    echo "  → 判读：输入链是通的（有字节写给终端）。终端里没看到字就是输出口/焦点问题，看第 ③ 段。"
fi
