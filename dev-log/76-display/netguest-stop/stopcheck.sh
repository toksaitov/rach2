#!/bin/sh
# stopcheck.sh: on a running Rach2, as root, check the two job-control rules
# 5368's netguest run on the Lenovo met: a stopped process that init adopted
# stays stopped, and a stopped job that its session leader's end leaves
# orphaned is sent SIGHUP and SIGCONT, which end a job that does not catch
# SIGHUP.
#
# The adopted processes are a `sleep` whose subshell has ended and, when one
# runs, the DHCP client `udhcpc -b` left in the background, whose parent is
# init; each is stopped, must read state T after 3 seconds, which takes init
# through at least two turns of its reaping loop, and is continued. The
# orphan is a `sleep` in a process group of its own, made by python3 under a
# session leader of its own; the leader stops it and exits, and the job must
# be gone within 3 seconds. Lines are `stopcheck: ...`; the last is
# `stopcheck: result: pass` or `stopcheck: result: fail: REASON`. Exit 0 or 1.
set -u

state()
{
    sed -n 's/^State:[[:space:]]*\([A-Za-z]\).*/\1/p' "/proc/$1/status" 2> /dev/null
}

parent()
{
    sed -n 's/^PPid:[[:space:]]*\([0-9]*\).*/\1/p' "/proc/$1/status" 2> /dev/null
}

fail()
{
    echo "stopcheck: result: fail: $*"
    exit 1
}

# held PID NAME: stop the adopted process PID, require T after 3 s, continue it.
held()
{
    [ "$(parent "$1")" = 1 ] || fail "$2 $1: its parent is $(parent "$1"), not init"
    kill -STOP "$1" || fail "$2 $1: kill -STOP failed"
    sleep 3
    s=$(state "$1")
    kill -CONT "$1"
    [ "$s" = T ] || fail "$2 $1: state $s 3 s after SIGSTOP, not T"
    echo "stopcheck: adopted: $2 $1 stayed stopped"
}

tmp=$(mktemp -d /tmp/stopcheck.XXXXXX) || fail "no scratch directory"
trap 'rm -rf "$tmp"' EXIT

(sleep 600 > /dev/null 2>&1 &
    echo $! > "$tmp/adopted")
sleep 1
a=$(cat "$tmp/adopted")
held "$a" sleep
kill "$a"

n=0
for f in /var/run/udhcpc.*.pid; do
    [ -r "$f" ] || continue
    p=$(cat "$f")
    [ -n "$(state "$p")" ] || continue
    held "$p" "udhcpc ${f#/var/run/udhcpc.}"
    n=$((n + 1))
done
echo "stopcheck: adopted: $n DHCP clients checked"

command -v python3 > /dev/null || fail "no python3 for the orphan check"
python3 - "$tmp/job" << 'EOF' &
import os, signal, sys, time
os.setsid()
job = os.fork()
if job == 0:
    os.setpgid(0, 0)
    os.execvp("sleep", ["sleep", "600"])
os.setpgid(job, job)
with open(sys.argv[1], "w") as f:
    f.write("%d\n" % job)
os.kill(job, signal.SIGSTOP)
for _ in range(100):
    with open("/proc/%d/status" % job) as f:
        if "\nState:\tT" in f.read():
            break
    time.sleep(0.05)
os._exit(0)
EOF
leader=$!
wait "$leader" || fail "the session leader failed"
j=$(cat "$tmp/job")
sleep 3
s=$(state "$j")
if [ -n "$s" ] && [ "$s" != Z ]; then
    kill -KILL "$j"
    fail "orphan: job $j is in state $s 3 s after its session leader ended"
fi
echo "stopcheck: orphan: job $j ended once its group was orphaned"
echo "stopcheck: result: pass"
