#!/usr/bin/env bash
# PC 侧：把本机网络共享给一根网线直连的 Switch（不需要路由器）
#
#   用法：sudo ./tools/pc-host-switch.sh [接口] [上网出口]
#   默认：接口 enx6c1ff76618b3（本机那张 RTL8153 的 MAC 名字），出口 wlan0
#
# 做完之后 Switch 上开 DHCP 就能拿到 192.168.50.10~50，网关/DNS 都是本机。
# 收工：sudo ./tools/pc-host-switch.sh --down
set -euo pipefail

IF=${1:-enx6c1ff76618b3}
WAN=${2:-wlan0}
SUBNET=192.168.50
ADDR=$SUBNET.1

if [[ "${1:-}" == "--down" ]]; then
    IF=${2:-$IF}
    sudo pkill -f "dnsmasq.*--interface=$IF" 2>/dev/null || true
    sudo nft delete rule ip nat postrouting oifname "$WAN" masquerade 2>/dev/null || true
    sudo ip addr flush dev "$IF" 2>/dev/null || true
    sudo nmcli device set "$IF" managed yes 2>/dev/null || true
    echo "已收工：$IF 交回 NetworkManager"
    exit 0
fi

# 1. 别让 NetworkManager 在这张网卡上自己跑 DHCP
sudo nmcli device set "$IF" managed no
sudo ip addr flush dev "$IF"
sudo ip addr add "$ADDR/24" dev "$IF"
sudo ip link set "$IF" up

# 2. 给对端发 DHCP（Switch 侧什么都不用配）
sudo pkill -f "dnsmasq.*--interface=$IF" 2>/dev/null || true
sudo dnsmasq --interface="$IF" --bind-interfaces \
    --dhcp-range=$SUBNET.10,$SUBNET.50,12h \
    --dhcp-option=3,"$ADDR" --dhcp-option=6,"$ADDR" \
    --dhcp-leasefile=/tmp/dnsmasq-switch.leases \
    --pid-file=/tmp/dnsmasq-switch.pid \
    --no-daemon >/tmp/dnsmasq-switch.log 2>&1 &

# 3. NAT：让对端能通过本机的 WiFi 上网
sudo sysctl -qw net.ipv4.ip_forward=1
sudo nft 'add table ip nat' 2>/dev/null || true
sudo nft 'add chain ip nat postrouting { type nat hook postrouting priority 100; }' 2>/dev/null || true
sudo nft delete rule ip nat postrouting oifname "$WAN" masquerade 2>/dev/null || true
sudo nft add rule ip nat postrouting oifname "$WAN" masquerade

echo "就绪：$IF = $ADDR/24，DHCP $SUBNET.10-50，出口 $WAN"
echo "Switch 侧：网卡设 DHCP（或静态 $SUBNET.2/24，网关 $ADDR）"
echo "租约：/tmp/dnsmasq-switch.leases    日志：/tmp/dnsmasq-switch.log"
echo "收工：sudo $0 --down"
