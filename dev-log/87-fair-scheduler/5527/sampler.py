"""sampler.py OUT [PERIOD]: every PERIOD seconds (1), append to OUT a line
`T UPTIME EPOCH` and a line `PID TID COMM RUN WAIT SLICES` for each thread, from
/proc/PID/task/TID/comm and schedstat, as threads.sh gives them, until
killed; read in one process, so that the sampling costs little."""
import os
import sys
import time

out = open(sys.argv[1], "w")
period = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0


def slurp(path):
    with open(path) as f:
        return f.read()


while True:
    lines = ["T %s %.3f" % (slurp("/proc/uptime").split()[0], time.time())]
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            tids = os.listdir("/proc/%s/task" % p)
        except OSError:
            continue
        for t in tids:
            try:
                s = slurp("/proc/%s/task/%s/schedstat" % (p, t)).split()
                c = slurp("/proc/%s/task/%s/comm" % (p, t)).strip().replace(" ", "_")
            except OSError:
                continue
            lines.append("%s %s %s %s" % (p, t, c, " ".join(s[:3])))
    out.write("\n".join(lines) + "\n")
    out.flush()
    time.sleep(period)
