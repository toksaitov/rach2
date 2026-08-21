#!/usr/bin/env python3
"""
netconsole_check.py: read a QEMU packet capture and check the netconsole frames.

The kernel's netconsole (rach2/sys/network/netconsole.c) repeats the serial
stream into broadcast Ethernet frames of EtherType 0x88b5. QEMU's filter-dump
object writes every frame the guest sends to a libpcap file, so a boot with
`rach2.netconsole` on the command line leaves the whole log on disk in the form
a machine with no serial port would put it on the wire.

This reads that file and has the frames to the contract the header states:
one boot id, sequence numbers from zero with no gaps, a length that matches the
frame, a checksum that sums to zero, and a broadcast destination. It then
requires a caller-named string in the reassembled log, which is what proves the
buffered output from before the device existed was carried, not lost.

Usage:
    netconsole_check.py CAPTURE [--expect TEXT ...] [--min-frames N] [--print]
"""
import argparse
import struct
import sys

ETHERTYPE = 0x88B5
HDR_LEN = 16
MAGIC = b"RCH2"


def read_pcap(path):
    """Yield each captured frame's bytes, from a classic or nanosecond pcap."""
    with open(path, "rb") as f:
        header = f.read(24)
        if len(header) < 24:
            raise ValueError("capture is shorter than a pcap file header")
        magic = header[:4]
        if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
            endian = "<"
        elif magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
            endian = ">"
        else:
            raise ValueError("not a pcap capture (magic %s)" % magic.hex())
        link = struct.unpack(endian + "I", header[20:24])[0]
        if link != 1:
            raise ValueError("capture link type is %d, not Ethernet" % link)
        while True:
            rec = f.read(16)
            if len(rec) < 16:
                return
            _, _, caplen, _ = struct.unpack(endian + "IIII", rec)
            data = f.read(caplen)
            if len(data) < caplen:
                return
            yield data


def ones_complement_sum(buf):
    """The IP header checksum over `buf`; zero means the frame is intact."""
    total = 0
    for i in range(0, len(buf) - 1, 2):
        total += (buf[i] << 8) | buf[i + 1]
        if total > 0xFFFF:
            total -= 0xFFFF
    if len(buf) & 1:
        total += buf[-1] << 8
        if total > 0xFFFF:
            total -= 0xFFFF
    return (~total) & 0xFFFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--expect", action="append", default=[],
                    help="text the reassembled log must contain; repeatable")
    ap.add_argument("--min-frames", type=int, default=1)
    ap.add_argument("--print", action="store_true", dest="show",
                    help="write the reassembled log to stdout")
    a = ap.parse_args()

    try:
        frames = list(read_pcap(a.capture))
    except (OSError, ValueError) as e:
        sys.stderr.write("netconsole_check: %s\n" % e)
        return 2

    ours = []
    for frame in frames:
        if len(frame) < 14:
            continue
        if struct.unpack(">H", frame[12:14])[0] == ETHERTYPE:
            ours.append(frame)

    fails = []
    if len(ours) < a.min_frames:
        fails.append("only %d netconsole frames in %d captured, wanted %d"
                     % (len(ours), len(frames), a.min_frames))

    log = bytearray()
    boot_ids = set()
    seqs = []
    src_macs = set()
    for i, frame in enumerate(ours):
        body = frame[14:]
        if len(body) < HDR_LEN:
            fails.append("frame %d is shorter than the netconsole header" % i)
            continue
        if body[:4] != MAGIC:
            fails.append("frame %d does not carry the magic" % i)
            continue
        boot_id, seq, length = struct.unpack(">IIH", body[4:14])
        # A frame the chip padded to the 60-byte minimum has trailing zeros
        # past the payload; the length field, not the frame size, is the log.
        if HDR_LEN + length > len(body):
            fails.append("frame %d claims %d bytes but has %d"
                         % (i, length, len(body) - HDR_LEN))
            continue
        if ones_complement_sum(body[:HDR_LEN + length]) != 0:
            fails.append("frame %d fails its checksum" % i)
        if frame[:6] != b"\xff" * 6:
            fails.append("frame %d is not broadcast" % i)
        src_macs.add(frame[6:12])
        boot_ids.add(boot_id)
        seqs.append(seq)
        log += body[HDR_LEN:HDR_LEN + length]

    if len(boot_ids) > 1:
        fails.append("frames carry %d different boot ids" % len(boot_ids))
    if 0 in boot_ids:
        fails.append("a frame has a zero boot id")
    if len(src_macs) > 1:
        fails.append("frames carry %d different source addresses" % len(src_macs))
    if seqs and seqs != list(range(len(seqs))):
        fails.append("sequence numbers are not 0..%d in order: %s"
                     % (len(seqs) - 1, seqs[:16]))

    text = log.decode("latin-1")
    for want in a.expect:
        if want not in text:
            fails.append("the log does not contain %r" % want)

    if a.show:
        sys.stdout.write(text)

    if fails:
        for f in fails:
            sys.stderr.write("netconsole_check: %s\n" % f)
        return 1
    mac = ":".join("%02x" % b for b in next(iter(src_macs))) if src_macs else "none"
    sys.stderr.write("netconsole_check: ok: %d frames, %d bytes, boot id 0x%08x, from %s\n"
                     % (len(ours), len(log), next(iter(boot_ids)) if boot_ids else 0, mac))
    return 0


if __name__ == "__main__":
    sys.exit(main())
