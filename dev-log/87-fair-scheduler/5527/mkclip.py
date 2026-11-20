"""mkclip.py OUT [SECONDS]: a 1920x1080 30 fps VP9 WebM clip of a moving
pattern with noise at a target of 6 Mbit/s (7.4 came out), encoded through
the host's libvpx by ctypes, for 5527's QEMU plays. No audio track."""
import ctypes
import os
import struct
import sys

out = sys.argv[1]
secs = int(sys.argv[2]) if len(sys.argv) > 2 else 60
W, H, FPS = 1920, 1080, 30
vpx = ctypes.CDLL("libvpx.so.12")
vpx.vpx_codec_vp9_cx.restype = ctypes.c_void_p
vpx.vpx_img_alloc.restype = ctypes.c_void_p
vpx.vpx_codec_get_cx_data.restype = ctypes.c_void_p
vpx.vpx_codec_error.restype = ctypes.c_char_p
iface = ctypes.c_void_p(vpx.vpx_codec_vp9_cx())

cfg = ctypes.create_string_buffer(8192)
if vpx.vpx_codec_enc_config_default(iface, cfg, 0) != 0:
    sys.exit("mkclip: no default configuration")


def geti(off):
    return struct.unpack_from("<I", cfg, off)[0]


def seti(off, v):
    struct.pack_into("<I", cfg, off, v)


# vpx_codec_enc_cfg_t: g_usage 0, g_threads 4, g_profile 8, g_w 12, g_h 16,
# g_timebase 28/32, g_lag_in_frames 44, rc_end_usage 72, rc_target_bitrate 112.
assert geti(112) == 256, "rc_target_bitrate not at 112: %d" % geti(112)
seti(4, 8)
seti(12, W)
seti(16, H)
seti(28, 1)
seti(32, FPS)
seti(44, 0)
seti(72, 0)
seti(112, 6000)

ctx = ctypes.create_string_buffer(1024)
for ver in range(1, 200):
    rc = vpx.vpx_codec_enc_init_ver(ctx, iface, cfg, 0, ver)
    if rc != 3:
        break
if rc != 0:
    sys.exit("mkclip: init: %d" % rc)
vpx.vpx_codec_control_(ctx, 13, ctypes.c_int(7))  # VP8E_SET_CPUUSED
vpx.vpx_codec_control_(ctx, 33, ctypes.c_int(2))  # VP9E_SET_TILE_COLUMNS

img = vpx.vpx_img_alloc(None, 0x102, W, H, 32)  # VPX_IMG_FMT_I420
planes = (ctypes.c_void_p * 4).from_address(img + 48)
strides = (ctypes.c_int * 4).from_address(img + 80)

# A texture twice the frame's width, panned a few pixels a frame, and eight
# frames of low noise XORed over it.
TW = 2 * W
tex = bytearray(TW * H)
for y in range(H):
    row = memoryview(tex)[y * TW:(y + 1) * TW]
    for x in range(TW):
        row[x] = (((x // 40) ^ (y // 40)) & 1) * 120 + ((x * 3 + y * 5) & 63) + 30
ctex = bytearray(TW // 2 * (H // 2))
for y in range(H // 2):
    for x in range(TW // 2):
        ctex[y * (TW // 2) + x] = 128 + (((x // 25) + (y // 25)) % 5) * 12 - 24
noise = [int.from_bytes(bytes(b & 7 for b in os.urandom(W * H)), "little") for _ in range(8)]


def ebml_id(i):
    n = (i.bit_length() + 7) // 8
    return i.to_bytes(n, "big")


def ebml_size(n):
    for length in range(1, 9):
        if n < (1 << (7 * length)) - 1:
            return (n | (1 << (7 * length))).to_bytes(length, "big")
    raise ValueError(n)


def el(i, payload):
    return ebml_id(i) + ebml_size(len(payload)) + payload


def uint(i, v):
    n = max(1, (v.bit_length() + 7) // 8)
    return el(i, v.to_bytes(n, "big"))


clusters = []
blocks = []
cluster_tc = 0


def flush():
    if blocks:
        clusters.append(el(0x1F43B675, uint(0xE7, cluster_tc) + b"".join(blocks)))
        blocks.clear()


def take():
    global cluster_tc
    it = ctypes.c_void_p(0)
    while True:
        p = vpx.vpx_codec_get_cx_data(ctx, ctypes.byref(it))
        if not p:
            return
        kind = ctypes.c_int.from_address(p).value
        if kind != 0:
            continue
        buf = ctypes.c_void_p.from_address(p + 8).value
        sz = ctypes.c_size_t.from_address(p + 16).value
        pts = ctypes.c_int64.from_address(p + 24).value
        flags = ctypes.c_uint32.from_address(p + 40).value
        ms = pts * 1000 // FPS
        key = flags & 1
        if key or ms - cluster_tc > 30000 or not blocks:
            flush()
            cluster_tc = ms
        data = ctypes.string_at(buf, sz)
        hdr = b"\x81" + struct.pack(">h", ms - cluster_tc) + (b"\x80" if key else b"\x00")
        blocks.append(el(0xA3, hdr + data))


n = secs * FPS
for f in range(n):
    off = (f * 4) % W
    y0 = planes[0]
    ys = strides[0]
    frame = b"".join(bytes(tex[y * TW + off:y * TW + off + W]) for y in range(H))
    frame = (int.from_bytes(frame, "little") ^ noise[f % 8]).to_bytes(W * H, "little")
    for y in range(H):
        ctypes.memmove(y0 + y * ys, frame[y * W:(y + 1) * W], W)
    for p in (1, 2):
        cs = strides[p]
        base = planes[p]
        coff = off // 2 if p == 1 else (W // 2 - off // 2) % (W // 2)
        for y in range(H // 2):
            s = y * (TW // 2) + coff
            ctypes.memmove(base + y * cs, bytes(ctex[s:s + W // 2]), W // 2)
    if vpx.vpx_codec_encode(ctx, ctypes.c_void_p(img), ctypes.c_int64(f), ctypes.c_ulong(1), 0,
                            ctypes.c_ulong(1)) != 0:
        sys.exit("mkclip: encode: %s" % vpx.vpx_codec_error(ctx))
    take()
    if f % 300 == 0:
        print("mkclip: frame %d" % f, flush=True)
vpx.vpx_codec_encode(ctx, None, ctypes.c_int64(n), ctypes.c_ulong(1), 0, ctypes.c_ulong(1))
take()
flush()

header = el(0x1A45DFA3, uint(0x4286, 1) + uint(0x42F7, 1) + uint(0x42F2, 4) + uint(0x42F3, 8) +
            el(0x4282, b"webm") + uint(0x4287, 4) + uint(0x4285, 2))
info = el(0x1549A966, uint(0x2AD7B1, 1000000) + el(0x4489, struct.pack(">d", secs * 1000.0)) +
          el(0x4D80, b"mkclip") + el(0x5741, b"mkclip"))
video = el(0xE0, uint(0xB0, W) + uint(0xBA, H))
track = el(0xAE, uint(0xD7, 1) + uint(0x73C5, 1) + uint(0x83, 1) + el(0x86, b"V_VP9") +
           uint(0x23E383, 1000000000 // FPS) + video)
tracks = el(0x1654AE6B, track)
segment = el(0x18538067, info + tracks + b"".join(clusters))
with open(out, "wb") as fh:
    fh.write(header + segment)
print("mkclip: wrote %s, %d bytes" % (out, len(header) + len(segment)))
