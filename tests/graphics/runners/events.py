"""events.py: read the events file that server.py writes for the browser
runners.

Usage:
  python3 -I events.py wait EVENTS FROM PID SILENCE
  python3 -I events.py pages EVENTS FROM TO STATE
  python3 -I events.py list EVENTS
  python3 -I events.py renderer EVENTS
  python3 -I events.py plays EVENTS FROM TO

`wait` reads EVENTS from line FROM until a `done` event, until the process
PID has ended, or until SILENCE seconds pass with no new line, and prints
`done`, `ended` or `silent` with the number of lines read in all. A page
posts a line as each page starts and ends, and a `tick` every 10 s while one
runs, so SILENCE ends only a browser that has stopped.

`pages` prints, for lines FROM to TO, one line a page of the WebGL suite,
`INDEX STATUS PASSED FAILED SKIPPED TIMEOUTS URL`; a page that started and
has no result is `crash` when STATE is `ended` and `hung` when `silent`.
`list` prints the suite list's length and SHA-256; `renderer` the record's
renderer lines; `plays` one line a play of video.html, `REP WARM TOTAL
DROPPED CORRUPTED SECONDS WIDTHxHEIGHT ERROR`.
"""
import json
import os
import sys
import time


def read(path, start=0, stop=None):
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    complete = lines[:-1]  # the last part has no newline yet
    out = []
    for line in complete[start:stop]:
        try:
            out.append(json.loads(line))
        except ValueError:
            out.append({"kind": "garbled"})
    return out, len(complete)


def ended(pid):
    try:
        with open("/proc/%d/stat" % pid, encoding="ascii") as f:
            state = f.read().rsplit(")", 1)[1].split()[0]
    except (OSError, IndexError):
        return True
    return state in ("Z", "X")


def wait(path, start, pid, silence):
    seen = start
    last = time.monotonic()
    while True:
        new, total = read(path, seen)
        if total > seen:
            seen = total
            last = time.monotonic()
            if any(e.get("kind") == "done" for e in new):
                return "done", seen
        if ended(pid):
            _, seen = read(path)
            return "ended", seen
        if time.monotonic() - last >= silence:
            return "silent", seen
        time.sleep(0.5)


def pages(path, start, stop, state):
    events, _ = read(path, start, stop)
    running = None
    for e in events:
        kind = e.get("kind")
        if kind == "start":
            running = e
        elif kind == "result":
            print(e["index"], e["status"], e["passed"], e["failed"], e["skipped"], e["timeouts"],
                  e["url"])
            running = None
    if running is not None and state in ("ended", "silent"):
        print(running["index"], "crash" if state == "ended" else "hung", 0, 0, 0, 0,
              running["url"])


def first(path, kind):
    events, _ = read(path)
    for e in events:
        if e.get("kind") == kind:
            return e
    return None


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == "wait":
        state, seen = wait(path, int(sys.argv[3]), int(sys.argv[4]), float(sys.argv[5]))
        print(state, seen)
    elif cmd == "pages":
        pages(path, int(sys.argv[3]), int(sys.argv[4]), sys.argv[5])
    elif cmd == "list":
        e = first(path, "list")
        print("%d %s" % (e["count"], e["sha256"]) if e else "0 none")
    elif cmd == "renderer":
        e = first(path, "renderer")
        for c in (e or {}).get("contexts", []):
            print("renderer %s %s; %s; unmasked %s; vendor %s" % (
                c.get("context"), c.get("renderer"), c.get("version"), c.get("unmasked"),
                c.get("vendor")))
    elif cmd == "plays":
        events, _ = read(path, int(sys.argv[3]), int(sys.argv[4]))
        for e in events:
            if e.get("kind") == "play":
                count = [("-" if e.get(k) is None else e[k]) for k in ("total", "dropped", "corrupted")]
                print(e["rep"], 1 if e["warm"] else 0, *count, "%.2f" % (e.get("currentTime") or 0),
                      "%sx%s" % (e.get("width"), e.get("height")),
                      (e.get("error") or "none").replace(" ", "_"))
    else:
        sys.exit("events: no command %s" % cmd)


if __name__ == "__main__":
    os.environ.setdefault("LC_ALL", "C")
    main()
