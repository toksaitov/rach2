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
ETHERTYPE_ARP = 0x0806
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
    ap.add_argument("--port", type=int, default=None,
                    help="the UDP destination port the datagram must carry")
    ap.add_argument("--dst-ip", default=None,
                    help="the IPv4 destination the datagram must carry")
    ap.add_argument("--routed-via", default=None, metavar="GATEWAY",
                    help="also require a frame to --routed-dst whose Ethernet "
                         "destination is the one frames to GATEWAY carry, which "
                         "is what proves a default route was used")
    ap.add_argument("--routed-dst", default=None, metavar="A.B.C.D",
                    help="the off-subnet destination to look for")
    ap.add_argument("--source", default=None, metavar="A.B.C.D",
                    help="require an IP packet the guest sent FROM this address, which "
                         "only a machine that has it can do")
    ap.add_argument("--owns", default=None, metavar="A.B.C.D",
                    help="require an ARP reply from the guest claiming this address, "
                         "which is the only way a machine can answer for one")
    a = ap.parse_args()

    if a.port is None and not (a.routed_via or a.routed_dst or a.owns or a.source):
        sys.stderr.write("bcast_check: nothing to check: give --port, or "
                         "--routed-via with --routed-dst\n")
        return 2

    if a.source:
        rc = check_source(a.capture, a.source)
        if rc != 0:
            return rc

    if a.owns:
        rc = check_owns(a.capture, a.owns)
        if rc != 0:
            return rc

    if a.routed_via or a.routed_dst:
        if not (a.routed_via and a.routed_dst):
            sys.stderr.write("bcast_check: --routed-via and --routed-dst go together\n")
            return 2
        rc = check_routed(a.capture, a.routed_via, a.routed_dst)
        if rc != 0:
            return rc

    if a.port is None:
        return 0
    return check_broadcast(a.capture, a.port, a.dst_ip)


def check_source(capture, address):
    """Require a packet the guest sent from `address`.

    A machine sends from an address only when the address is on its interface,
    so this is evidence that a lease was applied and not merely printed.
    """
    want = bytes(int(p) for p in address.split("."))
    seen = 0

    for frame in read_pcap(capture):
        if len(frame) < 14 + 20:
            continue
        if struct.unpack(">H", frame[12:14])[0] != ETHERTYPE_IP:
            continue
        if frame[14 + 12:14 + 16] == want:
            seen += 1

    if seen == 0:
        sys.stderr.write("bcast_check: nothing in %s was sent from %s, so no machine had "
                         "that address on an interface\n" % (capture, address))
        return 1
    print("bcast_check: ok: %d packet(s) were sent from %s" % (seen, address))
    return 0


def check_owns(capture, address):
    """Require an ARP reply claiming `address`.

    A machine answers ARP for an address only when the address is on its
    interface, so a reply is evidence the configuration was applied and not
    merely that a lease was printed. Nothing here can forge it: the reply is on
    the wire, in a capture of the wire.
    """
    want = bytes(int(p) for p in address.split("."))

    for frame in read_pcap(capture):
        if len(frame) < 14 + 28:
            continue
        if struct.unpack(">H", frame[12:14])[0] != ETHERTYPE_ARP:
            continue
        arp = frame[14:14 + 28]
        if arp[0:2] != b"\x00\x01" or arp[2:4] != b"\x08\x00":
            continue
        if arp[4] != 6 or arp[5] != 4:
            continue
        if arp[6:8] != b"\x00\x02":  # a reply, not a request
            continue
        if arp[14:18] == want:
            print("bcast_check: ok: the machine answered ARP for %s, so the address is on "
                  "its interface" % address)
            return 0

    sys.stderr.write("bcast_check: nothing answered ARP for %s in %s, so no machine has "
                     "that address\n" % (address, capture))
    return 1


def check_routed(capture, gateway, destination):
    """Require every packet for `destination` to leave for the gateway's MAC.

    The gateway's hardware address is learned from its own ARP reply, not from
    where this machine sent things: taking it from our own frames would compare
    a choice against itself, and a stack that broadcast everything, or that
    picked one wrong address consistently, would agree with itself.
    """
    gw = bytes(int(p) for p in gateway.split("."))
    dst = bytes(int(p) for p in destination.split("."))
    gw_macs = set()
    routed = []

    for frame in read_pcap(capture):
        if len(frame) < 14:
            continue
        etype = struct.unpack(">H", frame[12:14])[0]
        if etype == ETHERTYPE_ARP and len(frame) >= 14 + 28:
            arp = frame[14:14 + 28]
            # An Ethernet/IPv4 reply whose sender is the gateway.
            if arp[0:2] == b"\x00\x01" and arp[2:4] == b"\x08\x00" and \
               arp[4] == 6 and arp[5] == 4 and arp[6:8] == b"\x00\x02" and \
               arp[14:18] == gw:
                mac = arp[8:14]
                if mac != b"\xff\xff\xff\xff\xff\xff":
                    gw_macs.add(mac)
            continue
        if etype != ETHERTYPE_IP or len(frame) < 14 + 20:
            continue
        if frame[14 + 16:14 + 20] == dst:
            routed.append(frame[0:6])

    if not gw_macs:
        sys.stderr.write("bcast_check: %s never answered an ARP request, so there is no "
                         "hardware address to compare against\n" % gateway)
        return 1
    if not routed:
        sys.stderr.write("bcast_check: nothing was sent to %s at all\n" % destination)
        return 1
    for mac in routed:
        if mac not in gw_macs:
            sys.stderr.write("bcast_check: a packet for %s went to %s, and the gateway %s "
                             "answers at %s: it was not routed through the gateway\n"
                             % (destination, mac.hex(), gateway,
                                ", ".join(m.hex() for m in sorted(gw_macs))))
            return 1
    print("bcast_check: ok: %d packet(s) for %s went to %s, which is where %s answers ARP"
          % (len(routed), destination, sorted(gw_macs)[0].hex(), gateway))
    return 0


def check_broadcast(capture, port, dst_ip):
    """Require a broadcast datagram for `port` on the wire."""
    want_ip = bytes(int(p) for p in dst_ip.split(".")) if dst_ip else None
    found = 0
    seen = 0

    for frame in read_pcap(capture):
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
        if dport != port:
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
                         % (port, capture, seen))
        return 1
    print("bcast_check: ok: %d broadcast datagram(s) for port %d" % (found, port))
    return 0


if __name__ == "__main__":
    sys.exit(main())
