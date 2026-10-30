#!/bin/sh
# Launched only in a dedicated SSH PTY. No network configuration is changed.
LC_ALL=C
export LC_ALL
interval=$1
case "$interval" in ''|*[!0-9.]*) exit 2 ;; esac
awk -v n="$interval" 'BEGIN { exit !(n > 0) }' || exit 2
trap 'exit 0' HUP INT TERM
boot=unknown
[ ! -r /proc/sys/kernel/random/boot_id ] || read -r boot < /proc/sys/kernel/random/boot_id
read -r stamp rest < /proc/uptime || exit 3
printf 'GX_CLOCK %s %s\n' "$boot" "$stamp"
while :; do
    for iface in rmnet_data0 usb0; do
        base=/sys/class/net/$iface
        read -r begin rest < /proc/uptime || exit 3
        if [ -r "$base/ifindex" ] && [ -r "$base/statistics/tx_bytes" ] && [ -r "$base/statistics/rx_bytes" ]; then
            if read -r idx < "$base/ifindex" && read -r tx < "$base/statistics/tx_bytes" && read -r rx < "$base/statistics/rx_bytes"; then
                read -r end rest < /proc/uptime || exit 3
                printf 'GX_NET %s %s %s %s %s %s %s\n' "$boot" "$iface" "$idx" "$begin" "$end" "$tx" "$rx"
            else
                printf 'GX_MISSING %s %s\n' "$iface" "$begin"
            fi
        else
            printf 'GX_MISSING %s %s\n' "$iface" "$begin"
        fi
    done
    sleep "$interval" || exit 4
done
