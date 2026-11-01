#!/bin/sh
# Runs every repro (~2-3 min on Rach2). Build tmp files go to the current directory; run it on the fs under test.
here=$(cd "$(dirname "$0")" && pwd); w=$(mktemp -d ./r2run.XXXXXX); cd "$w" || exit 1
for c in syscall_latency fs_thread_scaling thread_scaling pagefault cloexec_check; do gcc -O2 -pthread "$here/$c.c" -o $c 2>/dev/null || echo "build failed: $c"; done
mkdir -p deep/1/2/3/4/5/6/7 && touch deep/f deep/1/2/3/f deep/1/2/3/4/5/6/7/f
echo "=== syscall_latency (R2-001/006/013)"; ./syscall_latency .
echo "=== fs_thread_scaling (R2-001/003)"; ./fs_thread_scaling
echo "=== thread_scaling (R2-004)"; for n in 250 500 1000 2000; do ./thread_scaling $n; done
echo "=== py_thread_wakeup (R2-005)"; python3 "$here/py_thread_wakeup.py" 125 250
echo "=== pagefault (R2-014)"; ./pagefault
echo "=== cloexec_check (sanity: all should be closed)"; ./cloexec_check | grep -c 'NOT HONORED' | sed 's/^/fds leaked across exec: /'
echo "=== mtime_make (R2-002)"; sh "$here/mtime_make.sh" .
echo "=== inotify_attrib (R2-011)"; python3 "$here/inotify_attrib.py"
echo "=== misc_syscalls"; python3 "$here/misc_syscalls.py"
echo "=== checks"; bash "$here/checks.sh"
cd .. && rm -r "$w"
