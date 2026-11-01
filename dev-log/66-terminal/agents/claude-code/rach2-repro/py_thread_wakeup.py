# R2-005: N threads blocked on one threading.Event, then set() + join().
# Linux: well under 0.1 s for n=500. Rach2 (2026-11-01): n=125 0.16 s, n=250 3.9 s, n=500 31 s.
import threading, time, sys
for n in [int(a) for a in sys.argv[1:]] or [125, 250]:
    ev = threading.Event(); t0 = time.time()
    ts = [threading.Thread(target=ev.wait) for _ in range(n)]; [t.start() for t in ts]; t1 = time.time()
    ev.set(); [t.join() for t in ts]; t2 = time.time()
    print(f"python threads n={n:5}: create {t1-t0:.3f}s  set+join {t2-t1:.3f}s")
