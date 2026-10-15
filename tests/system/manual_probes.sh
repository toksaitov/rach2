#!/bin/sh
#
# manual_probes.sh KEY ADDRESS MODE DIR
#
# The probes and measurements a person runs over SSH, as root, against an
# installed machine. Every mode but `clean` first runs
# tools/dev/machine_identity.sh, so RACH2_MEASURE_SLOT and RACH2_MEASURE_KERNEL
# must name the slot and the kernel the machine is running, and the record says
# which kernel answered. DIR receives the logs, under a directory of this run's
# own. RACH2_PROBE_PORT names another SSH port, for a rehearsal against a
# machine in the emulator.
#
# probes  The ten probes that tests/Readme.md lists for an installed root,
#         on the machine's ext2 root, with the prerequisites that list gives
#         each. The executables are copied from build/tests and compared
#         byte for byte on the machine; a name the machine already has is
#         refused unless it has the same bytes, and so is a name a probe
#         reserves. Nine run over SSH, and what this script staged is removed
#         after them. orphanprobe must be the kernel's init, which no SSH
#         child can be, so it is left at /orphanprobe and the console steps
#         for it are printed. devprobe switches the machine's screen to
#         graphics mode and back.
# clean   Remove /orphanprobe once its console run is done.
# writes  The disk commands and cache flushes of 2000 calls of 4 KiB through
#         one open file, appended to a new file and then rewritten in place,
#         each run ended by one fsync, read from /proc/disk around each;
#         three runs.
# pipe    `dd` of 128 MiB in 1 MiB reads from the root's own partition, the
#         device /proc/mounts names for /, into `sha256sum`,
#         three runs, each timed by the machine's clock and by this host's
#         around its SSH command. An installed boot publishes the partitions
#         and not the whole disk, so the partition is what the pipe reads.
#
# The exit status is 0 when every probe or run passed.
set -eu
# Print the usage and exit 2.
usage()
{
    echo 'usage: manual_probes.sh KEY ADDRESS probes|clean|writes|pipe DIR' >&2
    exit 2
}
[ "$#" -eq 4 ] || usage
key=$1
address=$2
mode=$3
directory=$4
port=${RACH2_PROBE_PORT:-22}
case "$mode" in
    probes | clean | writes | pipe) ;;
    *) usage ;;
esac
case "$address" in
    '' | *[!0-9.]*) echo 'manual-probes: expected an IPv4 address' >&2; exit 2 ;;
esac
case "$port" in
    '' | *[!0-9]*) echo 'manual-probes: RACH2_PROBE_PORT must be a port number' >&2; exit 2 ;;
esac
[ -r "$key" ] && [ -d "$directory" ] || {
    echo 'manual-probes: need a readable key and a log directory' >&2
    exit 2
}
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
tests=$repo/build/tests
work=$(mktemp -d "$directory/manual-probes.XXXXXX")
known=$work/known_hosts
probes='shelltest limitprobe dirprobe unlinkdirprobe mvtest vfsprobe dnstest modeprobe devprobe'
names="hello suidhelper $probes orphanprobe"
# The root-level names the probes create or require absent, and the scratch
# directory this script works in.
reserved='d e g h m work nope unlinkdir vp procx rach2-probes'
staged=
ran=

# remote COMMAND: run COMMAND on the machine as root over SSH.
remote()
{
    ssh -i "$key" -p "$port" -o BatchMode=yes -o ConnectTimeout=10 \
        -o UserKnownHostsFile="$known" -o StrictHostKeyChecking=accept-new \
        "root@$address" "$1"
}

# host_sum FILE: print FILE's SHA-256, as this host computes it.
host_sum()
{
    shasum -a 256 <"$1" | cut -d ' ' -f 1
}

# The machine's view of NAMES: each as `have NAME absent` or
# `have NAME SHA256`, the root's type, the reserved names present, and any
# terminal owned by uid 1000 or 1001. The names are this script's own words.
survey()
{
    remote "$(sed "s/@NAMES@/$1/; s/@RESERVED@/$reserved/" <<'EOF'
: manual-probes survey
echo "root $(awk '$2 == "/" { t = $3 } END { print t }' /proc/mounts)"
for name in @NAMES@; do
    if [ -e "/$name" ]; then
        echo "have $name $(sha256sum <"/$name" | cut -d ' ' -f 1)"
    else
        echo "have $name absent"
    fi
done
for name in @RESERVED@; do
    [ ! -e "/$name" ] || echo "reserved $name present"
done
for tty in /dev/pts/*; do
    [ -e "$tty" ] || continue
    owner=$(stat -c %u "$tty")
    if [ "$owner" = 1000 ] || [ "$owner" = 1001 ]; then
        echo "session $tty"
    fi
done
EOF
)"
}

# Remove what this run staged and what the probes left under the names the
# survey found absent, and put back the machine's own hosts file if a probe
# run was cut short before its own trap did.
# shellcheck disable=SC2329
cleanup()
{
    [ -n "$staged$ran" ] || return 0
    keep=
    [ -z "$ran" ] || keep=orphanprobe
    remove=
    for name in $staged; do
        [ "$name" = "$keep" ] || remove="$remove /$name"
    done
    for name in $reserved; do
        remove="$remove /$name"
    done
    remote "if [ -f /rach2-probes/hosts.saved ]; then cp -p /rach2-probes/hosts.saved /etc/hosts; fi
if [ -f /rach2-probes/hosts.absent ]; then rm -f /etc/hosts; fi
rm -rf$remove" >"$work/cleanup.log" 2>&1 ||
        echo "manual-probes: cleanup failed; see $work/cleanup.log" >&2
}
# The Mac's sh, bash 3.2, gives an EXIT trap a status of 0 when `set -u`
# stops a script that also has `set -e`, as this one does, so a run that
# reached no success exit fails here.
finished=
trap 'status=$?; cleanup; [ -n "$finished" ] || [ "$status" -ne 0 ] || status=1; exit "$status"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

echo "manual-probes: logs: $work"
if [ "$mode" != clean ]; then
    sh "$repo/tools/dev/machine_identity.sh" "$work/identity.log" ssh -i "$key" -p "$port" \
        -o BatchMode=yes -o ConnectTimeout=10 -o UserKnownHostsFile="$known" \
        -o StrictHostKeyChecking=accept-new "root@$address"
fi

if [ "$mode" = clean ]; then
    [ -f "$tests/orphanprobe" ] || { echo "manual-probes: missing $tests/orphanprobe" >&2; exit 1; }
    survey orphanprobe >"$work/survey.log"
    have=$(sed -n 's/^have orphanprobe //p' "$work/survey.log")
    if [ "$have" = absent ]; then
        echo 'manual-probes: result: /orphanprobe is already gone'
    elif [ "$have" = "$(host_sum "$tests/orphanprobe")" ]; then
        remote 'rm -f /orphanprobe'
        echo 'manual-probes: result: /orphanprobe removed'
    else
        echo "manual-probes: refused: /orphanprobe is not $tests/orphanprobe; left in place" >&2
        exit 1
    fi
    finished=1
    exit 0
fi

if [ "$mode" = writes ]; then
    # Each run: a new file of 2000 appended 4 KiB calls, then the same file
    # rewritten in place by 2000 calls, each ended by fsync, with every
    # earlier dirty block synced before the first reading. The guest clock
    # counts delivered ticks and is printed for the record, not judged.
    rc=0
    remote "$(cat <<'EOF'
: manual-probes writes
grep -q " / ext2 " /proc/mounts || { echo "manual-probes: writes: the root is not ext2"; exit 1; }
dir=$(mktemp -d /rach2-cost.XXXXXX) || exit 1
counts() { awk '$1 == "disk-commands" { c = $2 } $1 == "disk-flushes" { f = $2 } END { print c, f }' /proc/disk; }
now() { read -r u _ </proc/uptime; echo $(( ${u%.*} * 100 + 1${u#*.} - 100 )); }
rc=0
for run in 1 2 3; do
    for kind in append rewrite; do
        if [ "$kind" = append ]; then
            rm -f "$dir/file"
            conv=fsync
        else
            conv=notrunc,fsync
        fi
        sync
        set -- $(counts) $(now)
        dd if=/dev/zero of="$dir/file" bs=4096 count=2000 conv=$conv 2>"$dir/dd.err" || rc=1
        grep -q "^8192000 bytes" "$dir/dd.err" || { cat "$dir/dd.err"; rc=1; }
        set -- "$@" $(counts) $(now)
        echo "manual-probes: writes: run $run $kind calls 2000 commands $(( $4 - $1 )) flushes $(( $5 - $2 )) guest-centiseconds $(( $6 - $3 ))"
    done
done
rm -rf "$dir"
exit $rc
EOF
)" >"$work/writes.log" 2>&1 || rc=1
    cat "$work/writes.log"
    [ "$rc" -eq 0 ] || { echo 'manual-probes: result: fail: a write run did not complete' >&2; exit 1; }
    echo 'manual-probes: result: writes measured three times'
    finished=1
    exit 0
fi

if [ "$mode" = pipe ]; then
    pipe_script=$(cat <<'EOF'
: manual-probes pipe
now() { read -r u _ </proc/uptime; echo $(( ${u%.*} * 100 + 1${u#*.} - 100 )); }
err=$(mktemp /tmp/manual-probes.XXXXXX) || exit 1
dev=$(awk '$2 == "/" { d = $1 } END { print d }' /proc/mounts)
if [ "${dev#/dev/}" = "$dev" ]; then
    echo "manual-probes: pipe: the root has no device"
    rm -f "$err"
    exit 1
fi
echo "manual-probes: pipe: device $dev"
start=$(now)
dd if="$dev" bs=1M count=128 2>"$err" | sha256sum | sed "s/ .*//; s/^/manual-probes: pipe: sha256 /"
end=$(now)
cat "$err"
grep -q "^134217728 bytes" "$err"
status=$?
rm -f "$err"
echo "manual-probes: pipe: guest-centiseconds $(( end - start ))"
exit $status
EOF
)
    rc=0
    for run in 1 2 3; do
        begin=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
        remote "$pipe_script" >"$work/pipe-$run.log" 2>&1 || rc=1
        end=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
        cat "$work/pipe-$run.log"
        echo "manual-probes: pipe: run $run host-seconds $(echo "$begin $end" | awk '{ printf "%.3f", $2 - $1 }') around the SSH command"
    done
    [ "$rc" -eq 0 ] || { echo 'manual-probes: result: fail: a pipe run did not read 128 MiB' >&2; exit 1; }
    echo 'manual-probes: result: pipe measured three times'
    finished=1
    exit 0
fi

for name in $names; do
    [ -f "$tests/$name" ] || { echo "manual-probes: missing $tests/$name; build it with make" >&2; exit 1; }
done
survey "$names" >"$work/survey.log"
cat "$work/survey.log"
[ "$(sed -n 's/^root //p' "$work/survey.log")" = ext2 ] || {
    echo 'manual-probes: refused: the root is not ext2' >&2
    exit 1
}
if grep -q '^reserved ' "$work/survey.log"; then
    echo "manual-probes: refused: names the probes reserve exist:$(sed -n 's/^reserved \([^ ]*\) present/ \/\1/p' "$work/survey.log" | tr -d '\n')" >&2
    exit 1
fi
absent=
for name in $names; do
    have=$(sed -n "s/^have $name //p" "$work/survey.log")
    if [ "$have" = absent ]; then
        absent="$absent $name"
    elif [ "$have" != "$(host_sum "$tests/$name")" ]; then
        echo "manual-probes: refused: /$name on the machine is not $tests/$name" >&2
        exit 1
    fi
done

# The scratch directory, the probes' executables and the script that runs
# them, each checked on the machine against this host's bytes.
staged=' '
remote 'mkdir /rach2-probes /rach2-probes/mv /rach2-probes/mode'
for name in $absent; do
    staged="$staged $name"
    remote "cat >/$name && chmod 755 /$name" <"$tests/$name"
done
cat >"$work/run.sh" <<'EOF'
# The probes of manual_probes.sh, run on the machine as root.
umask 022
restore_hosts()
{
    if [ -f /rach2-probes/hosts.saved ]; then
        cp -p /rach2-probes/hosts.saved /etc/hosts && rm -f /rach2-probes/hosts.saved
    elif [ -f /rach2-probes/hosts.absent ]; then
        rm -f /etc/hosts /rach2-probes/hosts.absent
    fi
}
trap restore_hosts EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run()
{
    name=$1
    shift
    echo "manual-probes: run: $name"
    status=0
    "$@" || status=$?
    echo "manual-probes: $name: status $status"
}

sessions=
for tty in /dev/pts/*; do
    [ -e "$tty" ] || continue
    case "$(stat -c %u "$tty")" in
        1000 | 1001) sessions="$sessions $tty" ;;
    esac
done

cd /rach2-probes || exit 1
run shelltest /shelltest </dev/null
if [ -n "$sessions" ]; then
    echo "manual-probes: limitprobe: not run: a terminal of uid 1000 or 1001 is open:$sessions"
else
    run limitprobe sh -c 'ulimit -n 1024 && ulimit -v unlimited && exec /limitprobe' </dev/null
fi
run dirprobe /dirprobe </dev/null
run unlinkdirprobe /unlinkdirprobe </dev/null
cd /rach2-probes/mv || exit 1
run mvtest /mvtest </dev/null
cd /rach2-probes || exit 1
run vfsprobe /vfsprobe </dev/null
if [ -e /etc/hosts ]; then
    cp -p /etc/hosts /rach2-probes/hosts.new && mv /rach2-probes/hosts.new /rach2-probes/hosts.saved ||
        exit 1
else
    : >/rach2-probes/hosts.absent
fi
printf '127.0.0.1   localhost\n10.0.2.2    gateway\n10.0.2.3    dns\n10.0.2.15   rach2\n' >/etc/hosts
run dnstest /dnstest </dev/null
restore_hosts
run modeprobe /modeprobe /rach2-probes/mode </dev/null
run devprobe /devprobe </dev/console
EOF
remote 'cat >/rach2-probes/run.sh' <"$work/run.sh"
survey "$names" >"$work/staged.log"
for name in $names; do
    have=$(sed -n "s/^have $name //p" "$work/staged.log")
    [ "$have" = "$(host_sum "$tests/$name")" ] || {
        echo "manual-probes: refused: /$name on the machine does not match $tests/$name after the copy" >&2
        exit 1
    }
done
echo "manual-probes: staged:$staged, each equal to build/tests"

echo 'manual-probes: devprobe switches the machine screen to graphics mode and back'
ran=1
remote 'sh /rach2-probes/run.sh </dev/null' 2>&1 | tee "$work/probes.log" || :
passed=0
failed=
for name in $probes; do
    status=$(sed -n "s/^manual-probes: $name: status //p" "$work/probes.log" | tr -d '\r')
    if [ "$status" = 0 ]; then
        passed=$((passed + 1))
    else
        failed="$failed $name"
    fi
done

case "${RACH2_MEASURE_SLOT:-}" in
    candidate) entry='Start Rach2' ;;
    *) entry='Start Rach2 with the previous kernel' ;;
esac
echo "manual-probes: orphanprobe: at the machine, reboot; stop the menu's countdown with an arrow key,"
echo "manual-probes: orphanprobe: select \"$entry\", press e, add rach2.rescue at the end of its multiboot line"
echo "manual-probes: orphanprobe: and boot with ctrl-x; at the kernel shell type exec /orphanprobe; the"
echo "manual-probes: orphanprobe: screen must show ORPHANPROBE OK and the init process exiting with status 0;"
echo "manual-probes: orphanprobe: type reboot, then run this script again with the mode clean"
if [ -n "$failed" ]; then
    echo "manual-probes: result: fail: $passed of nine passed over SSH; not passed:$failed" >&2
    exit 1
fi
echo 'manual-probes: result: pass: nine of nine passed over SSH; orphanprobe waits at /orphanprobe for the console'
finished=1
