#!/bin/bash
# Switch 上的 overlay 形态一键起：Xorg → openbox → alacritty → 我们的覆盖层
# 依赖：openbox、alacritty（apt），~/.config/openbox/rc.xml 里加了 A-Up/A-Down 与 sit-fixed 规则
set +e
export DISPLAY=:0
export XDG_RUNTIME_DIR=/run/user/$(id -u)
REPO=$(cd "$(dirname "$0")/.." && pwd)

# 1) X（不经过 sddm）
if ! pgrep -f '/usr/lib/xorg/Xorg' > /dev/null; then
    sudo -n /usr/lib/xorg/Xorg :0 vt2 -nolisten tcp > /tmp/xorg.log 2>&1 &
    sleep 7
fi
# 2) 窗口管理器
pgrep -f '^openbox' > /dev/null || { setsid openbox > /tmp/openbox.log 2>&1 & sleep 3; }
# 3) 终端（几何由 openbox 的 sit-fixed 规则钉住）
pgrep -f 'bin/alacritty' > /dev/null || {
    setsid alacritty -e "${SHELL:-/bin/bash}" > /tmp/alacritty.log 2>&1 &
    sleep 6
}
# 4) 我们的覆盖层（挖洞 + 顶栏 + 键盘）
pkill -f 'switch-ime-term --backend sdl'
"$REPO/build/switch-ime-term" --backend sdl --overlay-env 2>/dev/null &
