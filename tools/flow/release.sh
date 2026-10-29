#!/bin/sh
#
# release.sh: release the phase In progress: test its candidate commit with a
# full run on each machine, install that commit on the installed Lenovo, and
# close the phase.
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
# * full: `full.yml` runs every suite on both architectures from clean at SHA,
#   on a runner of the one machine that SHA's `tools/ci/site` selects, which
#   asks QEMU for the accelerator the file names and no other.
#   The run is found by its title, `Release tests of SHORT for phase #P`
#   with SHORT the first eight digits of SHA, and a start after the
#   candidate was chosen, before another is started, so a release started
#   again does not start a second, and a new candidate of the same commit
#   gets a run of its own. A run that fails ends the release, naming the `ci`
#   issues that report it, or saying that none does when its report filed
#   nothing. A run that is cancelled or does not finish is started again, at
#   most three runs in all.
# * A rerun: a candidate that differs from the last candidate of the same
#   phase only by the fixes of its full run's failures is tested by a run of
#   only the suites that failed, titled `Release tests of SHORT for phase #P:
#   again only SUITE on ARCH, ...`. That holds when the last record failed at
#   `full` with ci's issues for its failures; every one of those issues is
#   closed and titled `Release tests fail on ARCH: SUITE, ...` with suites the
#   registry has for ARCH at SHA; every architecture that the run did not pass
#   has one; every first-parent commit since the last candidate closes one
#   of them, with a `Closes #N` line, and closes no other issue; and SHA's
#   `tools/ci/site` selects the machine and the accelerator that the last
#   candidate's did, so that the rerun and the suites the failed run passed
#   were tested on one machine. Anything else, or anything that cannot be read,
#   gives the candidate a full run. An issue of the failed run that is still
#   open, such as one that bisection labelled `intermittent`, has no fix, so
#   the next candidate, the same commit included, gets a full run; the release
#   changes no issue but its phase.
# * The other machines: tools/flow/machines names them and gives each run's
#   settings. Each runs `etude run routine-sequence phase_first_run` of every
#   case of both architectures, prepared from clean, with the selection that
#   `etude run -N` allocates in the same copy just before. The Mac's run, mac,
#   starts with the full run, detached as `release.sh -m ID P`, in the copy and
#   the admission site under build/machines/mac of this clone, and the release
#   waits for it before the install. Windows' run, windows, starts once the full
#   run has passed, on the same testing host, in the copy the machines file
#   names, which a bundle of `origin/master` updates; while it runs, a claim of
#   the whole host in WSL's admission, `etude-launch alone`, keeps WSL's tests
#   from starting. The Lenovo's run, lenovo, is the native cases of
#   tests/native on the trial boot, which `native-deploy run` starts from here
#   with the payload the build makes, before the acceptance. A run that ends
#   without a status runs again, at most three runs in all. A run that fails
#   files a `ci` issue titled `Release tests fail on MACHINE: WORD, ...` with
#   the suites, `ARCH:SUITE`, or the native cases that failed, and ends the
#   release. The next candidate of the same phase runs again only those, on
#   that machine, and keeps the runs that passed on the others, when that issue
#   is closed and every first-parent commit since the failed candidate closes
#   it and no other; anything else gives it every run.
# * The candidate is checked again, here, before the build, before the
#   install, and before the phase is closed: it is void unless `board.sh
#   phase` still says `ready: yes SHA`, since a later commit, an open member,
#   a `ci` issue or a hold means that SHA is not the phase's finished work.
#   A board that cannot be read is waited on.
# * build: SHA is built from clean for the Lenovo on the testing host, in
#   the copy `build/worktrees/release` of the owner's, /home/testing/rach2, with
#   `ARCH=x86_64 make clean` and `make machine GNU_AGENTS=all`, whose GNU
#   root has every package of the agents layer, and `make native-assets` when
#   the machines file names the Lenovo, and the kernel, the recovery kernel
#   and image, the GNU root, compressed, and the native cases' payload are
#   fetched with their SHA-256 into a worktree of SHA, STATE/wt.
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
# * accept: the native cases and `tests/system/accept/metal_accept.sh` must
#   pass on the trial boot. Only then `bootctl commit` makes the candidate the
#   saved default.
# * record: /etc/rach2/release on the machine, which runs the committed
#   candidate, is written with SHA, the phase, the full run and the root's
#   digest, flushed to the disk with the directories that hold it, and read
#   back. A flush that fails fails the step, since the read-back may come
#   from the buffer cache; the phase is not closed until the file names SHA.
# * close: the phase gets a comment with the commit, the run, the machine and
#   the accelerator it tested on, the digests and the verdicts; when the
#   candidate is still ready, the record says
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
# STATE, `build/flow/release` of the clone's main working tree unless given,
# holds `record`, the record of the release under way, one line per fact: `phase
# P`, `sha SHA`, `chosen TIME`, `rerun R O ARCH:SUITE...` when run R at commit O
# failed and only those suites run again, `again MACHINE WORD...` when only
# those run again on MACHINE, `dispatched TIME`, `run ID RESULT`, `filed ID
# #N...` for the issues that report a run that failed, `MACHINE started RUN`,
# `result MACHINE RUN failure WORD...` and `filed MACHINE #N WORD...` for a run
# on another machine, `STEP done TIME DETAIL`, `touched TIME` before the first
# change to the machine, `install started TIME`, `reported TIME` once the phase
# has the report, `closing TIME`, and last `failed STEP: WHY`, `void: WHY` or
# `closed TIME`; `log`, everything the script prints; `wt`, the worktree; and
# the records of releases that ended, as `P-SHA-TIME`, with the build's digests
# beside each closed one as `P-SHA-TIME.sha256`. A release that closes its phase
# copies the digests out of `wt` and then removes `wt`, with the build's
# products in it, last. A record that ended, or that belongs to another phase,
# is copied aside once the board has given a new candidate, and the new record
# then replaces it in one rename. The new record keeps the old one's `touched`
# line unless the old release closed, since the machine may still run what that
# release left. A done step is skipped when the script runs again, a transfer
# whose files still hash right on the machine is kept, and a step that writes
# the machine first asks it what it runs and which disk has the serial. A reboot
# counts as done when a boot younger than the request answers, by /proc/uptime.
# One release runs at a time, under a lock on STATE/lock.
#
# The exit status is 0 when the phase was released and closed, or released
# and kept open; 75 when it could not start or go on for a reason that
# passes, such as an unreachable host or a busy build copy, and the manager
# starts it again later; and 1 when a step failed, as the record's last line
# says, and the manager reads why.

REPO=toksaitov/rach2
OWNER=/home/testing/rach2
COPY=$OWNER/build/worktrees/release
ARCH_DIR=build/x86_64
RECOVERY_IMAGE=images/rach2-recovery-with-key.krffs
WSL='MSYS_NO_PATHCONV=1 wsl.exe -- bash -s'

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
    case $1 in
        preflight) what="the check of the Lenovo before the release changes it" ;;
        full) what="the run of the release tests on both architectures" ;;
        mac) what="the whole run of the release tests on the Mac" ;;
        windows) what="the whole run of the release tests on Windows on the testing host" ;;
        lenovo) what="the native cases on the Lenovo's trial boot" ;;
        build) what="the build for the Lenovo on the testing host" ;;
        recovery) what="the update of the Lenovo's recovery system" ;;
        transfer) what="the copy of the kernel and the root to the Lenovo" ;;
        kernel) what="the write of the kernel to the candidate slot" ;;
        install) what="the install of the root" ;;
        trial) what="the trial boot of the new system" ;;
        accept) what="the acceptance tests on the trial boot" ;;
        record) what="the write of /etc/rach2/release on the Lenovo" ;;
        *) what="a step of the release" ;;
    esac
    gh issue comment "$phase" --repo "$REPO" --body "The release of commit $sha failed at the step \`$1\`, $what: $2. The phase stays In progress. The release's record is \`$state/record\` on the manager's computer." > /dev/null ||
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
        note "filed $1 $filed"
        fail full "run $1 failed; ci filed its failures as $filed"
    fi
    fail full "run $1 failed and no open ci issue names it, so its report filed nothing; read the run's log"
}

# site REV: print the machine and the accelerator of the release tests that
# REV's tools/ci/site selects, as tools/ci/site.sh reads them; fail when the
# file cannot be read or the script refuses it.
site()
{
    git -C "$repo" show "$1:tools/ci/site" 2> /dev/null | sh "$repo/tools/ci/site.sh" 2> /dev/null
}

# plain ARCH:SUITE...: print the suites as `SUITE on ARCH, ...`.
plain()
{
    printf '%s\n' "$@" | awk -F: '{ printf "%s%s on %s", (NR > 1 ? ", " : ""), $2, $1 }'
}

# again: print `R O ARCH:SUITE...` when SHA may be tested by a run of only
# the suites that failed in run R at commit O, as the rerun above says; the
# record is the last release's, which ended. Print nothing otherwise.
again()
{
    grep -qx "phase $phase" "$state/record" && grep -q '^failed full: ' "$state/record" ||
        return 0
    # shellcheck disable=SC2046 # the line is words
    set -- $(grep '^filed ' "$state/record" | tail -n 1)
    [ $# -ge 3 ] || return 0
    r=$2 o=$(sed -n 's/^sha //p' "$state/record")
    shift 2
    issues=$(echo "$*" | tr -d '#')
    git -C "$repo" fetch -q origin 2> /dev/null &&
        git -C "$repo" merge-base --is-ancestor "$o" "$sha" 2> /dev/null &&
        commits=$(git -C "$repo" rev-list --first-parent "$o..$sha" 2> /dev/null) &&
        [ -n "$commits" ] &&
        registry=$(git -C "$repo" show "$sha:tests/registry" 2> /dev/null) &&
        here=$(site "$sha") && [ "$(site "$o")" = "$here" ] || return 0
    for c in $commits; do
        closes=$(git -C "$repo" log -1 --format=%B "$c" 2> /dev/null |
            sed -n 's/^Closes #\([0-9][0-9]*\)$/\1/p')
        [ -n "$closes" ] || return 0
        for n in $closes; do
            case " $issues " in *" $n "*) ;; *) return 0 ;; esac
        done
    done
    words='' arches=''
    for n in $issues; do
        line=$(gh issue view "$n" --repo "$REPO" --json state,title --jq '"\(.state) \(.title)"' 2> /dev/null) ||
            return 0
        # shellcheck disable=SC2046 # the title's suites are words
        set -- $(printf '%s\n' "$line" |
            sed -nE 's/^CLOSED Release tests fail on (i686|x86_64): (.*)$/\1 \2/p' | sed 's/,/ /g')
        [ $# -ge 2 ] || return 0
        a=$1
        shift
        suites=$(printf '%s\n' "$registry" | awk -F'|' -v a="$a" '
            /^[a-z]/ && ($6 == "" || index("," $6 ",", "," a ",")) { print $1 }')
        for t in "$@"; do
            printf '%s\n' "$suites" | grep -qx "$t" || return 0
            words="$words $a:$t"
        done
        arches="$arches $a"
    done
    ctx=ci/full need='i686 x86_64'
    if grep -q '^rerun ' "$state/record"; then
        ctx=ci/rerun
        need=$(sed -n 's/^rerun [0-9]* [0-9a-f]* //p' "$state/record" | tr ' ' '\n' | sed 's/:.*//' | sort -u)
    fi
    states=$(gh api "repos/$REPO/commits/$o/statuses?per_page=100" \
        --jq ".[] | select(.target_url | endswith(\"/actions/runs/$r\")) | \"\(.context) \(.state)\"" 2> /dev/null) ||
        return 0
    for a in $need; do
        [ "$(printf '%s\n' "$states" | sed -n "s|^$ctx/$a ||p" | head -n 1)" != success ] || continue
        case " $arches " in *" $a "*) ;; *) return 0 ;; esac
    done
    echo "$r $o$words"
}

# full: find or start the full run of SHA, or the rerun the record gives, and
# wait for it to end, first applying a result the record has and has not
# acted on.
full()
{
    only=$(sed -n 's/^rerun [0-9]* [0-9a-f]* //p' "$state/record")
    # shellcheck disable=SC2046 # the result is two words
    set -- $(grep -E '^run [0-9]+ (success|failure)$' "$state/record" | tail -n 1)
    case ${3-} in
        success) mark full "run $2${only:+, of $only only}"; return 0 ;;
        failure) failed_run "$2" ;;
    esac
    title="Release tests of $(echo "$sha" | cut -c 1-8) for phase #$phase"
    # shellcheck disable=SC2086 # the suites are words
    [ -z "$only" ] || title="$title: again only $(plain $only)"
    while :; do
        run=$(gh run list --repo "$REPO" --workflow full.yml --limit 50 \
            --json databaseId,displayTitle,status,conclusion,createdAt \
            --jq "[.[] | select(.displayTitle == \"$title\" and
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
                success) mark full "run $1${only:+, of $only only}"; return 0 ;;
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
        set -- -f sha="$sha" -f title="$title"
        [ -z "$only" ] || set -- "$@" -f suites="$only"
        gh workflow run full.yml --repo "$REPO" --ref master "$@" ||
            later "the full run could not be started"
        say "full: started a run of $sha${only:+ for $only only}"
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
    native='' payload=''
    if has lenovo; then
        native="ARCH=x86_64 make native-assets &&"
        payload=tests/native/prepared.tar.gz
    fi
    if ! RACH2_TESTING_COPY=$COPY sh tools/dev/testing.sh run \
        "ARCH=x86_64 make clean && ARCH=x86_64 make machine GNU_AGENTS=all && $native cd $ARCH_DIR &&
        gzip -1 -c images/os-real-gnu.ext2 > images/os-real-gnu.ext2.gz &&
        sha256sum rach2/kernel rach2-recovery/kernel $RECOVERY_IMAGE images/os-real-gnu.ext2 $payload \
            > images/release.sha256" > "$state/build.log" 2>&1; then
        if grep -q 'testing: the copy at .* is in use\|testing: the host did not answer' "$state/build.log"; then
            later "the build copy is busy or the host does not answer"
        fi
        if grep -q '^preflight: result: refused: .* GiB the build needs' "$state/build.log"; then
            later "the testing host is short of space"
        fi
        fail build "the build failed: $(tail -n 3 "$state/build.log" | tr '\n' ' ')"
    fi
    RACH2_TESTING_COPY=$COPY sh tools/dev/testing.sh get "$ARCH_DIR/rach2/kernel" \
        "$ARCH_DIR/rach2-recovery/kernel" "$ARCH_DIR/$RECOVERY_IMAGE" \
        "$ARCH_DIR/images/os-real-gnu.ext2.gz" "$ARCH_DIR/images/release.sha256" \
        ${payload:+"$ARCH_DIR/$payload"} >> "$state/build.log" 2>&1 ||
        later "the build's results could not be fetched"
    for f in rach2/kernel rach2-recovery/kernel "$RECOVERY_IMAGE" $payload; do
        [ "$(shasum -a 256 "$files/../$f" 2> /dev/null | cut -d ' ' -f 1)" = "$(digest "$f")" ] ||
            fail build "$f did not arrive whole"
    done
    [ "$(gzip -dc "$files/os-real-gnu.ext2.gz" | shasum -a 256 | cut -d ' ' -f 1)" = \
        "$(digest images/os-real-gnu.ext2)" ] || fail build "the root did not arrive whole"
    if [ -n "$payload" ]; then
        rm -rf "$files/../tests/native/prepared"
        if ! mkdir -p "$files/../tests/native/prepared" ||
            ! tar -xzf "$files/../$payload" -C "$files/../tests/native/prepared"; then
            fail build "the native cases' payload could not be unpacked"
        fi
    fi
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

# setting MACHINE NAME: print each value that the machines file gives NAME for
# MACHINE, one a line.
setting()
{
    awk -v m="$1" -v n="$2" '$1 == m && $2 == n { sub(/^[^ ]+ +[^ ]+ +/, ""); print }' \
        "$repo/tools/flow/machines"
}

# has MACHINE: succeed when the machines file has a setting for MACHINE.
has()
{
    awk -v m="$1" '$1 == m { found = 1 } END { exit !found }' "$repo/tools/flow/machines"
}

# environment MACHINE: print the shell commands that give a run on MACHINE the
# variables and the PATH that the machines file gives it.
environment()
{
    setting "$1" env | sed "s/^\([A-Za-z_][A-Za-z0-9_]*\)=\(.*\)$/export \1='\2'/"
    p=$(setting "$1" path | tr '\n' :)
    [ -z "$p" ] || echo "export PATH='$p'\"\$PATH\""
}

# who MACHINE: print MACHINE as the reports name it.
who()
{
    case $1 in
        mac) echo "the Mac" ;;
        windows) echo "Windows on the testing host" ;;
        lenovo) echo "the Lenovo" ;;
    esac
}

# machine_failed MACHINE RUN LOG WORD...: file a `ci` issue for the run RUN on
# MACHINE, in which the WORDs failed, `ARCH:SUITE` words or the names of
# native cases, or `runner` for a run that named none, with the end of LOG,
# and end the release. A release started again files it once.
machine_failed()
{
    m=$1 id=$2 log=$3
    shift 3
    grep -q "^result $m $id " "$state/record" || note "result $m $id failure $*"
    what=$(echo "$*" | sed 's/ /, /g')
    url=$(gh issue create --repo "$REPO" --label ci \
        --title "Release tests fail on $(who "$m"): $what" \
        --body "The release of phase #$phase ran the release tests on $(who "$m") at commit $sha, in the run \`$id\`, and these failed: $what. The run's log is \`$log\` on the manager's computer; its end is below. When every commit since $(echo "$sha" | cut -c 1-8) closes this issue and no other, the next release runs only these again on $(who "$m"), and the runs that passed on the other machines stand.

\`\`\`
$(tail -n 40 "$log" 2> /dev/null)
\`\`\`") || later "the ci issue of the failure on $m could not be filed"
    note "filed $m #${url##*/} $*"
    fail "$m" "run $id failed: $what; filed as #${url##*/}"
}

# mac_run ID: make the run ID of the release tests on the Mac, of every suite,
# or of the words of the record's `again mac` line only, in the copy TREE with
# the admission site SITE, with its output in RUNS/ID.console and its status
# appended to RUNS/ID.times as `rc=N`.
mac_run()
{
    id=$1
    sha=$(sed -n 's/^sha //p' "$state/record")
    words=$(sed -n 's/^again mac //p' "$state/record")
    accel=$(setting mac accel)
    date '+start %FT%T%z' > "$runs/$id.times"
    # shellcheck disable=SC2086 # the suites are words
    (
        eval "$(environment mac)"
        unset ETUDE_SITE ETUDE_ADMITTED ETUDE_PREPARATION_CLEAN
        cd "$tree" && git checkout -q -f --detach "$sha" &&
            make build/tools/etude/etude build/tools/etude/etude-volume &&
            cp build/tools/etude/etude build/tools/etude/etude-volume "$site/bin/" &&
            ETUDE_SITE=$site/etude-site "$site/bin/etude" run -N -t "$tree" -a both -A "$accel" \
                -J "$runs/$id.selection.json" ${words:-all} &&
            ETUDE_PHASE_SELECTION_EXPORT=$runs/$id.selection.json ETUDE_PREPARATION_CLEAN=1 \
                "$site/bin/etude" run routine-sequence phase_first_run "$tree" "$runs/$id" both \
                "$id" "$site/etude-site" "$site/bin/etude" "$accel" ${words:-all}
    ) > "$runs/$id.console" 2>&1
    echo "rc=$?" >> "$runs/$id.times"
    date '+end %FT%T%z' >> "$runs/$id.times"
}

# mac_stop: stop the Mac's run that still runs, which belongs to an earlier
# record, with its process group, and check that the ledger has none of its
# claims.
mac_stop()
{
    [ -f "$macdir/run" ] || return 0
    read -r pg last < "$macdir/run"
    if kill -TERM -"$pg" 2> /dev/null; then
        i=0
        while kill -0 -"$pg" 2> /dev/null; do
            [ "$i" -lt 30 ] || fail mac "the earlier run on the Mac, process group $pg, did not stop"
            i=$((i + 1))
            sleep 10
        done
        say "mac: stopped the earlier run, process group $pg"
    fi
    rm -f "$macdir/run"
    [ -z "$last" ] || ! grep -qF "$last" "$site/ledger/claims" 2> /dev/null ||
        fail mac "the stopped run $last keeps a claim in $site/ledger/claims; a person checks it and removes it"
}

# mac_start: start the run of the release tests on the Mac, detached, as
# `release.sh -m ID P`, after stopping a run of an earlier record.
mac_start()
{
    [ -d "$tree" ] && [ -f "$site/etude-site" ] ||
        fail mac "the Mac has no copy at $tree or no admission site at $site"
    mac_stop
    git -C "$repo" fetch -q origin || later "the fetch failed"
    id=phase$phase-$(echo "$sha" | cut -c 1-12)-mac-$(date -u '+%m%d-%H%M%S')
    mkdir -p "$runs/$id"
    note "mac started $id"
    nohup perl -e 'setpgrp(0, 0); exec @ARGV' sh "$repo/tools/flow/release.sh" -s "$state" \
        -m "$id" "$phase" > /dev/null 2>&1 < /dev/null &
    echo "$! $id" > "$macdir/run"
    say "mac: started the run $id"
}

# mac_wait: wait for the Mac's run of the record to end, start it again when
# it ended without a status, three runs in all, and mark it done or file its
# failure.
mac_wait()
{
    step=mac
    while :; do
        id=$(sed -n 's/^mac started //p' "$state/record" | tail -n 1)
        rc=$(sed -n 's/^rc=//p' "$runs/$id.times" 2> /dev/null)
        [ -z "$rc" ] || break
        pg=$(cut -d ' ' -f 1 "$macdir/run" 2> /dev/null)
        if [ -z "$pg" ] || ! kill -0 -"$pg" 2> /dev/null; then
            rm -f "$macdir/run"
            [ "$(grep -c '^mac started ' "$state/record")" -lt 3 ] ||
                fail mac "three runs on the Mac ended without a status"
            say "mac: the run $id ended without a status; starting another"
            mac_start
            continue
        fi
        say "mac: the run $id runs"
        sleep 300
    done
    rm -f "$macdir/run"
    if [ "$rc" = 0 ]; then
        mark mac "run $id"
        return 0
    fi
    # shellcheck disable=SC2046 # the suites are words
    machine_failed mac "$id" "$runs/$id.console" $(grep '^run: result: failed: ' "$runs/$id.console" |
        tail -n 1 | sed 's/^run: result: failed: //' | grep . || echo runner)
}

# wsl COMMAND: run COMMAND in Ubuntu in WSL on the testing host.
wsl()
{
    echo "$1" | ssh -o BatchMode=yes testing "$WSL"
}

# windows: make the run of the release tests on Windows on the testing host,
# of every suite or of the words of the record's `again windows` line only,
# while a claim of the whole host in WSL's admission keeps WSL's tests from
# starting, and mark it done or file its failure; a run that ends without a
# status runs again, three runs in all.
windows()
{
    step=windows
    wtree=$(setting windows tree) out=$(setting windows out) wsite=$(setting windows site)
    accel=$(setting windows accel)
    words=$(sed -n 's/^again windows //p' "$state/record")
    held=/mnt/$(echo "$out" | cut -c 1 | tr '[:upper:]' '[:lower:]')$(echo "$out" | cut -c 3-)
    while :; do
        [ "$(grep -c '^windows started ' "$state/record")" -lt 3 ] ||
            fail windows "three runs on Windows ended without a status"
        id=phase$phase-$(echo "$sha" | cut -c 1-12)-windows-$(date -u '+%m%d-%H%M%S')
        if ! git -C "$repo" fetch -q origin ||
            ! git -C "$repo" bundle create "$state/windows.bundle" refs/remotes/origin/master > /dev/null 2>&1; then
            later "the bundle for Windows could not be made"
        fi
        scp -q -o BatchMode=yes "$state/windows.bundle" "testing:$out/release.bundle" ||
            later "the bundle could not be copied to the testing host"
        note "windows started $id"
        wsl "exec /usr/local/libexec/rach2/etude-launch alone sh -c 'touch $held/$id.held; n=0
            while [ ! -e $held/$id.end ] && [ \$n -lt 720 ]; do sleep 15; n=\$((n + 1)); done'" \
            > "$state/$id.hold" 2>&1 &
        holder=$!
        until wsl "test -e $held/$id.held" 2> /dev/null; do
            kill -0 "$holder" 2> /dev/null ||
                fail windows "WSL's admission did not give the whole host: $(tail -n 3 "$state/$id.hold" | tr '\n' ' ')"
            say "windows: waiting for WSL's tests to end"
            sleep 30
        done
        say "windows: the run $id starts"
        # shellcheck disable=SC2086 # the suites are words
        {
            echo "cd $wtree || { echo 'status 1'; exit; }"
            echo "git fetch -q $out/release.bundle refs/remotes/origin/master && git checkout -q -f --detach $sha || { echo 'status 1'; exit; }"
            environment windows
            echo "MSYS_NO_PATHCONV=1 wsl.exe -u root -e sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' < /dev/null"
            echo "ETUDE_SITE=$wsite make build/tools/etude/etude > $out/$id.make.log 2>&1 || { tail -n 20 $out/$id.make.log; echo 'status 1'; exit; }"
            echo "e=$out/etude-$id.exe"
            echo "cp build/tools/etude/etude \"\$e\" || { echo 'status 1'; exit; }"
            echo "unset ETUDE_SITE ETUDE_ADMITTED ETUDE_PREPARATION_CLEAN"
            echo "mkdir -p $out/$id $out/tmp"
            echo "ETUDE_SITE=$wsite \"\$e\" run -N -t $wtree -a both -A $accel -J $out/$id.selection.json ${words:-all} > $out/$id.selection.log 2>&1 || { tail -n 20 $out/$id.selection.log; echo 'status 1'; exit; }"
            echo "ETUDE_PHASE_SELECTION_EXPORT=$out/$id.selection.json ETUDE_PREPARATION_CLEAN=1 \"\$e\" run routine-sequence phase_first_run $wtree $out/$id both $id $wsite \"\$e\" $accel ${words:-all} > $out/$id.console 2>&1"
            # shellcheck disable=SC2016 # the remote shell expands these
            echo 'rc=$?'
            echo "tail -n 40 $out/$id.console"
            # shellcheck disable=SC2016 # the remote shell expands these
            echo 'echo "status $rc"'
        } | ssh -o BatchMode=yes testing 'C:/msys64/usr/bin/bash.exe -l -s' > "$state/$id.log" 2>&1
        wsl "touch $held/$id.end" > /dev/null 2>&1
        i=0
        while kill -0 "$holder" 2> /dev/null && [ "$i" -lt 12 ]; do
            i=$((i + 1))
            sleep 5
        done
        kill "$holder" 2> /dev/null
        wait "$holder"
        rc=$(sed -n 's/^status //p' "$state/$id.log" | tail -n 1)
        [ -z "$rc" ] || break
        say "windows: the run $id ended without a status"
    done
    if [ "$rc" = 0 ]; then
        mark windows "run $id"
        return 0
    fi
    # shellcheck disable=SC2046 # the suites are words
    machine_failed windows "$id" "$state/$id.log" $(grep '^run: result: failed: ' "$state/$id.log" |
        tail -n 1 | sed 's/^run: result: failed: //' | grep . || echo runner)
}

# lenovo_run: run the native cases on the trial boot, every case or those of
# the record's `again lenovo` line, with native-deploy from this computer, and
# mark it done or file its failure.
lenovo_run()
{
    step=lenovo
    words=$(sed -n 's/^again lenovo //p' "$state/record")
    make build/tools/dev/native-deploy build/tools/dev/native-hostpeer > "$state/native.log" 2>&1 ||
        fail lenovo "native-deploy did not build: $(tail -n 3 "$state/native.log" | tr '\n' ' ')"
    [ "$(grep -c '^lenovo started ' "$state/record")" -lt 3 ] ||
        fail lenovo "three runs of the native cases ended without a status"
    id=phase$phase-$(echo "$sha" | cut -c 1-12)-lenovo-$(date -u '+%m%d-%H%M%S')
    note "lenovo started $id"
    # shellcheck disable=SC2086 # the cases are words
    if build/tools/dev/native-deploy run "$files/../tests/native/prepared" "$address" candidate \
        "$files/../rach2/kernel" "$repo/krffs/rach2_demo_ed25519" "$(setting lenovo target)" "$id" \
        "$state/$id" --hostpeer build/tools/dev/native-hostpeer --peer "$(setting lenovo peer)" \
        --echo-port "$(setting lenovo echo-port)" --network "$(setting lenovo network)" \
        --interface "$(setting lenovo interface)" --gateway "$(setting lenovo gateway)" \
        --far "$(setting lenovo far)" --empty "$(setting lenovo empty)" \
        --silent "$(setting lenovo silent)" --purpose phase_first_run -- $words \
        > "$state/$id.out" 2>&1; then
        mark lenovo "run $id"
        return 0
    fi
    log=''
    for f in "$state/$id"/collection-*.out; do
        [ ! -f "$f" ] || log=$f
    done
    # shellcheck disable=SC2046 # the cases are words
    machine_failed lenovo "$id" "${log:-$state/$id.out}" $({ [ -z "$log" ] ||
        sed -n 's/^native: \([^:]*\): \([a-z]*\).*/\1 \2/p' "$log"; } |
        awk '$1 != "result" && $2 != "pass" { print $1 }' | grep . || echo runner)
}

# again_machine: print the lines that make SHA's release run again only what
# failed on the Mac, Windows or the Lenovo in the last release, `again MACHINE
# WORD...`, followed by the lines of the runs that passed on the other machines,
# when that release filed the failure as an issue, the issue is closed, and
# every first-parent commit since its candidate closes that issue and no other.
# Print nothing otherwise.
again_machine()
{
    grep -qx "phase $phase" "$state/record" || return 0
    # shellcheck disable=SC2046 # the line is words
    set -- $(grep -E '^filed (mac|windows|lenovo) #' "$state/record" | tail -n 1)
    [ $# -ge 4 ] && grep -q "^failed $2: " "$state/record" || return 0
    m=$2 n=${3#\#}
    shift 3
    o=$(sed -n 's/^sha //p' "$state/record")
    git -C "$repo" fetch -q origin 2> /dev/null &&
        git -C "$repo" merge-base --is-ancestor "$o" "$sha" 2> /dev/null &&
        commits=$(git -C "$repo" rev-list --first-parent "$o..$sha" 2> /dev/null) &&
        [ -n "$commits" ] || return 0
    for c in $commits; do
        [ "$(git -C "$repo" log -1 --format=%B "$c" 2> /dev/null |
            sed -n 's/^Closes #\([0-9][0-9]*\)$/\1/p')" = "$n" ] || return 0
    done
    gh issue view "$n" --repo "$REPO" --json state,title --jq '"\(.state) \(.title)"' 2> /dev/null |
        grep -q '^CLOSED ' || return 0
    echo "again $m $*"
    grep -E '^(dispatched [0-9]+|run [0-9]+ success|rerun .*|(full|mac|windows) done .*)$' "$state/record" |
        grep -v "^$m done "
}

main()
{
    state=
    macrun=
    while getopts m:s: o; do
        case $o in
            m) macrun=$OPTARG ;;
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
    [ -n "$state" ] ||
        state=$(cd "$repo" && cd "$(git rev-parse --git-common-dir)/.." && pwd)/build/flow/release ||
        exit 1
    address=${RACH2_MACHINE_ADDRESS:-192.168.0.106}
    mac=${RACH2_MACHINE_MAC:-28:d2:44:7e:e0:af}
    serial=${RACH2_MACHINE_SERIAL:-S6P2NX0T381638Z}
    mkdir -p "$state"
    state=$(cd "$state" && pwd)
    files=$state/wt/$ARCH_DIR/images
    macdir=$repo/build/machines/mac
    tree=$macdir/tree site=$macdir/site runs=$macdir/runs
    if [ -n "$macrun" ]; then
        mac_run "$macrun"
        exit 0
    fi
    [ -f "$repo/tools/flow/machines" ] || {
        echo "release: refused: $repo/tools/flow/machines is missing" >&2
        exit 2
    }
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
        rerun='' kept=''
        [ -z "$old" ] || rerun=$(again)
        [ -z "$old" ] || [ -n "$rerun" ] || kept=$(again_machine)
        {
            printf 'phase %s\nsha %s\nchosen %s\n' "$phase" "$sha" "$(date -u '+%FT%TZ')"
            [ -z "$rerun" ] || echo "rerun $rerun"
            [ -z "$kept" ] || printf '%s\n' "$kept"
            [ -z "$old" ] || grep -q '^closed ' "$state/record" ||
                grep '^touched ' "$state/record" | tail -n 1
        } > "$state/record.new"
        [ -z "$old" ] || cp "$state/record" "$state/$old-$(date -u '+%m%d-%H%M%S')" ||
            later "the old record could not be kept"
        mv "$state/record.new" "$state/record"
        again=$(printf '%s\n' "$kept" | sed -n 's/^again //p')
        say "#$phase: the candidate is $sha${rerun:+, for a rerun of $rerun}${again:+, for a rerun of $again}"
        tests="Every suite now runs on it on both architectures from a clean build, on Ubuntu in WSL and on each machine that tools/flow/machines names, and the native cases run on the Lenovo's trial boot"
        # shellcheck disable=SC2086 # the rerun is words
        [ -z "$rerun" ] || tests=$(set -- $rerun
            echo "Every commit since $(echo "$2" | cut -c 1-8) fixes a failure of the release tests in https://github.com/$REPO/actions/runs/$1, so only the suites that failed run again on it, from a clean build: $(shift 2; plain "$@")")
        # shellcheck disable=SC2086 # the line is words
        [ -z "$again" ] || tests=$(set -- $again
            m=$1
            shift
            echo "Every commit since the last candidate fixes the failure of the release tests on $(who "$m"), so only these run again there: $(echo "$*" | sed 's/ /, /g'). The runs that passed on the other machines stand")
        gh issue comment "$phase" --repo "$REPO" --body "The release of this phase has started with commit $sha. $tests. The phase closes when that commit has passed them and runs on the Lenovo." > /dev/null ||
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

    if has mac && ! done_ mac && ! grep -q '^mac started ' "$state/record"; then
        mac_start
    fi
    done_ full || full
    if has windows && ! done_ windows; then
        windows
    fi
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
    if has mac && ! done_ mac; then
        mac_wait
    fi

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

    if has lenovo && ! done_ lenovo; then
        where || fail lenovo "the machine answered as another machine"
        [ -n "$now" ] || later "the machine does not answer"
        [ "$now" = "system candidate" ] || fail lenovo "the machine runs $now, not the trial boot"
        lenovo_run
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
        mark accept "metal-accept: PASS; committed"
    fi

    run=$(sed -n 's/^run \([0-9]*\) success$/\1/p' "$state/record" | tail -n 1)
    rerun=$(sed -n 's/^rerun //p' "$state/record")
    if ! done_ record; then
        step=record
        where || fail record "the machine answered as another machine"
        [ -n "$now" ] || later "the machine does not answer"
        [ "$now" = "system candidate" ] ||
            fail record "the machine runs $now, not the committed candidate"
        want=$(printf 'commit %s\nphase %s\nfull %s\n' "$sha" "$phase" "$run"
            [ -z "$rerun" ] || echo "rerun of ${rerun%% *}"
            printf 'root %s' "$(digest images/os-real-gnu.ext2)")
        wrote=$(printf '%s\n' "$want" | lenovo '{ mkdir -p /etc/rach2 && cat > /etc/rach2/release.new &&
            sync /etc/rach2/release.new && mv /etc/rach2/release.new /etc/rach2/release &&
            sync /etc/rach2/release /etc/rach2 /etc; } 2>&1; echo "status $?"') ||
            later "the machine does not answer"
        case $(printf '%s\n' "$wrote" | tail -n 1) in
            'status 0') ;;
            status*) fail record "writing /etc/rach2/release failed: $(printf '%s' "$wrote" | tr '\n' ' ')" ;;
            *) later "the machine does not answer" ;;
        esac
        have=$(lenovo 'cat /etc/rach2/release 2> /dev/null; echo answered') &&
            [ "$(printf '%s\n' "$have" | tail -n 1)" = answered ] ||
            later "the machine does not answer"
        [ "$(printf '%s\n' "$have" | sed '$d')" = "$want" ] ||
            fail record "/etc/rach2/release on the machine does not name $sha: $(printf '%s\n' "$have" | sed '$d' | tr '\n' ' ')"
        mark record "/etc/rach2/release names $sha"
    fi

    if ! grep -q '^reported ' "$state/record"; then
        tests="Every suite passed on both architectures in the run https://github.com/$REPO/actions/runs/$run."
        # shellcheck disable=SC2086 # the rerun is words
        [ -z "$rerun" ] || tests=$(set -- $rerun
            echo "The run https://github.com/$REPO/actions/runs/$run tested again only the suites that failed in https://github.com/$REPO/actions/runs/$1 at commit $(echo "$2" | cut -c 1-8), $(shift 2; plain "$@"), and they passed. Every commit since $(echo "$2" | cut -c 1-8) fixes one of those failures, and the earlier run passed every other suite it ran.")
        on="the one machine that the commit's \`tools/ci/site\` selects"
        if machine=$(sh "$repo/tools/ci/site.sh" "$state/wt/tools/ci/site" 2> /dev/null); then
            on="$on, whose runners have the label \`$(printf '%s\n' "$machine" | sed -n 's/^runner=//p')\`, with QEMU's accelerator \`$(printf '%s\n' "$machine" | sed -n 's/^accelerator=//p')\`"
        fi
        others=$(grep -E '^(mac|windows|lenovo) done ' "$state/record" | while read -r m _ _ _ id; do
            # shellcheck disable=SC2016 # the backquotes are Markdown
            printf '%s, in the run `%s`; ' "$(who "$m")" "$id"
        done | sed 's/; $//')
        again=$(sed -n 's/^again //p' "$state/record")
        # shellcheck disable=SC2086 # the line is words
        [ -z "$again" ] || again=" Only $(set -- $again; shift; echo "$*" | sed 's/ /, /g') ran again on $(who "${again%% *}"), since every commit since the last candidate fixes their failure there."
        [ -z "$others" ] || others="
* The release tests passed as well on $others.$again"
        gh issue comment "$phase" --repo "$REPO" --body "Commit $sha is released: the Lenovo runs it as its default system.

* $tests
* The release tests ran on $on.$others
* The commit was built in $COPY on the testing host with \`ARCH=x86_64 make clean && ARCH=x86_64 make machine GNU_AGENTS=all\`.
* The SHA-256 digests are $(digest rach2/kernel) for the kernel, $(digest rach2-recovery/kernel) for the recovery kernel, $(digest "$RECOVERY_IMAGE") for the recovery image and $(digest images/os-real-gnu.ext2) for the GNU root.
* The release updated the recovery partition and installed the kernel and the root from the recovery system. On the trial boot, machine_identity.sh and metal_accept.sh passed; \`bootctl commit\` then made the new system the saved default, and /etc/rach2/release on the machine names $sha." > /dev/null ||
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
                gh issue comment "$phase" --repo "$REPO" --body "This phase stays In progress, since commit $sha is no longer its finished work: the board reports \`$why\`. The Lenovo runs $sha; the next release takes the rest." > /dev/null ||
                    later "the comment on #$phase failed"
                void "$why" ;;
        esac
        note "closing $(date -u '+%FT%TZ')"
        gh issue close "$phase" --repo "$REPO" > /dev/null || later "#$phase could not be closed"
    fi
    sh "$repo/tools/flow/board.sh" 'done' > /dev/null || later "board.sh done failed"
    sh "$repo/tools/flow/board.sh" archive "$phase" > /dev/null || later "board.sh archive $phase failed"
    sh "$repo/tools/flow/notify.sh" "rach2: phase #$phase is finished and installed on the Lenovo, which now runs $(echo "$sha" | cut -c 1-8)" ||
        say "the push to the owner's phone failed"
    note "closed $(date -u '+%FT%TZ')"
    say "#$phase is released and closed"
    old=$phase-$(echo "$sha" | cut -c 1-8)-$(date -u '+%m%d-%H%M%S')
    mv "$state/record" "$state/$old"
    if ! cp "$files/release.sha256" "$state/$old.sha256"; then
        say "the digests could not be copied, so the worktree $state/wt stays"
    elif ! { cd "$repo" && git worktree remove --force "$state/wt" && git worktree prune; }; then
        say "the worktree $state/wt could not be removed"
    fi
}

main "$@"
