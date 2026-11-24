"""Read the big lock's words, each processor's record and the virtio-blk
driver's queues from a pmemsave of the kernel image (physical 0x100000 up).
usage: image.py KERNEL.DEBUG IMAGE"""
import struct, subprocess, sys

debug, image = sys.argv[1], open(sys.argv[2], "rb").read()
syms = {}
for line in subprocess.run(["nm", debug], stdout=subprocess.PIPE, text=True).stdout.splitlines():
    f = line.split()
    if len(f) == 3:
        syms[f[2]] = int(f[0], 16)
BASE = 0xffffffff80100000


def rd(addr, fmt):
    off = addr - BASE
    return struct.unpack_from("<" + fmt, image, off)


def sym(name, fmt="I"):
    return rd(syms[name], fmt)[0]


print("bkl_word %d bkl_claim %d bkl_lent %#x" % (sym("bkl_word"), sym("bkl_claim"), sym("bkl_lent")))
PERCPU = 344
for cpu in range(4):
    a = syms["cpu_table"] + cpu * PERCPU
    cur, idle = rd(a, "QQ")
    depth, online = rd(a + 40, "ii")
    irq_depth, = rd(a + 216, "I")
    since, vector, nr, waited = rd(a + 280, "QIiQ")
    start, waiting, total = rd(a + 320, "III")
    print("cpu%d current %#x idle %#x bkl_depth %d online %d irq_depth %d bkl_since %d "
          "bkl_vector %d bkl_nr %d bkl_waited %d wake_bkl start %d waiting %d total %d" %
          (cpu, cur, idle, depth, online, irq_depth, since, vector, nr, waited, start, waiting,
           total))
VBLK, Q, SLOT = 41888, 5192, 64
for d in range(3):
    a = syms["vblks"] + d * VBLK
    name = rd(a + 16, "16s")[0].split(b"\0")[0].decode()
    used, up, started, gone, dead, closed = rd(a + 36, "iiiiii")
    nq, = rd(a + 96, "H")
    if not used:
        continue
    print("vblk%d %s up %d started %d dead %d closed %d nqueues %d" %
          (d, name, up, started, dead, closed, nq))
    for i in range(nq):
        qa = a + 352 + i * Q
        lock, = rd(qa, "I")
        vq, = rd(qa + 16, "Q")
        head, tail = rd(qa + 1056, "QQ")
        unkicked, = rd(qa + 1072, "i")
        states = [rd(qa + 32 + k * SLOT, "i")[0] for k in range(16)]
        issued = [(k, rd(qa + 32 + k * SLOT, "iII")[1], rd(qa + 32 + k * SLOT + 16, "Q")[0])
                  for k in range(16) if states[k] != 0]
        print("  q%d lock %d vq %#x head %#x tail %#x unkicked %d slots %s" %
              (i, lock, vq, head, tail, unkicked, "".join(str(s) for s in states)))
        for k, typ, req in issued:
            print("    slot %d state %d type %d request %#x" % (k, states[k], typ, req))
