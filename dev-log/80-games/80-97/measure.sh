#!/bin/bash
# Repeat the event-based Crysis comparison in the task's prepared host copy.
set -e
W=/home/testing/rach2/build/worktrees/codex/build/runs/task4801
mkdir -p "$W"
exec 9>"$W/measure.lock"
flock -n 9
cd /home/testing/rach2/build/worktrees/codex
[ "$(cat /proc/sys/vm/compaction_proactiveness)" = 20 ]
cp dev-log/80-games/80-92/{crysis-load.py,summary.py,tasks.py,hosttasks.py,kvmdiff.py} "$W/load/"
python3 - "$W" <<'PY'
import pathlib,sys
w=pathlib.Path(sys.argv[1])
p=w/'load/crysis-load.py'
s=p.read_text().replace("stat = 'cat /proc/stat;", "stat = 'cat /proc/interrupts; cat /proc/stat;").replace("common = ['-accel',", "common = ['-name', 'task4801,debug-threads=on', '-accel',")
p.write_text(s)
PY
python3 - "$W/load/crysis-load.py" <<'PYPATCH'
from pathlib import Path
import sys
p=Path(sys.argv[1]);s=p.read_text()
s=s.replace('q = None\n', 'q = None\nexact = None\nfrom exact_load import ExactLoad\n',1)
s=s.replace("    game = 'env PULSE_SERVER", "    exact = ExactLoad(cmd, ssh, wrap, w, out, tag)\n    game = 'env PULSE_SERVER",1)
s=s.replace('finally:\n', 'finally:\n    if exact is not None:\n        exact.close()\n',1)
p.write_text(s)
PYPATCH
mkdir -p "$W/bin"
python3 - "$W/bin/qemu-system-x86_64" <<'PYBIN'
import pathlib,sys
p=pathlib.Path('/usr/bin/qemu-system-x86_64')
data=p.read_bytes()
old=b'CPU %d/KVM'
assert data.count(old)==1, data.count(old)
out=pathlib.Path(sys.argv[1])
out.write_bytes(data.replace(old,b'N48 %d/KVM'))
out.chmod(0o755)
PYBIN
export PATH="$W/bin:$PATH"
cp dev-log/80-games/80-97/qmpstats.py "$W/qmpstats.py"
cp dev-log/80-games/80-97/{loadwatch.py,exact_load.py} "$W/load/"
rm -f "$W/QMPSTOP"
python3 "$W/qmpstats.py" "$W/load" "$W/QMPSTOP" /sys/kernel/tracing/instances/task4801 &
watcher=$!
trap 'touch "$W/QMPSTOP"; wait "$watcher"' EXIT
site=/etc/rach2/etude-site
locks=$(sed -n 's/^locks //p' "$site")
ledger=$(sed -n 's/^ledger //p' "$site")
capacity=$(sed -n 's/^capacity //p' "$site")
for turn in ${RUNS:?set RUNS to unused N and L turn names}; do
    system=rach2
    kernel=$W/kernel-base
    case $turn in N*) kernel=$W/kernel-integrated;; L*) system=linux;; esac
    [ ! -e "$W/load/out-$system-$turn" ] || { echo "task4801: refused: turn already exists" >&2; exit 1; }
    echo "task4801: start $system $turn"
    runuser -u testing -- "$PWD/build/tools/etude/etude" admit -c -d "$locks" -w 3600 -r "$W/admit-$turn.txt" "$ledger" "task4801:$turn:$$:$(date +%s)" testing shared 4000,5497558138,1 "$capacity" 14400 /usr/bin/python3 "$W/load/crysis-load.py" "$system" "$turn" "$kernel" > "$W/load/run-$system-$turn.out" 2>&1
    echo "task4801: end $system $turn"
    cat "$W/load/run-$system-$turn.out"
done
python3 "$W/load/summary.py" "$W"/load/out-*
