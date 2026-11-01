#!/bin/sh
# R2-016: SIGTERM to the PID you launched leaves the real browser (and later its children) running.
/usr/bin/chromium-headless-shell --no-sandbox --remote-debugging-port=0 about:blank 2>/dev/null & W=$!
sleep 3; B=$(pgrep -P $W | head -1); echo "wrapper pid=$W  real browser pid=$B"
kill -TERM $W; sleep 3
[ -e /proc/$B ] && echo "BUG: browser $B still running 3 s after its launcher got SIGTERM (ppid now $(awk '{print $4}' /proc/$B/stat))"
KIDS=$(pgrep -P $B | tr '\n' ' '); kill -KILL $B; sleep 5; for k in $KIDS; do [ -e /proc/$k ] && echo "BUG: child $k ($(tr '\0' ' ' </proc/$k/cmdline | grep -oE -- '--type=[a-z-]+')) still alive 5 s after browser SIGKILL"; done
for k in $KIDS; do kill -KILL $k 2>/dev/null; done
