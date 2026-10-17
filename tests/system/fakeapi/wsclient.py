"""wsclient.py: a WebSocket client for the fakeapi suite, run on the guest.

Usage: python3 wsclient.py HOST[:PORT] PATH CAFILE MESSAGE...

Connects to HOST on PORT, 443 when left out, over TLS verified against
CAFILE, asks PATH for a WebSocket, sends a ping, and then sends each MESSAGE
as text and waits for one message back. It prints `pong PAYLOAD` for the
ping's answer, each message it receives on a line of its own, and `close
CODE` when the server closes the WebSocket, which it answers with its own
close. The frames it sends are
masked, as RFC 6455 requires of a client; a message of 126 bytes or more
takes the 16-bit length.
"""

import base64
import hashlib
import os
import socket
import ssl
import struct
import sys

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def send(conn, opcode, data):
    """Send one masked frame."""
    mask = os.urandom(4)
    length = len(data)
    if length < 126:
        header = struct.pack("!BB", 0x80 | opcode, 0x80 | length)
    else:
        header = struct.pack("!BBH", 0x80 | opcode, 0x80 | 126, length)
    conn.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))


def exactly(rfile, count):
    """Read COUNT bytes, or exit when the connection ends first."""
    data = rfile.read(count)
    if len(data) != count:
        sys.exit("wsclient: the connection ended in a frame")
    return data


def receive(rfile):
    """Return the opcode and payload of the next frame, which is unmasked."""
    first = exactly(rfile, 2)
    if first[1] & 0x80:
        sys.exit("wsclient: the server masked a frame")
    length = first[1] & 0x7F
    if length == 126:
        length = struct.unpack("!H", exactly(rfile, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", exactly(rfile, 8))[0]
    return first[0] & 0x0F, exactly(rfile, length)


def main(address, path, cafile, messages):
    """Run the exchange the usage describes."""
    host, _, port = address.partition(":")
    context = ssl.create_default_context(cafile=cafile)
    conn = context.wrap_socket(socket.create_connection((host, int(port or 443)), timeout=60),
                               server_hostname=host)
    rfile = conn.makefile("rb")
    key = base64.b64encode(os.urandom(16)).decode()
    conn.sendall((f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n"
                  f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                  "Sec-WebSocket-Version: 13\r\n\r\n").encode())
    status = rfile.readline().decode().rstrip("\r\n")
    headers = {}
    while line := rfile.readline().decode().rstrip("\r\n"):
        name, _, value = line.partition(":")
        headers[name.strip().lower()] = value.strip()
    accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    if not status.startswith("HTTP/1.1 101 ") or headers.get("sec-websocket-accept") != accept:
        sys.exit(f"wsclient: the server did not accept the WebSocket: {status}")
    send(conn, 0x9, b"keepalive")
    opcode, data = receive(rfile)
    print(f"{'pong' if opcode == 0xA else opcode} {data.decode()}")
    for message in messages:
        send(conn, 0x1, message.encode())
        opcode, data = receive(rfile)
        if opcode != 0x1:
            sys.exit(f"wsclient: a frame of opcode {opcode} where a message was due")
        print(data.decode())
    opcode, data = receive(rfile)
    if opcode != 0x8:
        sys.exit(f"wsclient: a frame of opcode {opcode} where the close was due")
    send(conn, 0x8, data[:2])
    print(f"close {struct.unpack('!H', data[:2])[0]}")
    conn.close()


if __name__ == "__main__":
    if len(sys.argv) < 4:
        sys.exit("usage: python3 wsclient.py HOST[:PORT] PATH CAFILE MESSAGE...")
    main(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:])
