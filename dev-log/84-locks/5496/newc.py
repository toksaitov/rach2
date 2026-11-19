# newc.py OUT NAME=PATH...: a gzip'd newc cpio of regular files, mode 0755.
import gzip, sys
out = gzip.open(sys.argv[1], "wb")
ino = 1
def ent(name, mode, data):
    global ino
    hdr = "070701" + "".join("%08x" % v for v in (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name) + 1, 0))
    ino += 1
    b = hdr.encode() + name.encode() + b"\0"
    b += b"\0" * (-len(b) % 4)
    b += data + b"\0" * (-len(data) % 4)
    out.write(b)
for arg in sys.argv[2:]:
    name, path = arg.split("=", 1)
    ent(name, 0o100755, open(path, "rb").read())
ent("TRAILER!!!", 0, b"")
out.close()
