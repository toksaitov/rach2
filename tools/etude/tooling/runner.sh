#!/bin/sh
#
# runner.sh: the tooling machine's job runner, installed as
# /usr/local/sbin/etude-job-runner and started once at boot by
# etude-job.service. It runs the job on the volume whose virtio serial
# number is etude-job, as root, and powers the machine off.
#
# The volume is a raw disk with a tar archive at its start, which
# etude-tooling writes: job.sh, the job's script; in/, which becomes the
# job's workspace, /var/tmp/etude-work; and outputs, the names of the
# workspace's files and directories to bring back, one per line. The runner
# runs `sh job.sh` in the workspace with standard input from /dev/null and
# in its environment ETUDE_WORK, the workspace, ETUDE_DISKS, the count of the
# other disks etude-tooling attached, and ETUDE_DISK1 and on, their devices.
# It then writes over the volume an archive of out/, with the outputs, and of
# stdout, stderr and status, the job's output and exit status, in that order.
#
# On the second serial port it writes `etude-job: start` before the job,
# `etude-job: ran STATUS` after it, and `etude-job: end STATUS` once the
# volume is written; or `etude-job: failed: WHY` when the runner itself
# cannot go on.
#
set -u
CHAN=/dev/ttyS1
VOL=/dev/disk/by-id/virtio-etude-job
STAGE=/var/tmp/etude-job
WORK=/var/tmp/etude-work

say()
{
    printf 'etude-job: %s\n' "$*" > "$CHAN"
}

# end: write everything to the disks and power off at once; the machine's
# own disk is a copy that etude-tooling discards.
end()
{
    sync
    poweroff -f
}

fail()
{
    say "failed: $*"
    end
}

mkdir -p "$STAGE" || fail 'the directories'
tar -x -f "$VOL" -C "$STAGE" --no-same-owner || fail 'the job volume is not an archive'
[ -f "$STAGE/job.sh" ] || fail 'the job volume has no job.sh'
[ -d "$STAGE/in" ] || fail 'the job volume has no in/'
mv "$STAGE/in" "$WORK" || fail 'the workspace'
udevadm settle --timeout=30 2>/dev/null
n=0
while [ -b "/dev/disk/by-id/virtio-etude-d$((n + 1))" ]; do
    n=$((n + 1))
    export "ETUDE_DISK$n=/dev/disk/by-id/virtio-etude-d$n"
done
export ETUDE_DISKS=$n ETUDE_WORK="$WORK" HOME=/root LANG=C.UTF-8
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
say start
(cd "$WORK" && sh "$STAGE/job.sh" < /dev/null > "$STAGE/stdout" 2> "$STAGE/stderr")
status=$?
say "ran $status"
mkdir "$STAGE/out" || fail 'the out directory'
if [ -f "$STAGE/outputs" ]; then
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        [ -e "$WORK/$name" ] || fail "the job made no $name"
        cp -R -l "$WORK/$name" "$STAGE/out/" || fail "copying $name"
    done < "$STAGE/outputs"
fi
echo "$status" > "$STAGE/status" || fail 'writing the status'
tar -c -f "$VOL" -C "$STAGE" --format=gnu --hard-dereference out stdout stderr status ||
    fail 'the results do not fit in the job volume'
sync
say "end $status"
end
