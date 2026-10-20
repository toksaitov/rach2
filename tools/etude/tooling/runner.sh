#!/bin/sh
#
# runner.sh: the tooling machine's job runner, installed as
# /usr/local/sbin/etude-job-runner and started once at boot by
# etude-job.service. It runs the job on the FAT volume whose virtio serial
# number is etude-job, as root, and powers the machine off.
#
# The volume has job.sh, the job's script; in/, whose contents are copied
# into the job's workspace, /var/tmp/etude-work; and outputs, the names of
# the workspace's files and directories to bring back, one per line. The
# runner runs `sh job.sh` in the workspace with standard input from
# /dev/null, its output in the volume's stdout and stderr, and in its
# environment ETUDE_WORK, the workspace, ETUDE_DISKS, the count of the other
# disks etude-tooling attached, and ETUDE_DISK1 and on, their devices. It then
# copies the outputs into out/, and writes status, the job's exit status,
# after every other file.
#
# On the second serial port it writes `etude-job: start` before the job,
# `etude-job: ran STATUS` after it, and `etude-job: end STATUS` once the
# volume is written and unmounted; or `etude-job: failed: WHY` when the
# runner itself cannot go on.
#
set -u
CHAN=/dev/ttyS1
VOL=/dev/disk/by-id/virtio-etude-job
MNT=/run/etude-job
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
    umount "$MNT" 2>/dev/null
    end
}

mkdir -p "$MNT" "$WORK" || fail 'the directories'
mount -t vfat -o rw,utf8,shortname=mixed "$VOL" "$MNT" || fail 'the job volume does not mount'
[ -f "$MNT/job.sh" ] || fail 'the job volume has no job.sh'
if [ -d "$MNT/in" ]; then
    cp -R "$MNT/in/." "$WORK/" || fail 'copying the inputs'
fi
udevadm settle --timeout=30 2>/dev/null
n=0
while [ -b "/dev/disk/by-id/virtio-etude-d$((n + 1))" ]; do
    n=$((n + 1))
    export "ETUDE_DISK$n=/dev/disk/by-id/virtio-etude-d$n"
done
export ETUDE_DISKS=$n ETUDE_WORK="$WORK" HOME=/root LANG=C.UTF-8
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
say start
(cd "$WORK" && sh "$MNT/job.sh" < /dev/null > "$MNT/stdout" 2> "$MNT/stderr")
status=$?
say "ran $status"
mkdir -p "$MNT/out" || fail 'the out directory'
if [ -f "$MNT/outputs" ]; then
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        [ -e "$WORK/$name" ] || fail "the job made no $name"
        cp -R "$WORK/$name" "$MNT/out/" || fail "copying $name"
    done < "$MNT/outputs"
fi
sync
echo "$status" > "$MNT/status" || fail 'writing the status'
umount "$MNT" || fail 'the job volume does not unmount'
say "end $status"
end
