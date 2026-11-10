#!/usr/bin/python3
"""Checksum a screenshot's pixels as drmcapture checksums a scanout.

    frame.py PNG

Reads an 8-bit RGB or RGBA PNG, without interlacing, as weston-screenshooter
writes it, and prints

    WIDTHxHEIGHT crc32 0xCRC

where CRC is zlib's CRC-32 of each pixel's red, green and blue bytes, row by
row, without the alpha byte, the same sum drmcapture gives a frame in any of
its 32-bit formats. It uses Python's standard library alone, since neither
guest has an imaging library.
"""
import struct
import sys
import zlib


def paeth(a, b, c):
    """The Paeth predictor of the PNG specification, section 9.4."""
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def rows(path):
    """Give the PNG's width, height and its rows of RGB bytes."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit("frame: %s: not a PNG" % path)
    pos, idat, header = 8, [], None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
        pos += 12 + length
    width, height, depth, colour, _, _, interlace = header
    if depth != 8 or colour not in (2, 6) or interlace != 0:
        sys.exit("frame: %s: depth %d, colour type %d, interlace %d; only 8-bit RGB or RGBA "
                 "without interlacing is read" % (path, depth, colour, interlace))
    bpp = 3 if colour == 2 else 4
    raw = zlib.decompress(b"".join(idat))
    stride = width * bpp
    prior = bytearray(stride)
    out = []
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prior[x]
            c = prior[x - bpp] if x >= bpp else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                line[x] = (line[x] + paeth(a, b, c)) & 255
            elif kind != 0:
                sys.exit("frame: %s: row %d has filter %d" % (path, y, kind))
        prior = line
        rgb = bytearray(line)
        if bpp == 4:
            del rgb[3::4]
        out.append(bytes(rgb))
    return width, height, out


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: frame.py PNG")
    width, height, lines = rows(sys.argv[1])
    crc = 0
    for line in lines:
        crc = zlib.crc32(line, crc)
    print("%dx%d crc32 0x%08x" % (width, height, crc))


if __name__ == "__main__":
    main()
