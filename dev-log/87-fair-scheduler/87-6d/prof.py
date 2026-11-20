"""prof.py: debugfs's bkl/profile in its whole-kernel mode, by function.

Usage: python3 -I prof.py KERNEL PROFILE [TOP]

KERNEL is the kernel's ELF with its symbols (build/x86_64/rach2/kernel.debug
of the build that made the booted kernel). Prints the samples by the
function of the sampled instruction, by entry (system call, vector or
ioctl), and by the chain of the instruction and its three callers, TOP
lines of each, 25 by default.
"""
import bisect
import collections
import subprocess
import sys

kernel, profile = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
syms = []
nm = subprocess.run(["nm", "-n", kernel], capture_output=True, text=True, check=True)
for line in nm.stdout.splitlines():
    words = line.split()
    if len(words) == 3 and words[1] in "tTwW":
        syms.append((int(words[0], 16), words[2]))
addrs = [a for a, _ in syms]


def name(pc):
    i = bisect.bisect_right(addrs, pc) - 1
    return syms[i][1] if i >= 0 else hex(pc)


byfn, byentry, chain = collections.Counter(), collections.Counter(), collections.Counter()
total = 0
with open(profile, encoding="ascii") as f:
    for line in f:
        words = line.split()
        if len(words) < 7:
            print(line.strip())
            continue
        entry, pcs, n = words[0] + " " + words[1], [int(w, 16) for w in words[2:6]], int(words[6])
        total += n
        byfn[name(pcs[0])] += n
        byentry[entry] += n
        chain[entry + ": " + " < ".join(name(pc) for pc in pcs if pc)] += n
for title, counts in (("function", byfn), ("entry", byentry), ("chain", chain)):
    print("==", title)
    for key, n in counts.most_common(top):
        print("%6d %5.1f%% %s" % (n, 100.0 * n / max(total, 1), key))
