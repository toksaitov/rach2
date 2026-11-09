"""p1runs.py: print the runs of 512-byte sectors where two images differ.

Usage: python3 -I p1runs.py WANTED.img CURRENT.img
Each line is "START COUNT", in sectors, for dd's skip, seek and count.
"""
import sys

a = open(sys.argv[1], "rb").read()
b = open(sys.argv[2], "rb").read()
assert len(a) == len(b), "sizes differ"
start = None
for s in range(len(a) // 512 + 1):
    same = s == len(a) // 512 or a[s * 512:(s + 1) * 512] == b[s * 512:(s + 1) * 512]
    if not same and start is None:
        start = s
    elif same and start is not None:
        print(start, s - start)
        start = None
