"""Poll QEMU's QMP query-stats for KVM's vm and vcpu statistics every 0.2 s
until QEMU ends, keeping the last reading and its time in DIR/stats.json."""
import json, os, socket, sys, time

d = sys.argv[1]
path = os.path.join(d, "s")
s = None
for _ in range(400):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(path)
        break
    except OSError:
        s.close()
        s = None
        time.sleep(0.05)
if s is None:
    sys.exit("poll: no socket")
f = s.makefile("rw")

def cmd(c, args=None):
    m = {"execute": c}
    if args is not None:
        m["arguments"] = args
    f.write(json.dumps(m) + "\n")
    f.flush()
    while True:
        line = f.readline()
        if not line:
            raise EOFError
        r = json.loads(line)
        if "return" in r or "error" in r:
            return r

f.readline()
cmd("qmp_capabilities")
n = 0
try:
    while True:
        t = time.time()
        vc = cmd("query-stats", {"target": "vcpu", "providers": [{"provider": "kvm"}]})
        vm = cmd("query-stats", {"target": "vm", "providers": [{"provider": "kvm"}]})
        n += 1
        tmp = os.path.join(d, "stats.tmp")
        with open(tmp, "w") as o:
            json.dump({"time": t, "polls": n, "vcpu": vc.get("return"), "vm": vm.get("return")}, o)
        os.rename(tmp, os.path.join(d, "stats.json"))
        time.sleep(0.2)
except (EOFError, OSError, ValueError):
    pass
with open(os.path.join(d, "end"), "w") as o:
    o.write("%f %d\n" % (time.time(), n))
