"""Serve Rach2's call counters to one unprivileged browser probe.

Usage: python3 -I calls.py SOCKET UID

Run as root with SOCKET in a root-owned, searchable directory. Only UID
may connect. Each connection receives one read of bkl/calls and ends.
"""
import os
from pathlib import Path
import socket
import sys


def serve(path, uid):
    """Listen at a new path, giving it to uid once the listener is ready."""
    counters = Path("/sys/kernel/debug/bkl/calls")
    os.umask(0o177)
    with socket.socket(socket.AF_UNIX) as server:
        server.bind(path)
        try:
            server.listen()
            os.chown(path, uid, -1)
            while True:
                client, _ = server.accept()
                with client:
                    client.settimeout(2)
                    try:
                        client.sendall(counters.read_bytes())
                    except OSError as err:
                        print("calls: " + str(err), file=sys.stderr)
        finally:
            os.unlink(path)


if __name__ == "__main__":
    serve(sys.argv[1], int(sys.argv[2]))
