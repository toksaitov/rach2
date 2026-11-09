#!/bin/sh
# touchpad.sh: the Lenovo's Synaptics touchpad as Debian's kernel gives it, for
# 76-10c: its block of /proc/bus/input/devices, psmouse's log, and EVIOCGABS of
# each axis of its event device in the form `evprobe touchpad` prints Rach2's.
# Reads only; runs as root on Debian, from `mac.sh sh 'sh -s' < touchpad.sh`.
set -eu
echo "== kernel $(uname -r)"
echo "== /proc/bus/input/devices"
awk 'BEGIN { RS = ""; ORS = "\n\n" } /Synaptics/' /proc/bus/input/devices
echo "== kernel log"
dmesg | grep -i -E 'synaptics|psmouse|serio1' || true
echo "== EVIOCGABS"
python3 -I - << 'EOF'
import fcntl, glob, os, struct

EVIOCGBIT_ABS = 0x80084523  # EVIOCGBIT(EV_ABS, 8)
EVIOCGABS = 0x80184540      # plus the axis

for name in sorted(glob.glob('/sys/class/input/event*/device/name')):
    with open(name) as f:
        if f.read().strip() != 'SynPS/2 Synaptics TouchPad':
            continue
    fd = os.open('/dev/input/' + name.split('/')[4], os.O_RDONLY)
    bits = bytearray(8)
    fcntl.ioctl(fd, EVIOCGBIT_ABS, bits)
    mask = int.from_bytes(bits, 'little')
    for code in range(64):
        if mask >> code & 1:
            info = bytearray(24)
            fcntl.ioctl(fd, EVIOCGABS + code, info)
            value, lo, hi, fuzz, flat, res = struct.unpack('6i', info)
            print('EVPROBE: abs 0x%x min %d max %d fuzz %d flat %d res %d value %d'
                  % (code, lo, hi, fuzz, flat, res, value))
    os.close(fd)
EOF
