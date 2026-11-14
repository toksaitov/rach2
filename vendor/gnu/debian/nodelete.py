#!/usr/bin/env python3
"""Mark a little-endian ELF shared library DF_1_NODELETE, so that dlclose never unmaps it."""

import struct
import sys

PT_DYNAMIC = 2
DT_NULL = 0
DT_FLAGS_1 = 0x6ffffffb
DF_1_NODELETE = 0x8


def nodelete(path):
    """Set DF_1_NODELETE in the DT_FLAGS_1 of PATH's dynamic segment.

    An existing DT_FLAGS_1 gains the flag; otherwise the entry takes the
    place of the first DT_NULL, which needs a second DT_NULL after it inside
    the segment's file size, as the linker's spare entries give. Returns a
    message, or raises ValueError when the file is not a library this
    function can change in place.
    """
    with open(path, 'r+b') as f:
        data = bytearray(f.read())
        if data[:4] != b'\x7fELF' or data[5] != 1:
            raise ValueError('not a little-endian ELF file')
        wide = {1: False, 2: True}.get(data[4])
        if wide is None:
            raise ValueError('unknown ELF class %d' % data[4])
        if wide:
            phoff, = struct.unpack_from('<Q', data, 0x20)
            phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
            entry, fmt = 16, '<qQ'
        else:
            phoff, = struct.unpack_from('<I', data, 0x1c)
            phentsize, phnum = struct.unpack_from('<HH', data, 0x2a)
            entry, fmt = 8, '<iI'
        dynamic = None
        for i in range(phnum):
            at = phoff + i * phentsize
            if struct.unpack_from('<I', data, at)[0] != PT_DYNAMIC:
                continue
            if wide:
                offset, = struct.unpack_from('<Q', data, at + 8)
                size, = struct.unpack_from('<Q', data, at + 32)
            else:
                offset, = struct.unpack_from('<I', data, at + 4)
                size, = struct.unpack_from('<I', data, at + 16)
            dynamic = (offset, size)
        if dynamic is None:
            raise ValueError('no dynamic segment')
        offset, size = dynamic
        count = size // entry
        for i in range(count):
            tag, value = struct.unpack_from(fmt, data, offset + i * entry)
            if tag == DT_FLAGS_1:
                struct.pack_into(fmt, data, offset + i * entry, tag, value | DF_1_NODELETE)
                break
            if tag == DT_NULL:
                if i + 1 >= count or struct.unpack_from(fmt, data, offset + (i + 1) * entry)[0] != DT_NULL:
                    raise ValueError('no spare dynamic entry')
                struct.pack_into(fmt, data, offset + i * entry, DT_FLAGS_1, DF_1_NODELETE)
                break
        else:
            raise ValueError('no DT_NULL in the dynamic segment')
        f.seek(0)
        f.write(data)
    return 'DF_1_NODELETE set in dynamic entry %d of %d' % (i, count)


def main():
    """Mark each library named on the command line."""
    if len(sys.argv) < 2:
        raise SystemExit('usage: nodelete.py LIBRARY...')
    for path in sys.argv[1:]:
        try:
            print('nodelete: %s: %s' % (path, nodelete(path)))
        except (OSError, ValueError) as error:
            raise SystemExit('nodelete: %s: %s' % (path, error))


if __name__ == '__main__':
    main()
