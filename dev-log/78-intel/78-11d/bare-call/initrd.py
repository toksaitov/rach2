"""Write a gzip newc initramfs holding /dev/console, /mnt and PROGRAM as /init,
with MODULE as /virtio_blk.ko.xz when one is given."""

import gzip
import sys


def entry(name, mode, data=b"", ino=0, rdev=(0, 0)):
    """Return one newc header, name and data, each padded to four bytes."""
    path = name.encode() + b"\0"
    fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0, rdev[0], rdev[1], len(path), 0)
    head = b"070701" + "".join("%08x" % v for v in fields).encode() + path
    head += b"\0" * (-len(head) % 4)
    return head + data + b"\0" * (-len(data) % 4)


def main():
    """Usage: initrd.py PROGRAM OUTPUT [MODULE]"""
    program, output = sys.argv[1:3]
    with open(program, "rb") as f:
        data = f.read()
    archive = (entry("dev", 0o40755, ino=1)
               + entry("dev/console", 0o20600, ino=2, rdev=(5, 1))
               + entry("mnt", 0o40755, ino=3)
               + entry("init", 0o100755, data, ino=4))
    if len(sys.argv) > 3:
        with open(sys.argv[3], "rb") as f:
            archive += entry("virtio_blk.ko.xz", 0o100644, f.read(), ino=5)
    archive += entry("TRAILER!!!", 0)
    with open(output, "wb") as f:
        f.write(gzip.compress(archive, 1))


main()
