#!/bin/sh
#
# release.sh: release the phase In progress: test its candidate commit with a
# full run, install that commit on the installed Lenovo, and close the phase.
#
# Usage: release.sh [-s STATE] P
#
# The manager starts it, detached in the tmux session `rach2-release`, when
# `board.sh phase` says `ready: yes SHA` for phase P, and starts it again,
# with the same arguments, when it ended without finishing. It runs on the
# owner's computer, which reaches the testing host as `ssh testing` and the
# Lenovo on its network. A release has these steps, each of which the record
# below lists once it is done:
#
# * The candidate: SHA, the commit `board.sh phase` gave, is written to the
#   record with the time it was chosen, and commented on the phase.
# * full: `full.yml` runs every suite on both architectures from clean at SHA.
#   The run is found by its name, `full at SHA`, and a start after the
#   candidate was chosen, before another is started, so a release started
#   again does not start a second, and a new candidate of the same commit
#   gets a run of its own. A run that fails ends the release, naming the `ci`
#   issues that report it, or saying that none does when its report filed
#   nothing. A run that is cancelled or does not finish is started again, at
#   most three runs in all.
# * The candidate is checked again, here, before the build, before the
#   install, and before the phase is closed: it is void unless `board.sh
#   phase` still says `ready: yes SHA`, since a later commit, an open member,
#   a `ci` issue or a hold means that SHA is not the phase's finished work.
#   A board that cannot be read is waited on.
# * build: SHA is built from clean for the Lenovo on the testing host, in
#   the copy /home/testing/rach2-clean, with `ARCH=x86_64 make clean` and
#   `make machine`, and the kernel, the recovery kernel and image, and the
#   GNU root, compressed, are fetched with their SHA-256 into a worktree of
#   SHA, STATE/wt.
# * recovery: the Lenovo boots its recovery system from its recovery
#   partition; the recovery kernel and image on that partition are replaced
#   when they differ from the build's, keeping the partition's recovery key
#   and the archive of the machine's accounts, and the machine boots the
#   recovery system again, which is then the build's. The old kernel and
#   root stay as they were, so a recovery system that does not start leaves
#   a machine that a power cycle returns to them.
# * transfer: the kernel and the root go to the recovery system's /dev/shm,
#   the root decompressed on the way, and each is checked by SHA-256.
# * kernel and install: `bootctl kernel` writes the kernel to the candidate
#   slot, and `rach2-install --update` writes the root, keeping the machine's
#   accounts and keys, and arms the candidate for one trial boot. The
#   installer runs detached on the machine, with its log and its status in
#   /dev/shm, so that a lost connection does not stop it; it is started once,
#   and a release started again finds it by its log and waits for it. An
#   install that a reboot interrupted, or that ended without success, is
#   never started again by this script: it ends the release for a person.
#   While the record has its `install started` line and no `install done`,
#   every later start of the script stops before it changes anything; the
#   person checks the machine's root by hand and then deletes that line.
# * trial: the machine boots the trial, and machine_identity.sh must find the
#   new kernel running from the candidate slot.
# * accept: `tests/system/accept/metal_accept.sh` must pass on the trial
#   boot. Only then `bootctl commit` makes the candidate the saved default,
#   and /etc/rach2/release on the machine records SHA, the phase and the run.
# * close: the phase gets a comment with the commit, the run, the digests and
#   the verdicts; when the candidate is still ready, the record says
#   `closing`, the phase is closed, its cards are archived with `board.sh
#   archive`, and the owner's phone gets one push. Otherwise the phase stays
#   In progress for its next release. Only a phase already closed skips the
#   check. A start of the script that finds a record with `closing` that did
#   not end finishes that release first, whatever P it was given.
#
# The machine must report the station address RACH2_MACHINE_MAC,
# 28:d2:44:7e:e0:af by default, and its disk the serial RACH2_MACHINE_SERIAL,
# S6P2NX0T381638Z by default, the Lenovo's (dev-log/43-machine/hardware.md),
# at RACH2_MACHINE_ADDRESS, 192.168.0.106 by default; the disk is found by
# that serial before every write. Host keys are not checked, since the
# recovery system makes new ones at every boot; the address and the serial
# identify the machine.
#
# STATE, `~/flow/manager/release` unless given, holds `record`, the record
# of the release under way, one line per fact: `phase P`, `sha SHA`,
# `chosen TIME`, `dispatched TIME`, `run ID RESULT`, `STEP done TIME DETAIL`,
# `touched TIME` before the first change to the machine, `install started
# TIME`, `reported TIME` once the phase has the report, `closing TIME`, and
# last `failed STEP: WHY`, `void: WHY` or `closed TIME`; `log`, everything
# the script prints; `wt`, the worktree; and the records of releases that
# ended, as `P-SHA-TIME`. A record that ended, or that belongs to another
# phase, is copied aside once the board has given a new candidate, and the
# new record then replaces it in one rename. The new record keeps the old
# one's `touched` line unless the old release closed, since the machine may
# still run what that release left. A done step is skipped when the script
# runs again, a transfer whose files still hash right on the machine is
# kept, and a step that writes the machine first asks it what it runs and
# which disk has the serial. A reboot counts as done when a boot younger
# than the request answers, by /proc/uptime. One release runs at a time,
# under a lock on STATE/lock.
#
# The exit status is 0 when the phase was released and closed, or released
# and kept open; 75 when it could not start or go on for a reason that
# passes, such as an unreachable host or a busy build copy, and the manager
# starts it again later; and 1 when a step failed, as the record's last line
# says, and the manager reads why.

REPO=toksaitov/rach2
COPY=/home/testing/rach2-clean
ARCH_DIR=build/x86_64
RECOVERY_IMAGE=images/rach2-recovery-with-key.krffs

# say TEXT: print TEXT and append it to the log, with the time in UTC.
say()
{
    echo "$(date -u '+%F %T') release: $*" | tee -a "$state/log"
}

# note LINE: append LINE to the record.
note()
{
    echo "$*" >> "$state/record"
}

# done_ STEP: succeed when the record says STEP is done.
done_()
{
    grep -q "^$1 done " "$state/record"
}

# mark STEP DETAIL: record STEP as done now.
mark()
{
    note "$1 done $(date -u '+%FT%TZ') $2"
    say "$1: done: $2"
}

# fail STEP WHY: record that STEP failed, say so on the phase, and exit 1.
fail()
{
    note "failed $1: $2"
    say "$1: failed: $2"
    gh issue comment "$phase" --repo "$REPO" --body "The release of $sha failed at its step \`$1\`: $2. The record is \`$state/record\` on the manager's machine." > /dev/null ||
        say "the comment on #$phase failed"
    exit 1
}

# later WHY: say why the release cannot go on now, and exit 75.
later()
{
    say "waiting: $1"
    exit 75
}

# lenovo COMMAND: run COMMAND on the machine as root.
lenovo()
{
    ssh -i "$repo/krffs/rach2_demo_ed25519" -o BatchMode=yes -o ConnectTimeout=10 \
        -o ServerAliveInterval=15 -o ServerAliveCountMax=4 -o LogLevel=ERROR \
        -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no \
        "root@$address" "$@"
}

# where: ask the machine what it runs, and set `now` to `system SLOT` or
# `recovery`, `disk` to the number of the disk with the serial, and `up` to the
# seconds since it booted; set `now` empty when it does not answer. Fail, with
# the reason on stderr, when it answers as another machine.
where()
{
    now='' disk='' up=''
    facts=$(lenovo 'cat /sys/class/net/eth0/address; cat /proc/cmdline; cat /proc/uptime; cat /proc/disk' 2> /dev/null) ||
        return 0
    [ "$(printf '%s\n' "$facts" | sed -n 1p)" = "$mac" ] || {
        say "the machine at $address reports station address $(printf '%s\n' "$facts" | sed -n 1p), not $mac" >&2
        return 1
    }
    disk=$(printf '%s\n' "$facts" | awk -v s="$serial" '$1 == "identity" && $NF == s { print $2 }')
    [ -n "$disk" ] || {
        say "the machine at $address has no disk with serial $serial" >&2
        return 1
    }
    up=$(printf '%s\n' "$facts" | sed -n 3p | cut -d . -f 1)
    slot=$(printf '%s\n' "$facts" | sed -n 2p | tr ' ' '\n' | sed -n 's/^rach2\.slot=//p')
    case $slot in
        recovery) now=recovery ;;
        *) now="system $slot" ;;
    esac
}

# reboot_into WANT: reboot the machine and wait up to ten minutes for a new
# boot, one whose uptime is shorter than the time since the reboot was asked
# for, to answer running WANT, `recovery` or `system`; fail the step STEP
# otherwise.
reboot_into()
{
    asked=$(date +%s)
    lenovo 'setsid sh -c "sleep 2; reboot" > /dev/null 2>&1 < /dev/null &' > /dev/null 2>&1
    sleep 30
    while :; do
        where || fail "$step" "the machine answered as another machine"
        if [ -n "$now" ] && [ "$up" -lt $(($(date +%s) - asked)) ]; then
            case $now in
                "$1"*) return 0 ;;
                *) fail "$step" "the machine came up running $now, not $1" ;;
            esac
        fi
        [ $(($(date +%s) - asked)) -lt 600 ] ||
            fail "$step" "no new boot has answered in the ten minutes since the machine was rebooted to $1; it needs a person to power-cycle it"
        sleep 10
    done
}

# digest FILE: print FILE's SHA-256 as the build listed it.
digest()
{
    awk -v f="$1" '$2 == f { print $1 }' "$files/release.sha256"
}

# still_ready: succeed when `board.sh phase` still says `ready: yes SHA` for
# the phase: SHA is master's head, reviewed through, with no work, hold or
# core failure; otherwise print why and fail, with status 2 when the board
# cannot be read.
still_ready()
{
    report=$(sh "$repo/tools/flow/board.sh" phase) || {
        echo "the board cannot be read"
        return 2
    }
    printf '%s\n' "$report" | grep -q "^phase: #$phase " || {
        echo "#$phase is not the phase In progress"
        return 1
    }
    last=$(printf '%s\n' "$report" | tail -n 1)
    [ "$last" = "ready: yes $sha" ] || {
        echo "$last"
        return 1
    }
}

# void WHY: record that the candidate is void for WHY, and exit 0.
void()
{
    note "void: $1"
    say "the candidate is void: $1"
    exit 0
}

# failed_run ID: end the release on the full run ID that failed, naming the
# `ci` issues that report it, or saying that none does, since then its report
# did not file its failures.
failed_run()
{
    filed=$(gh issue list --repo "$REPO" --label ci --state open --limit 200 \
        --json number,body,comments --jq "[.[] |
            select((.body + ([.comments[].body] | join(\" \"))) |
                contains(\"/actions/runs/$1\")) | \"#\\(.number)\"] | join(\" \")") ||
        later "the ci issues cannot be listed"
    if [ -n "$filed" ]; then
        fail full "run $1 failed; ci filed its failures as $filed"
    fi
    fail full "run $1 failed and no open ci issue names it, so its report filed nothing; read the run's log"
}

# full: find or start the full run of SHA and wait for it to end, first
# applying a result the record has and has not acted on.
full()
{
    # shellcheck disable=SC2046 # the result is two words
    set -- $(grep -E '^run [0-9]+ (success|failure)$' "$state/record" | tail -n 1)
    case ${3-} in
        success) mark full "run $2"; return 0 ;;
        failure) failed_run "$2" ;;
    esac
    while :; do
        run=$(gh run list --repo "$REPO" --workflow full.yml --limit 50 \
            --json databaseId,displayTitle,status,conclusion,createdAt \
            --jq "[.[] | select(.displayTitle == \"full at $sha\" and
                .createdAt >= \"$chosen\")][0] |
                select(. != null) | \"\(.databaseId) \(.status) \(.conclusion)\"") ||
            later "the full runs cannot be listed"
        # shellcheck disable=SC2086 # the run is three words
        set -- $run
        if [ -n "$run" ] && ! grep -q "^run $1 " "$state/record"; then
            if [ "$2" != completed ]; then
                say "full: run $1 is $2"
                sleep 300
                continue
            fi
            note "run $1 $3"
            case $3 in
                success) mark full "run $1"; return 0 ;;
                failure) failed_run "$1" ;;
            esac
            say "full: run $1 ended $3"
            continue
        fi
        last=$(sed -n 's/^dispatched //p' "$state/record" | tail -n 1)
        if [ -n "$last" ] && [ $(($(date +%s) - last)) -lt 600 ]; then
            sleep 30
            continue
        fi
        [ "$(grep -c '^dispatched ' "$state/record")" -lt 3 ] ||
            fail full "three full runs of $sha ended without a result"
        note "dispatched $(date +%s)"
        gh workflow run full.yml --repo "$REPO" --ref master -f sha="$sha" ||
            later "the full run could not be started"
        say "full: started a run of $sha"
        sleep 30
    done
}

# build: build SHA for the Lenovo on the testing host and fetch the results.
build()
{
    git -C "$repo" fetch -q origin || later "the fetch failed"
    git -C "$repo" worktree remove --force "$state/wt" 2> /dev/null
    rm -rf "$state/wt"
    git -C "$repo" worktree prune
    git -C "$repo" worktree add -q --detach "$state/wt" "$sha" ||
        fail build "no worktree of $sha"
    cd "$state/wt" || fail build "no worktree"
    if ! RACH2_TESTING_COPY=$COPY sh tools/dev/testing.sh run \
        "ARCH=x86_64 make clean && ARCH=x86_64 make machine && cd $ARCH_DIR &&
        gzip -1 -c images/os-real-gnu.ext2 > images/os-real-gnu.ext2.gz &&
        sha256sum rach2/kernel rach2-recovery/kernel $RECOVERY_IMAGE images/os-real-gnu.ext2 \
            > images/release.sha256" > "$state/build.log" 2>&1; then
        if grep -q 'testing: the copy at .* is in use\|testing: the host did not answer' "$state/build.log"; then
            later "the build copy is busy or the host does not answer"
        fi
        fail build "the build failed: $(tail -n 3 "$state/build.log" | tr '\n' ' ')"
    fi
    RACH2_TESTING_COPY=$COPY sh tools/dev/testing.sh get "$ARCH_DIR/rach2/kernel" \
        "$ARCH_DIR/rach2-recovery/kernel" "$ARCH_DIR/$RECOVERY_IMAGE" \
        "$ARCH_DIR/images/os-real-gnu.ext2.gz" "$ARCH_DIR/images/release.sha256" \
        >> "$state/build.log" 2>&1 || later "the build's results could not be fetched"
    for f in rach2/kernel rach2-recovery/kernel "$RECOVERY_IMAGE"; do
        [ "$(shasum -a 256 "$files/../$f" 2> /dev/null | cut -d ' ' -f 1)" = "$(digest "$f")" ] ||
            fail build "$f did not arrive whole"
    done
    [ "$(gzip -dc "$files/os-real-gnu.ext2.gz" | shasum -a 256 | cut -d ' ' -f 1)" = \
        "$(digest images/os-real-gnu.ext2)" ] || fail build "the root did not arrive whole"
    mark build "kernel $(digest rach2/kernel) root $(digest images/os-real-gnu.ext2)"
}

# send FILE DEST: copy FILE to DEST on the machine and check its SHA-256.
send()
{
    lenovo "cat > $2" < "$1" || fail "$step" "the copy of $1 did not finish"
    [ "$(lenovo "sha256sum $2" | cut -d ' ' -f 1)" = "$(shasum -a 256 "$1" | cut -d ' ' -f 1)" ] ||
        fail "$step" "$2 does not hash as $1 does"
}

# recovery: put the build's recovery system on the recovery partition and
# boot it, the machine running recovery when this is called.
recovery()
{
    step=recovery
    k=$(digest rach2-recovery/kernel) i=$(digest "$RECOVERY_IMAGE")
    p2=/dev/disk${disk}p2
    have=$(lenovo "mkdir -p /mnt/p2 && mount -t vfat -o ro $p2 /mnt/p2 &&
        sha256sum /mnt/p2/boot/recovery /mnt/p2/boot/recovery.krffs; umount /mnt/p2") ||
        fail recovery "the recovery partition $p2 could not be read"
    if [ "$(printf '%s\n' "$have" | awk '{ printf "%s ", $1 }')" != "$k $i " ]; then
        say "recovery: writing the recovery kernel and image to $p2"
        lenovo "mount -t vfat $p2 /mnt/p2" || fail recovery "$p2 would not mount"
        send "$files/../rach2-recovery/kernel" /mnt/p2/boot/recovery.new
        send "$files/../$RECOVERY_IMAGE" /mnt/p2/boot/recovery.krffs.new
        lenovo 'cd /mnt/p2/boot && mv recovery.new recovery && mv recovery.krffs.new recovery.krffs &&
            sync && cd / && umount /mnt/p2' || fail recovery "the new recovery files could not be put in place"
    fi
    lenovo "bootctl --target /dev/disk${disk}p1 trial recovery" > /dev/null ||
        fail recovery "a trial of recovery could not be armed"
    reboot_into recovery
    [ "$(lenovo 'sha256sum /bin/rach2-install' | cut -d ' ' -f 1)" = \
        "$(shasum -a 256 "$state/wt/utils/rach2-install" | cut -d ' ' -f 1)" ] ||
        fail recovery "the recovery system that runs is not the build's: its rach2-install differs"
    mark recovery "kernel $k image $i"
}

# install: send the kernel and the root to the machine in recovery, install
# the kernel, and run the root's installer once; or wait for the run that a
# release before this one started.
install()
{
    step=install
    started=$(lenovo 'cat /dev/shm/install.status 2> /dev/null
        test -e /dev/shm/install.log && echo running; echo answered') &&
        [ "$(printf '%s\n' "$started" | tail -n 1)" = answered ] ||
        later "the machine does not answer"
    started=$(printf '%s\n' "$started" | sed '$d')
    if grep -q '^install started ' "$state/record" && [ -z "$started" ]; then
        fail install "the installer was started and its log is gone, so a reboot interrupted it; run rach2-install --update by hand from recovery"
    fi
    root=$(digest images/os-real-gnu.ext2)
    if [ -z "$started" ] && { ! done_ transfer ||
        [ "$(lenovo 'sha256sum /dev/shm/root.ext2 /dev/shm/kernel 2> /dev/null' |
            awk '{ printf "%s ", $1 }')" != "$root $(digest rach2/kernel) " ]; }; then
        step=transfer
        lenovo 'rm -f /dev/shm/kernel /dev/shm/root.ext2 /dev/shm/root.ext2.sha256' ||
            later "the machine does not answer"
        free=$(lenovo "df -k /dev/shm | awk 'NR == 2 { print \$4 }'")
        need=$(($(gzip -dc "$files/os-real-gnu.ext2.gz" | wc -c) / 1024 + 262144))
        [ "${free:-0}" -gt "$need" ] ||
            fail transfer "/dev/shm has $free KiB free and the root needs $need"
        send "$files/../rach2/kernel" /dev/shm/kernel
        lenovo 'gzip -dc > /dev/shm/root.ext2' < "$files/os-real-gnu.ext2.gz" ||
            fail transfer "the root did not arrive"
        [ "$(lenovo 'sha256sum /dev/shm/root.ext2' | cut -d ' ' -f 1)" = "$root" ] ||
            fail transfer "the root on the machine does not hash as the build's"
        lenovo "echo $root > /dev/shm/root.ext2.sha256" || fail transfer "no digest file"
        mark transfer "kernel and root in /dev/shm"
    fi
    if [ -z "$started" ]; then
        if ! done_ kernel; then
            step=kernel
            lenovo 'bootctl kernel /dev/shm/kernel' > "$state/kernel.log" 2>&1
            grep -q '^bootctl: installed:' "$state/kernel.log" ||
                fail kernel "$(tr '\n' ' ' < "$state/kernel.log")"
            mark kernel "candidate slot"
        fi
        step=install
        note "install started $(date -u '+%FT%TZ')"
        lenovo "setsid sh -c 'rach2-install --update /dev/shm/root.ext2 /dev/disk$disk \
            > /dev/shm/install.log 2>&1; echo \$? > /dev/shm/install.status' \
            > /dev/null 2>&1 < /dev/null &" || fail install "the installer could not be started"
        say "install: rach2-install --update runs on the machine"
    fi
    i=0
    while :; do
        status=$(lenovo 'cat /dev/shm/install.status 2> /dev/null') ||
            say "install: the machine did not answer; waiting"
        case $status in
            0) break ;;
            [0-9]*) fail install "rach2-install ended $status: $(lenovo 'tail -n 5 /dev/shm/install.log' | tr '\n' ' ')" ;;
        esac
        i=$((i + 1))
        [ "$i" -lt 240 ] || fail install "rach2-install has not ended after an hour"
        sleep 15
    done
    lenovo 'cat /dev/shm/install.log' >> "$state/log" 2>&1
    mark install "root $(digest images/os-real-gnu.ext2)"
}

main()
{
    state=$HOME/flow/manager/release
    while getopts s: o; do
        case $o in
            s) state=$OPTARG ;;
            *) echo "usage: release.sh [-s STATE] P" >&2; exit 2 ;;
        esac
    done
    shift $((OPTIND - 1))
    case ${1-} in
        ''|*[!0-9]*) echo "usage: release.sh [-s STATE] P" >&2; exit 2 ;;
    esac
    phase=$1
    repo=$(cd "$(dirname "$0")/../.." && pwd)
    address=${RACH2_MACHINE_ADDRESS:-192.168.0.106}
    mac=${RACH2_MACHINE_MAC:-28:d2:44:7e:e0:af}
    serial=${RACH2_MACHINE_SERIAL:-S6P2NX0T381638Z}
    mkdir -p "$state"
    state=$(cd "$state" && pwd)
    files=$state/wt/$ARCH_DIR/images
    exec 9>> "$state/lock"
    perl -MFcntl=:flock -e 'open(L, ">&=9") && flock(L, LOCK_EX | LOCK_NB) or exit 1' ||
        later "another release runs"
    chmod 600 "$repo/krffs/rach2_demo_ed25519"

    if [ -f "$state/record" ] && grep -q '^closing ' "$state/record" &&
        ! grep -q '^closed \|^void\|^failed ' "$state/record"; then
        phase=$(sed -n 's/^phase //p' "$state/record")
        say "#$phase: its release closed the phase and has not finished; finishing it"
    fi
    old=
    if [ -f "$state/record" ] && { ! grep -qx "phase $phase" "$state/record" ||
        grep -q '^failed \|^void\|^closed ' "$state/record"; }; then
        if grep -q '^install started ' "$state/record" &&
            ! grep -q '^install done ' "$state/record"; then
            say "the record has an installer that was started and whose success no release saw; check the machine's root by hand, then delete the record's \`install started\` line"
            exit 1
        fi
        old=$(sed -n 's/^phase //p' "$state/record")-$(sed -n 's/^sha //p' "$state/record" | cut -c 1-8)
    fi
    if [ -n "$old" ] || [ ! -f "$state/record" ]; then
        report=$(sh "$repo/tools/flow/board.sh" phase) || later "the board cannot be read"
        printf '%s\n' "$report" | grep -q "^phase: #$phase " ||
            later "#$phase is not the phase In progress"
        sha=$(printf '%s\n' "$report" | sed -n 's/^ready: yes \([0-9a-f]\{40\}\)$/\1/p')
        [ -n "$sha" ] || later "#$phase is not ready: $(printf '%s\n' "$report" | tail -n 1)"
        {
            printf 'phase %s\nsha %s\nchosen %s\n' "$phase" "$sha" "$(date -u '+%FT%TZ')"
            [ -z "$old" ] || grep -q '^closed ' "$state/record" ||
                grep '^touched ' "$state/record" | tail -n 1
        } > "$state/record.new"
        [ -z "$old" ] || cp "$state/record" "$state/$old-$(date -u '+%m%d-%H%M%S')" ||
            later "the old record could not be kept"
        mv "$state/record.new" "$state/record"
        say "#$phase: the candidate is $sha"
        gh issue comment "$phase" --repo "$REPO" --body "Release: the candidate is $sha. Its full run starts now; the phase closes when that commit has passed it and runs on the Lenovo." > /dev/null ||
            say "the comment on #$phase failed"
    fi
    sha=$(sed -n 's/^sha //p' "$state/record")
    chosen=$(sed -n 's/^chosen //p' "$state/record")

    if ! done_ accept; then
        where || fail preflight "the machine at $address is not the Lenovo"
        [ -n "$now" ] || later "the machine at $address does not answer"
        if ! grep -q '^touched ' "$state/record"; then
            case $now in
                "system candidate"|"system fallback") ;;
                *) fail preflight "the machine runs $now before the release has touched it" ;;
            esac
            lenovo 'bootctl status' | grep -Eq '^bootctl: next_entry: (empty|absent)' ||
                fail preflight "a trial is armed on the machine"
        fi
    fi

    done_ full || full
    if ! done_ build; then
        why=$(still_ready)
        case $? in
            0) ;;
            2) later "$why" ;;
            *) void "$why" ;;
        esac
        ssh -o BatchMode=yes -o ConnectTimeout=10 testing true 2> /dev/null ||
            later "the testing host does not answer"
        build
    fi
    cd "$state/wt" || fail build "no worktree"
    chmod 600 krffs/rach2_demo_ed25519

    if ! done_ install; then
        step=recovery
        if ! grep -q '^install started ' "$state/record"; then
            why=$(still_ready)
            case $? in
                0) ;;
                2) later "$why" ;;
                *) void "$why" ;;
            esac
        fi
        where || fail recovery "the machine answered as another machine"
        case $now in
            recovery) ;;
            system*)
                ! grep -q '^install started ' "$state/record" ||
                    fail install "the machine runs $now after the installer was started, so a reboot came before the release saw its result; check the root by hand"
                note "touched $(date -u '+%FT%TZ')"
                lenovo 'bootctl trial recovery' > /dev/null ||
                    fail recovery "a trial of recovery could not be armed"
                reboot_into recovery ;;
            *) later "the machine does not answer" ;;
        esac
        done_ recovery || recovery
        install
    fi

    if ! done_ trial; then
        step=trial
        where || fail trial "the machine answered as another machine"
        case $now in
            recovery) reboot_into system ;;
            "system candidate") ;;
            *) fail trial "the machine runs ${now:-nothing that answers} after the install" ;;
        esac
        where && [ "$now" = "system candidate" ] ||
            fail trial "the trial boot is not from the candidate slot"
        RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL=$files/../rach2/kernel \
            sh tools/dev/machine_identity.sh "$state/identity" ssh -i "$repo/krffs/rach2_demo_ed25519" \
            -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR -o UserKnownHostsFile=/dev/null \
            -o StrictHostKeyChecking=no "root@$address" > /dev/null 2>&1 ||
            fail trial "the machine does not run the new kernel from the candidate slot: $(tail -n 3 "$state/identity" | tr '\n' ' ')"
        mark trial "the new kernel runs from the candidate slot"
    fi

    if ! done_ accept; then
        step=accept
        mkdir -p build/images
        make krffs > "$state/krffs.log" 2>&1 || later "the host KRFFS tools did not build"
        sh tests/system/accept/metal_accept.sh "$address" > "$state/accept.log" 2>&1 ||
            fail accept "metal_accept.sh failed: $(grep -i 'fail' "$state/accept.log" | tail -n 3 | tr '\n' ' ')"
        lenovo 'bootctl commit' > "$state/commit.log" 2>&1
        grep -q '^bootctl: committed:' "$state/commit.log" ||
            fail accept "bootctl commit: $(tr '\n' ' ' < "$state/commit.log")"
        run=$(sed -n 's/^run \([0-9]*\) success$/\1/p' "$state/record" | tail -n 1)
        lenovo "mkdir -p /etc/rach2 && printf 'commit %s\nphase %s\nfull %s\nroot %s\n' $sha $phase $run $(digest images/os-real-gnu.ext2) > /etc/rach2/release" ||
            say "accept: /etc/rach2/release could not be written"
        mark accept "metal-accept: PASS; committed"
    fi

    run=$(sed -n 's/^run \([0-9]*\) success$/\1/p' "$state/record" | tail -n 1)
    if ! grep -q '^reported ' "$state/record"; then
        gh issue comment "$phase" --repo "$REPO" --body "Released $sha to the Lenovo.

* Full run: https://github.com/$REPO/actions/runs/$run passed on both architectures.
* Build: \`ARCH=x86_64 make clean && ARCH=x86_64 make machine\` in $COPY.
* Kernel $(digest rach2/kernel), recovery kernel $(digest rach2-recovery/kernel), recovery image $(digest "$RECOVERY_IMAGE"), GNU root $(digest images/os-real-gnu.ext2), each by SHA-256.
* The recovery partition was updated, the kernel and root installed from recovery, the trial boot passed machine_identity.sh and metal_accept.sh, and \`bootctl commit\` made it the saved default." > /dev/null ||
            later "the report could not be commented"
        note "reported $(date -u '+%FT%TZ')"
    fi
    issue_state=$(gh issue view "$phase" --repo "$REPO" --json state --jq .state) ||
        later "#$phase cannot be read"
    if [ "$issue_state" != CLOSED ]; then
        why=$(still_ready)
        case $? in
            0) ;;
            2) later "$why" ;;
            *)
                gh issue comment "$phase" --repo "$REPO" --body "This phase stays In progress: $why. The Lenovo runs $sha; the next release takes the rest." > /dev/null ||
                    later "the comment on #$phase failed"
                void "$why" ;;
        esac
        note "closing $(date -u '+%FT%TZ')"
        gh issue close "$phase" --repo "$REPO" > /dev/null || later "#$phase could not be closed"
    fi
    sh "$repo/tools/flow/board.sh" 'done' > /dev/null || later "board.sh done failed"
    sh "$repo/tools/flow/board.sh" archive "$phase" > /dev/null || later "board.sh archive $phase failed"
    sh "$repo/tools/flow/notify.sh" "rach2: phase #$phase released; the Lenovo runs $(echo "$sha" | cut -c 1-8)" ||
        say "the push to the owner's phone failed"
    note "closed $(date -u '+%FT%TZ')"
    say "#$phase is released and closed"
    old=$phase-$(echo "$sha" | cut -c 1-8)-$(date -u '+%m%d-%H%M%S')
    mv "$state/record" "$state/$old"
}

main "$@"
