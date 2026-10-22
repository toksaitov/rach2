#!/usr/bin/env python3
"""sntpserver.py: a small SNTP server (RFC 4330) for the gnu family's ntpd case.

Usage: python3 -B sntpserver.py EPOCH PORT

The server binds UDP port PORT on 127.0.0.1, prints `sntpserver: port N` on
standard output once it is bound, and answers each client request with a time that is EPOCH, in seconds
since 1970, when it starts, and then advances at the host's rate. QEMU's
user network delivers a guest's datagram for 10.0.2.2 to the host's
loopback, so the guest reaches the server as 10.0.2.2:N. CLOCKREF requests
receive the same time scale, with host monotonic nanoseconds, for the
guest's bracketed comparison. The server exits when its parent does, so a
case that is killed leaves no server behind.
"""

import os
import select
import socket
import struct
import sys
import time

# Seconds from 1900, where NTP's timestamps begin, to 1970.
NTP_FROM_UNIX = 2208988800


def served(epoch, start):
    """Return the server's time: EPOCH at START, advancing with the host."""
    return epoch + time.monotonic() - start


def stamp(t):
    """Return T, seconds since 1970, as an NTP timestamp of 64 bits."""
    whole = int(t)
    return struct.pack("!II", whole + NTP_FROM_UNIX, int((t - whole) * 2**32))


def reply(request, received, sent):
    """Return the answer to REQUEST, a client's packet, or None for another.

    The answer is a stratum 1 server's, with no leap warning, the client's
    version and poll, and the client's transmit time as its originate time,
    which is how the client knows the answer for its own.
    """
    if len(request) < 48 or request[0] & 7 != 3:
        return None
    version = (request[0] >> 3) & 7
    head = struct.pack("!BBBbII4s", (version << 3) | 4, 1, request[2], -20, 0, 0, b"RCH2")
    return head + stamp(received) + request[40:48] + stamp(received) + stamp(sent)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: sntpserver.py EPOCH PORT")
    epoch = int(sys.argv[1])
    port = int(sys.argv[2])
    start = time.monotonic()
    parent = os.getppid()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", port))
    print("sntpserver: port %d" % sock.getsockname()[1], flush=True)
    while os.getppid() == parent:
        ready, _, _ = select.select([sock], [], [], 1.0)
        if not ready:
            continue
        request, client = sock.recvfrom(512)
        # The reference uses the configured epoch independently of NTP encoding.
        if request.startswith(b"CLOCKREF "):
            fields = request.split()
            if len(fields) == 2 and fields[1].isdigit():
                mono = time.monotonic_ns()
                real = int((epoch + time.monotonic() - start) * 1e9)
                sock.sendto(b"CLOCKREF %s %d %d" % (fields[1], mono, real), client)
            continue
        received = served(epoch, start)
        answer = reply(request, received, served(epoch, start))
        if answer is not None:
            sock.sendto(answer, client)
            print("sntpserver: answered %s:%d" % client, flush=True)


if __name__ == "__main__":
    main()
