"""Sum the elapsed and processor times of the attempts in a history stream
and list the N longest: casetimes.py STREAM [N]."""
import json, sys
tot = cpu = n = 0
rows = []
for l in open(sys.argv[1]):
    r = json.loads(l)
    if r["kind"] != "attempt":
        continue
    d = r["data"]
    e = d["time"]["elapsed_us"]["value"] or 0
    c = (d["resources"]["tree_cpu_us"] or {}).get("value") or 0
    case = d["item_id"].split(":")[1]
    tot += e; cpu += c; n += 1
    rows.append((e, c, case, d.get("outcome")))
print("attempts %d elapsed %.1f s cpu %.1f s" % (n, tot / 1e6, cpu / 1e6))
if len(sys.argv) > 2:
    for e, c, case, o in sorted(rows, reverse=True)[:int(sys.argv[2])]:
        print("%8.2f %8.2f %s %s" % (e / 1e6, c / 1e6, case, o))
