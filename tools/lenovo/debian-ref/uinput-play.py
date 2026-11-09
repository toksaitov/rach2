"""uinput-play.py: a virtual keyboard and mouse that plays a list of steps,
for the Crysis steps of the 80-2 session (run2 step 9's tool).

Usage: python3 -I uinput-play.py STEP...
  wait:S         sleep S seconds
  down:K, up:K   press or release K (a KEY_ or BTN_ name below)
  tap:K          press and release K
  move:DX,DY     relative pointer motion
  at:X,Y         put the pointer at panel pixel X,Y (a second, absolute device)
  click:B        press and release button B on the absolute device
  type:TEXT      type lowercase letters, digits, '_', '.', ' '
  shot:PATH      grab the panel's scanout to PATH (ffmpeg kmsgrab)
The device is made through /dev/uinput, so the compositor (libinput) sees it
like a USB keyboard with a mouse.
"""
import fcntl
import os
import struct
import subprocess
import sys
import time

CODES = {"KEY_ESC": 1, "KEY_ENTER": 28, "KEY_C": 46, "KEY_V": 47, "KEY_F": 33,
         "KEY_GRAVE": 41, "KEY_BACKSPACE": 14, "KEY_SPACE": 57, "KEY_W": 17, "KEY_1": 2, "KEY_2": 3,
         "BTN_LEFT": 0x110, "BTN_RIGHT": 0x111, "BTN_MIDDLE": 0x112,
         "KEY_LEFTSHIFT": 42, "KEY_MINUS": 12, "KEY_DOT": 52}
ROWS = {"qwertyuiop": 16, "asdfghjkl": 30, "zxcvbnm": 44, "1234567890": 2}
for row, base in ROWS.items():
    for i, ch in enumerate(row):
        CODES.setdefault("KEY_" + ch.upper(), base + i)
EV_SYN, EV_KEY, EV_REL = 0, 1, 2
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_RELBIT = 0x40045564, 0x40045565, 0x40045566
UI_DEV_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405c5503, 0x5501, 0x5502
UI_SET_ABSBIT, UI_ABS_SETUP, EV_ABS = 0x40045567, 0x401c5504, 3

fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
for ev in (EV_KEY, EV_REL):
    fcntl.ioctl(fd, UI_SET_EVBIT, ev)
for c in CODES.values():
    fcntl.ioctl(fd, UI_SET_KEYBIT, c)
for r in (0, 1):
    fcntl.ioctl(fd, UI_SET_RELBIT, r)
fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack("HHHH80sI", 3, 0x1234, 0x5678, 1,
                                          b"ref80 virtual keyboard and mouse", 0))
fcntl.ioctl(fd, UI_DEV_CREATE)
# An absolute pointer, as QEMU's USB tablet is, for exact positions.
fa = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
for ev in (EV_KEY, EV_ABS):
    fcntl.ioctl(fa, UI_SET_EVBIT, ev)
for c in (0x110, 0x111, 0x112):
    fcntl.ioctl(fa, UI_SET_KEYBIT, c)
for a, m in ((0, 1365), (1, 767)):
    fcntl.ioctl(fa, UI_SET_ABSBIT, a)
    fcntl.ioctl(fa, UI_ABS_SETUP, struct.pack("Hxx6i", a, 0, 0, m, 0, 0, 0))
fcntl.ioctl(fa, UI_DEV_SETUP, struct.pack("HHHH80sI", 3, 0x1234, 0x5679, 1,
                                          b"ref80 virtual tablet", 0))
fcntl.ioctl(fa, UI_DEV_CREATE)
time.sleep(2)


def emit(t, c, v, f=None):
    os.write(fd if f is None else f, struct.pack("llHHi", 0, 0, t, c, v))


def syn(f=None):
    emit(EV_SYN, 0, 0, f)


try:
    for step in sys.argv[1:]:
        op, _, arg = step.partition(":")
        print("%.3f %s" % (time.time(), step), flush=True)
        if op == "wait":
            time.sleep(float(arg))
        elif op in ("down", "up"):
            emit(EV_KEY, CODES[arg], 1 if op == "down" else 0)
            syn()
        elif op == "tap":
            emit(EV_KEY, CODES[arg], 1)
            syn()
            time.sleep(0.1)
            emit(EV_KEY, CODES[arg], 0)
            syn()
        elif op == "type":
            for ch in arg:
                if ch == "_":
                    seq = ["KEY_LEFTSHIFT", "KEY_MINUS"]
                else:
                    seq = [{" ": "KEY_SPACE", ".": "KEY_DOT"}.get(ch, "KEY_" + ch.upper())]
                for k in seq:
                    emit(EV_KEY, CODES[k], 1)
                    syn()
                time.sleep(0.03)
                for k in reversed(seq):
                    emit(EV_KEY, CODES[k], 0)
                    syn()
                time.sleep(0.03)
        elif op == "at":
            x, y = (int(v) for v in arg.split(","))
            emit(EV_ABS, 0, x, fa)
            emit(EV_ABS, 1, y, fa)
            syn(fa)
        elif op == "click":
            emit(EV_KEY, CODES[arg], 1, fa)
            syn(fa)
            time.sleep(0.1)
            emit(EV_KEY, CODES[arg], 0, fa)
            syn(fa)
        elif op == "move":
            dx, dy = (int(v) for v in arg.split(","))
            emit(EV_REL, 0, dx)
            emit(EV_REL, 1, dy)
            syn()
        elif op == "shot":
            subprocess.run(["ffmpeg", "-loglevel", "error", "-device", "/dev/dri/card0", "-f", "kmsgrab",
                            "-i", "-", "-vf", "hwmap=derive_device=vaapi,scale_vaapi=w=1366:h=768:format=nv12,"
                            "hwdownload,format=nv12", "-frames:v", "1", "-y", arg],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
        time.sleep(0.05)
finally:
    for c in CODES.values():
        emit(EV_KEY, c, 0)
    syn()
    time.sleep(0.2)
    fcntl.ioctl(fd, UI_DEV_DESTROY)
    os.close(fd)
    fcntl.ioctl(fa, UI_DEV_DESTROY)
    os.close(fa)
