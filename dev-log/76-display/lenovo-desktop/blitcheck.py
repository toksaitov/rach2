#!/usr/bin/env python3
"""blitcheck.py: fbdrm's copy to the screen, checked byte for byte and timed.

Run as root on a machine whose first card is fbdrm, with no master on it.
The script becomes the card's master, shows a dumb buffer of random bytes at
the connector's one mode, and reads the screen back through /dev/mem at the
address and pitch the kernel's log gives for fbdrm:

  whole    SETCRTC copies the whole buffer; every pixel of the screen is the
           buffer's.
  clips    the buffer is filled again and DIRTYFB names rectangles with odd
           edges; the screen has the new bytes inside them and the old
           everywhere else, so a copy that runs past a rectangle shows.
  time     COUNT whole-screen DIRTYFB calls, each a copy of every row, timed
           by the monotonic clock.

Usage: blitcheck.py [COUNT [LOG]]

COUNT is 100 when not given. LOG is a file of the kernel's log kept earlier,
for a machine whose log has since lost fbdrm's line; without it `dmesg` is
read. Prints `blitcheck: result: ...` lines, then `BLITCHECK OK` and exits 0, or
`BLITCHECK FAIL: what` and exits 1. Closing the card gives the screen back
to the console.
"""

import ctypes
import fcntl
import mmap
import os
import random
import re
import struct
import subprocess
import sys
import time


def iowr(nr, size):
    """The number of DRM's read-write ioctl `nr` whose argument has `size` bytes."""
    return (3 << 30) | (size << 16) | (ord("d") << 8) | nr


GETRESOURCES = iowr(0xA0, 64)
SETCRTC = iowr(0xA2, 104)
GETCONNECTOR = iowr(0xA7, 80)
ADDFB = iowr(0xAE, 28)
RMFB = iowr(0xAF, 4)
DIRTYFB = iowr(0xB1, 24)
CREATE_DUMB = iowr(0xB2, 32)
MAP_DUMB = iowr(0xB3, 16)
DESTROY_DUMB = iowr(0xB4, 4)
MODEINFO = 68
CONNECTED = 1


def fail(what):
    print("BLITCHECK FAIL: %s" % what)
    sys.exit(1)


def call(fd, request, fmt, *values):
    """One ioctl whose argument is `values` packed as `fmt`; its fields after."""
    arg = bytearray(struct.pack(fmt, *values))
    fcntl.ioctl(fd, request, arg, True)
    return struct.unpack(fmt, arg)


def first_ids(fd):
    """The first CRTC and the connectors of the card `fd`."""
    res = call(fd, GETRESOURCES, "=4Q8I", *([0] * 12))
    ncrtc, nconn = res[5], res[6]
    crtcs = (ctypes.c_uint32 * max(ncrtc, 1))()
    conns = (ctypes.c_uint32 * max(nconn, 1))()
    call(fd, GETRESOURCES, "=4Q8I", 0, ctypes.addressof(crtcs), ctypes.addressof(conns), 0,
         0, ncrtc, nconn, 0, 0, 0, 0, 0)
    if ncrtc == 0 or nconn == 0:
        fail("the card has %d CRTCs and %d connectors" % (ncrtc, nconn))
    return crtcs[0], list(conns)[:nconn]


def first_mode(fd, conns):
    """The first connected connector with a mode, and its first mode's bytes."""
    for conn in conns:
        got = call(fd, GETCONNECTOR, "=4Q12I", 0, 0, 0, 0, 0, 0, 0, 0, conn, 0, 0, 0, 0, 0, 0, 0)
        nmodes, connection = got[4], got[11]
        if connection != CONNECTED or nmodes == 0:
            continue
        modes = (ctypes.c_uint8 * (MODEINFO * nmodes))()
        call(fd, GETCONNECTOR, "=4Q12I", 0, ctypes.addressof(modes), 0, 0,
             nmodes, 0, 0, 0, conn, 0, 0, 0, 0, 0, 0, 0)
        return conn, bytes(modes[:MODEINFO])
    fail("no connected connector has a mode")
    return None


def screen_of_log(path):
    """The format, pitch and physical address of fbdrm's screen, from the
    kernel's log: the file `path`, or `dmesg` when it is None."""
    if path:
        with open(path, encoding="utf-8", errors="replace") as kept:
            log = kept.read()
    else:
        log = subprocess.run(["dmesg"], capture_output=True, text=True, check=True).stdout
    found = re.findall(r"fbdrm: registered: simpledrm (\d+)x(\d+), (\S+), pitch (\d+), at 0x([0-9a-fA-F]+)",
                       log)
    if not found:
        fail("the kernel's log has no fbdrm line")
    width, height, fourcc, pitch, base = found[-1]
    return int(width), int(height), fourcc, int(pitch), int(base, 16)


def read_screen(base, pitch, width, height):
    """The screen's rows, `width` pixels of 4 bytes each, through /dev/mem."""
    rows = []
    with open("/dev/mem", "rb", buffering=0) as mem:
        for y in range(height):
            want = width * 4
            row = b""
            mem.seek(base + y * pitch)
            while len(row) < want:
                part = mem.read(want - len(row))
                if not part:
                    fail("/dev/mem ends at row %d of the screen" % y)
                row += part
            rows.append(row)
    return rows


def differing(rows, want):
    """How many rows of the screen differ from `want`, and the first that does."""
    bad = [y for y in range(len(want)) if rows[y] != want[y]]
    return len(bad), (bad[0] if bad else -1)


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    width, height, fourcc, screen_pitch, base = screen_of_log(sys.argv[2] if len(sys.argv) > 2 else None)
    fd = os.open("/dev/dri/card0", os.O_RDWR | os.O_CLOEXEC)
    crtc, conns = first_ids(fd)
    conn, mode = first_mode(fd, conns)
    hdisplay, vdisplay = struct.unpack_from("=H", mode, 4)[0], struct.unpack_from("=H", mode, 14)[0]
    if (hdisplay, vdisplay) != (width, height):
        fail("the mode is %dx%d and the log's screen %dx%d" % (hdisplay, vdisplay, width, height))
    dumb = call(fd, CREATE_DUMB, "=6IQ", height, width, 32, 0, 0, 0, 0)
    handle, pitch, size = dumb[4], dumb[5], dumb[6]
    fb = call(fd, ADDFB, "=7I", 0, width, height, pitch, 32, 24, handle)[0]
    offset = call(fd, MAP_DUMB, "=IIQ", handle, 0, 0)[2]
    pixels = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=offset)
    rng = random.Random(2657)

    def fill():
        data = rng.randbytes(size)
        pixels[:] = data
        return [data[y * pitch:y * pitch + width * 4] for y in range(height)]

    old = fill()
    connector = ctypes.c_uint32(conn)
    fcntl.ioctl(fd, SETCRTC, struct.pack("=Q7I", ctypes.addressof(connector), 1, crtc, fb, 0, 0, 0, 1) + mode)
    print("blitcheck: result: screen %dx%d %s, pitch %d, at 0x%x; buffer pitch %d"
          % (width, height, fourcc, screen_pitch, base, pitch))
    checked = fourcc == "XR24"
    if checked:
        bad, first = differing(read_screen(base, screen_pitch, width, height), old)
        print("blitcheck: result: whole: %d of %d rows differ" % (bad, height))
        if bad:
            fail("whole: row %d of the screen is not the buffer's" % first)
        new = fill()
        rects = [(3, 5, width - 7, 9), (width // 2 + 1, height // 3, width // 2 + 2, height - 1),
                 (0, height - 1, width, height), (width - 1, 0, width, height - 2),
                 (17, height // 2, 1041, height // 2 + 1)]
        clips = (ctypes.c_uint16 * (4 * len(rects)))(*[v for rect in rects for v in rect])
        call(fd, DIRTYFB, "=4IQ", fb, 0, 0, len(rects), ctypes.addressof(clips))
        want = [bytearray(row) for row in old]
        for x1, y1, x2, y2 in rects:
            for y in range(y1, y2):
                want[y][x1 * 4:x2 * 4] = new[y][x1 * 4:x2 * 4]
        bad, first = differing(read_screen(base, screen_pitch, width, height), want)
        print("blitcheck: result: clips: %d rectangles, %d of %d rows differ" % (len(rects), bad, height))
        if bad:
            fail("clips: row %d of the screen is wrong" % first)
    else:
        print("blitcheck: result: the screen is %s, not XR24: bytes not compared" % fourcc)
    spans = []
    for _ in range(count):
        start = time.monotonic_ns()
        call(fd, DIRTYFB, "=4IQ", fb, 0, 0, 0, 0)
        spans.append(time.monotonic_ns() - start)
    spans.sort()
    print("blitcheck: result: time: %d whole-screen copies of %d bytes: mean %.3f ms, median %.3f ms, "
          "least %.3f ms, most %.3f ms"
          % (count, width * 4 * height, sum(spans) / count / 1e6, spans[count // 2] / 1e6,
             spans[0] / 1e6, spans[-1] / 1e6))
    pixels.close()
    call(fd, RMFB, "=I", fb)
    call(fd, DESTROY_DUMB, "=I", handle)
    os.close(fd)
    print("BLITCHECK OK")


if __name__ == "__main__":
    try:
        main()
    except OSError as err:
        fail("%s" % err)
