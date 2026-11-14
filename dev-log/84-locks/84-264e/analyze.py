"""Sum the KVM statistics and device accesses of the QEMU runs under DIR,
or of those whose case begins with PREFIX: analyze.py DIR [PREFIX]."""
import collections, glob, json, os, sys

KEYS = ["exits", "io_exits", "mmio_exits", "halt_exits", "irq_exits",
        "irq_window_exits", "nmi_window_exits", "signal_exits",
        "request_irq_exits", "insn_emulation", "pf_emulate", "pf_taken",
        "pf_fixed", "hypercalls", "irq_injections", "host_state_reload",
        "tlb_flush", "invlpg", "halt_wakeup", "halt_wait_ns",
        "halt_poll_success_ns", "halt_poll_fail_ns"]

def stats(path):
    j = json.load(open(path))
    tot = collections.Counter()
    ncpu = 0
    for v in j["vcpu"] or []:
        ncpu += 1
        for s in v["stats"]:
            if isinstance(s["value"], (int, float)) and not isinstance(s["value"], bool):
                tot[s["name"]] += s["value"]
    return tot, ncpu, j["time"]

def case(r):
    try:
        env = dict(l.rstrip("\n").split("=", 1) for l in open(os.path.join(r, "env")) if "=" in l)
    except OSError:
        return "?"
    if env.get("ETUDE_CASE"):
        return env["ETUDE_CASE"]
    t = os.path.basename(env.get("TMPDIR", ""))
    parts = t.split(".")
    return ".".join(parts[4:-1]) if len(parts) > 5 else "?"

def main():
    d = sys.argv[1]
    runs = sorted(glob.glob(os.path.join(d, "q.*")))
    if len(sys.argv) > 2:
        runs = [r for r in runs if case(r).startswith(sys.argv[2])]
    tot = collections.Counter()
    acc = collections.Counter()
    per = []
    wall = 0.0
    for r in runs:
        sp = os.path.join(r, "stats.json")
        if not os.path.exists(sp):
            print("no stats:", r, file=sys.stderr)
            continue
        t, ncpu, last = stats(sp)
        start = float(open(os.path.join(r, "start")).read())
        end = os.path.join(r, "end")
        e = float(open(end).read().split()[0]) if os.path.exists(end) else last
        wall += e - start
        tot.update(t)
        args = open(os.path.join(r, "args")).read().split("\n")
        per.append((t["exits"], t["io_exits"], t["mmio_exits"], t["halt_exits"], ncpu, round(e - start, 2), r, case(r)))
        ap = os.path.join(r, "access.txt")
        if os.path.exists(ap):
            for line in open(ap):
                f = line.split()
                if len(f) >= 5:
                    acc[(f[1].strip("'"), f[2], f[3], f[4])] += int(f[0])
    print("runs", len(runs), "qemu wall s %.1f" % wall)
    for k in KEYS:
        print("%-22s %14d" % (k, tot[k]))
    named = tot["io_exits"] + tot["mmio_exits"] + tot["halt_exits"] + tot["irq_exits"] + \
        tot["irq_window_exits"] + tot["nmi_window_exits"] + tot["signal_exits"] + tot["request_irq_exits"]
    print("%-22s %14d" % ("rest", tot["exits"] - named))
    if acc:
        byname = collections.Counter()
        for (n, a, s, rw), c in acc.items():
            byname[n] += c
        print("\naccesses reaching QEMU: %d" % sum(acc.values()))
        for n, c in byname.most_common(30):
            print("%-28s %12d" % (n, c))
        print("\nby address:")
        for (n, a, s, rw), c in acc.most_common(40):
            print("%12d %-24s %-12s %s %s" % (c, n, a, s, rw))
    print("\nper run (exits io mmio halt cpus wall):")
    for p in sorted(per):
        print(*p)

main()
