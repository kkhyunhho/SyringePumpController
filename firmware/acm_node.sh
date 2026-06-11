#!/usr/bin/env bash
#
# Recreate the /dev/ttyACM* node for the ESP32-S3-BOX-3 when udev is
# absent (e.g. inside the dev container).
#
# Plugging in or resetting the board enumerates its USB-Serial/JTAG
# interface in sysfs, but with no udev running the matching /dev node is
# never created, so `idf.py -p /dev/ttyACM0 flash` fails with
# "No such file or directory". This script finds the Espressif (vid
# 303a) CDC-ACM tty in sysfs and mknod's the corresponding /dev entry
# with the kernel-assigned major:minor.
#
# Idempotent — safe to re-run after every replug/reset. The USB topology
# (e.g. 3-2.4) and thus the ACM index can change between plugs; this
# script always reads the current values from sysfs rather than assuming
# a fixed port. Needs root (mknod).
#
# Usage:
#   sudo firmware/acm_node.sh        # or just `bash firmware/acm_node.sh` as root
#   idf.py -p /dev/ttyACM0 flash monitor
#
set -euo pipefail

ESP_VID="303a" # Espressif Systems — USB JTAG/serial debug unit
found=0

for sys in /sys/class/tty/ttyACM*; do
    [ -e "$sys" ] || continue
    name=$(basename "$sys")
    usbdev=$(readlink -f "$sys/device/.." 2>/dev/null) || continue
    vid=$(cat "$usbdev/idVendor" 2>/dev/null || echo "")
    [ "$vid" = "$ESP_VID" ] || continue
    found=1

    node="/dev/$name"
    devnums=$(cat "$sys/dev") # e.g. 166:0
    major=${devnums%%:*}
    minor=${devnums##*:}
    topo=$(basename "$usbdev")
    prod=$(cat "$usbdev/product" 2>/dev/null || echo "?")

    if [ -e "$node" ]; then
        echo "exists:  $node  (USB $topo, $prod)"
    else
        mknod "$node" c "$major" "$minor"
        chgrp dialout "$node" 2>/dev/null || true
        chmod 660 "$node"
        echo "created: $node  c $major $minor  (USB $topo, $prod)"
    fi
done

if [ "$found" = 0 ]; then
    echo "no Espressif ($ESP_VID) ACM device in sysfs — is the BOX-3 plugged in?" >&2
    exit 1
fi
