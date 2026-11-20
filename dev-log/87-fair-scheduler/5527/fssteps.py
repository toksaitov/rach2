"""fssteps.py DIR DEV: the device writes, sectors and flushes of each step of
a SQLite commit and of a rename over a file in DIR, 50 calls each, read from
DEV's line of /proc/diskstats around each call, for 5527."""
import os, sys
d, dev = sys.argv[1], sys.argv[2]
def w():
    for l in open("/proc/diskstats"):
        f = l.split()
        if f[2] == dev: return int(f[7]), int(f[9]), (int(f[18]) if len(f) > 18 else 0)
    return 0, 0, 0
buf = b"x" * 8192
db = os.open(d + "/s.sqlite", os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o644)
os.write(db, buf * 4); os.fsync(db)
os.sync()
res = {}
def step(name, fn, rounds=50):
    tot = [0, 0, 0]
    for i in range(rounds):
        a = w(); fn(i); b = w()
        for k in range(3): tot[k] += b[k] - a[k]
    print("%-28s writes %5.2f sectors %6.1f flushes %4.2f per call" % (name, tot[0] / rounds, tot[1] / rounds, tot[2] / rounds))
jr = d + "/s.sqlite-journal"
fds = {}
def create(i): fds[i] = os.open(jr + str(i), os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o644)
def write8k(i): os.pwrite(fds[i], buf, 0)
def fsync(i): os.fsync(fds[i])
def dbwrite(i): os.pwrite(db, buf[:4096], (i % 8) * 4096)
def fdatasync(i): os.fdatasync(db)
def close(i): os.close(fds[i])
def unlink(i): os.unlink(jr + str(i))
def rename(i):
    t = d + "/p.tmp"; f = os.open(t, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644); os.write(f, buf[:6000]); os.close(f)
def ren(i): os.rename(d + "/p.tmp", d + "/p.js")
step("create (open O_CREAT)", create)
step("pwrite 8 KiB", write8k)
step("fsync of it", fsync)
step("pwrite 4 KiB db", dbwrite)
step("fdatasync db", fdatasync)
step("close", close)
step("unlink", unlink)
for i in range(50):
    rename(i)
    a = w(); ren(i); b = w()
    res.setdefault("r", [0, 0, 0])
    for k in range(3): res["r"][k] += b[k] - a[k]
print("%-28s writes %5.2f sectors %6.1f flushes %4.2f per call" % ("rename over a file", res["r"][0] / 50, res["r"][1] / 50, res["r"][2] / 50))
