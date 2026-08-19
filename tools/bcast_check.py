#!/usr/bin/env python3
"""
bcast_check.py: read a QEMU packet capture and find a broadcast UDP datagram.

A socket that asked to broadcast and then sent to a broadcast address must put
a frame on the wire addressed to every machine on it. The return value of the
send does not prove that: the transmit path is asynchronous, and a datagram
that was routed at a gateway, or ARPed for and dropped, would leave the same
zero behind. QEMU's filter-dump object writes every frame the guest sends to a
libpcap file, so this reads that file and requires the frame itself.

Usage:
    bcast_check.py CAPTURE --port PORT [--dst-ip A.B.C.D]
"""
import argparse
import struct
import sys

ETHERTYPE_IP = 0x0800
IPPROTO_UDP = 17


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--port", type=int, required=True,
                    help="the UDP destination port the datagram must carry")
    ap.add_argument("--dst-ip", default=None,
                    help="the IPv4 destination the datagram must carry")
    a = ap.parse_args()

    want_ip = None
    if a.dst_ip:
        want_ip = bytes(int(p) for p in a.dst_ip.split("."))

    found = 0
    seen = 0
    for frame in read_pcap(a.capture):
        if len(frame) < 14 + 20 + 8:
            continue
        if struct.unpack(">H", frame[12:14])[0] != ETHERTYPE_IP:
            continue
        ihl = (frame[14] & 0x0F) * 4
        if ihl < 20 or len(frame) < 14 + ihl + 8:
            continue
        if frame[14 + 9] != IPPROTO_UDP:
            continue
        dport = struct.unpack(">H", frame[14 + ihl + 2:14 + ihl + 4])[0]
        if dport != a.port:
            continue
        seen += 1
        if frame[0:6] != b"\xff\xff\xff\xff\xff\xff":
            continue
        if want_ip is not None and frame[14 + 16:14 + 20] != want_ip:
            continue
        found += 1

    if found == 0:
        sys.stderr.write("bcast_check: no broadcast datagram for port %d in %s "
                         "(%d datagram(s) for that port went somewhere else)\n"
                         % (a.port, a.capture, seen))
        return 1
    print("bcast_check: ok: %d broadcast datagram(s) for port %d" % (found, a.port))
    return 0


if __name__ == "__main__":
    sys.exit(main())
