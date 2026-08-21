#!/usr/bin/env python3
"""
rawwire_check.py: read a capture and check what the raw-socket probe sent.

Two things about a raw socket cannot be seen from inside the machine, and both
are what this reads off the wire:

  * the hop limit is the socket's. `rawprobe` sends three ICMP echo requests
    with IP_TTL 1, 2 and 3 and three datagrams with 4, 5 and 6; a stack that
    accepted the option and ignored it sends six frames with the same limit,
    and traceroute would then answer at the first hop for every one of them.

  * the checksum is the caller's. The probe sends one echo request whose
    checksum field is 0x1234, which is wrong; a stack that recomputed it would
    repair it on the way out, and a program that deliberately sends a bad one
    would have no way to.

Usage: rawwire_check.py <capture.pcap>
Exit code 0 if both hold, 1 otherwise, with what was seen printed either way.
"""
import struct
import sys

PCAP_MAGIC_LE = 0xA1B2C3D4
PCAP_MAGIC_BE = 0xD4C3B2A1


def frames(path):
    """Yield each captured frame as bytes."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 24:
        raise SystemExit("rawwire_check: %s is too short to be a capture" % path)
    magic = struct.unpack("<I", data[:4])[0]
    if magic == PCAP_MAGIC_LE:
        end = "<"
    elif magic == PCAP_MAGIC_BE:
        end = ">"
    else:
        raise SystemExit("rawwire_check: %s is not a pcap file" % path)
    off = 24
    while off + 16 <= len(data):
        _, _, caplen, _ = struct.unpack(end + "IIII", data[off:off + 16])
        off += 16
        if off + caplen > len(data):
            break
        yield data[off:off + caplen]
        off += caplen


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: rawwire_check.py <capture.pcap>")
    path = sys.argv[1]

    icmp_ttls = set()
    udp_ttls = set()
    kept_checksum = False

    for frame in frames(path):
        if len(frame) < 34:
            continue
        if frame[12] != 0x08 or frame[13] != 0x00:
            continue  # not IPv4
        ihl = (frame[14] & 0x0F) * 4
        ttl = frame[22]
        proto = frame[23]
        body = 14 + ihl
        if proto == 1 and len(frame) >= body + 8:
            if frame[body] == 8:  # echo request, which only this machine sends
                icmp_ttls.add(ttl)
                if frame[body + 2] == 0x12 and frame[body + 3] == 0x34:
                    kept_checksum = True
        elif proto == 17 and len(frame) >= body + 8:
            dport = (frame[body + 2] << 8) | frame[body + 3]
            if 40000 <= dport <= 40010:
                udp_ttls.add(ttl)

    ok = True
    want_icmp = {1, 2, 3}
    want_udp = {4, 5, 6}
    if not want_icmp.issubset(icmp_ttls):
        print("rawwire_check: FAIL: the raw sends did not carry the hop limits "
              "they were given: saw %s" % sorted(icmp_ttls), file=sys.stderr)
        ok = False
    else:
        print("rawwire_check: ok: raw sends left with hop limits 1, 2 and 3")
    if not want_udp.issubset(udp_ttls):
        print("rawwire_check: FAIL: the datagram sends did not carry the hop "
              "limits they were given: saw %s" % sorted(udp_ttls), file=sys.stderr)
        ok = False
    else:
        print("rawwire_check: ok: datagram sends left with hop limits 4, 5 and 6")
    if not kept_checksum:
        print("rawwire_check: FAIL: the message with a wrong checksum did not "
              "leave with it, so the checksum is not the caller's", file=sys.stderr)
        ok = False
    else:
        print("rawwire_check: ok: the caller's checksum crossed the wire unchanged")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
