#!/bin/sh
# Run crysis-load.py SYSTEM TURN [KERNEL] as one claim of the site's ledger.
W=/home/testing/rach2/build/worktrees/m1/build/runs/task4159
site=/etc/rach2/etude-site
f() { sed -n "s/^$1 //p" "$site"; }
exec /home/testing/rach2/build/worktrees/m1/build/tools/etude/etude admit -c -d "$(f locks)" -w 3600 -r "$W/admit-$1-$2.txt" "$(f ledger)" \
    "task4159:$1-$2:$$:$(date +%s)" "$(id -un)" shared ${VIEW_CLAIM:-4000,5497558138,1} "$(f capacity)" 14400 \
    /usr/bin/python3 "$W/crysis-load.py" "$@"
