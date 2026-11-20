"""Read the round marks in probe.sh's events and print the counter differences.

Usage: python3 -I rounds.py EVENTS
"""
import json
import sys


def calls(mark):
    """Read the count, wall time, holds, held time and wait time of each row."""
    rows = {}
    for line in mark["calls"].splitlines():
        fields = line.split()
        if fields[0] in ("syscall", "vector", "take", "ioctl", "slept", "kthread"):
            rows[tuple(fields[:2])] = tuple(map(int, fields[2:7]))
    return rows


def compare_calls(start, end):
    """Print the largest deltas in counts and times."""
    before, after = calls(start), calls(end)
    rows = {}
    for key, values in after.items():
        delta = tuple(a - b for a, b in zip(values, before.get(key, (0,) * 5)))
        if min(delta) < 0:
            raise ValueError("bkl/calls counter went backwards: " + " ".join(key))
        rows[key] = delta
    print("  kind number calls wall_us holds held_us wait_us")
    for column, label in ((4, "wait"), (3, "held"), (1, "wall"), (0, "calls")):
        print("  by " + label)
        for key in sorted(rows, key=lambda k: -rows[k][column])[:5]:
            if rows[key][column]:
                print("   ", *key, *rows[key])


def compare_threads(start, end):
    """Compare threads seen at both marks and show who used the processors."""
    before, after = start["threads"], end["threads"]
    watched = ("firefox-esr", "CanvasRenderer", "ki915")
    rows = []
    for tid, thread in after.items():
        old = before.get(tid)
        if old is None or old[:2] != thread[:2]:
            continue
        run, wait, user, system, count = (thread[i] - old[i] for i in (2, 3, 6, 7, 8))
        if min(run, wait, user, system, count) >= 0 and (run or wait or thread[1] in watched):
            rows.append((thread[0], thread[1], tid, run / 1e6, wait / 1e6, user, system, count))
    print("  paired threads: run %.1f ms, wait %.1f ms, user %d ms, system %d ms" %
          tuple(sum(r[i] for r in rows) for i in (3, 4, 5, 6)))
    rows.sort(key=lambda r: -(r[3] + r[4]))
    for i, row in enumerate(rows):
        if i < 8 or row[1] in watched:
            print("    %s/%s %s run %.1f wait %.1f user %d system %d dispatches %d" % row)
    for cpu, (busy, idle) in end["cpus"].items():
        old_busy, old_idle = start["cpus"][cpu]
        print("    cpu %s busy %d idle %d ms" % (cpu, busy - old_busy, idle - old_idle))


def show(source):
    """Pair the starts and ends of all five rounds of the selected draw."""
    marks = {}
    for line in source:
        event = json.loads(line)
        if event.get("kind") == "mark" and "round" in event:
            key = event["api"], event["draw"], event["round"], event["at"]
            if key in marks:
                raise ValueError("duplicate round mark: " + str(key))
            marks[key] = event
    if len(marks) != 10:
        raise ValueError("expected ten round marks, found %d" % len(marks))
    for key, start in sorted(marks.items()):
        api, draw, number, position = key
        if position != "start":
            continue
        end = marks[api, draw, number, "end"]
        call_window = (end["calls_snap_ns"][0] - start["calls_snap_ns"][1]) / 1e6
        thread_window = (end["snap"][0] - start["snap"][1]) * 1000
        print("webgl%d %s round %d: %.1f ms, call window %.1f ms, thread window %.1f ms" %
              (api, draw, number, end["elapsed_ms"], call_window, thread_window))
        if "calls" in start and "calls" in end:
            compare_calls(start, end)
        else:
            print("  calls unavailable:", start.get("calls_error"), end.get("calls_error"))
        compare_threads(start, end)


if __name__ == "__main__":
    with open(sys.argv[1], encoding="utf-8") as events:
        show(events)
