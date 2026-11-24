"""Walk the page tables of the held guest's processes by HMP, through the
driver's hmp.in, and give the physical page behind each given user address.
usage: walk.py RUNDIR PROC_LIST_ADDR PID:ADDR[,ADDR...]..."""
import os, re, sys, time
from pathlib import Path

run = Path(sys.argv[1])
proc_list = int(sys.argv[2], 16)
want = {}
for arg in sys.argv[3:]:
    pid, addrs = arg.split(":")
    want[int(pid)] = [int(a, 16) for a in addrs.split(",")]


def hmp(lines):
    out = run / "hmp.out"
    if out.exists():
        out.unlink()
    tmp = run / "hmp.in.tmp"
    tmp.write_text("\n".join(lines) + "\n")
    os.rename(tmp, run / "hmp.in")
    for _ in range(600):
        if out.exists():
            text = out.read_text()
            out.unlink()
            return text
        time.sleep(0.1)
    raise SystemExit("walk: no answer from the driver")


def words(text):
    """The 64-bit words of x or xp output, in order."""
    vals = []
    for line in text.splitlines():
        if ":" in line and not line.startswith("(qemu)"):
            vals += [int(w, 16) for w in re.findall(r"0x([0-9a-f]{16})", line.split(":", 1)[1])]
    return vals


first = words(hmp(["x /1gx %#x" % proc_list]))[0]
p, seen, dirs = first, set(), {}
while p and p not in seen and len(seen) < 4096:
    seen.add(p)
    w = words(hmp(["x /82gx %#x" % p]))
    pid = w[0] & 0xffffffff
    page_dir, nxt = w[632 // 8], w[640 // 8]
    if pid in want:
        dirs[pid] = page_dir
    p = nxt
print("walk: %d processes on proc_list" % len(seen))
for pid, addrs in sorted(want.items()):
    if pid not in dirs:
        print("walk: pid %d not found" % pid)
        continue
    text = hmp(["gva2gpa %#x" % dirs[pid]])
    m = re.search(r"gpa: 0x([0-9a-f]+)", text)
    table = int(m.group(1), 16)
    for va in addrs:
        t, path = table, []
        for level, shift in (("pml4", 39), ("pdpt", 30), ("pd", 21), ("pt", 12)):
            e = words(hmp(["xp /1gx %#x" % (t + ((va >> shift) & 511) * 8)]))[0]
            path.append("%s %#x" % (level, e))
            if not e & 1:
                print("walk: pid %d va %#x: not present at %s (%s)" % (pid, va, level, "; ".join(path)))
                break
            frame = e & 0x000ffffffffff000
            if level in ("pdpt", "pd") and e & 0x80:
                size = 1 << shift
                pa = (e & 0x000fffffffffe000 & ~(size - 1)) + (va & (size - 1))
                print("walk: pid %d va %#x -> pa %#x (large page at %s)" % (pid, va, pa, level))
                break
            if level == "pt":
                pa = frame + (va & 4095)
                val = hmp(["xp /1wx %#x" % pa]).splitlines()[-1].split(":", 1)[-1].strip()
                print("walk: pid %d va %#x -> page %#x pa %#x word %s" % (pid, va, frame, pa, val))
            t = frame
