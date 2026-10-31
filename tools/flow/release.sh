#!/bin/sh
#
# release.sh: release milestone M at commit SHA: test SHA with a full run on
# each machine, install it on the Lenovo, move master to it and close M.
#
# Usage: release.sh [-r] [-s STATE] M SHA
#
# M is the number of a GitHub milestone, whose description names its branch
# as `board.sh milestone M` reads it, and SHA a commit already pushed to
# that branch that contains origin/master, so that master can move to it by
# fast-forward. The manager starts the script detached in the tmux session
# `rach2-release`, and starts it again with the same arguments when it ended
# without finishing; it then resumes from its record. It runs on the owner's
# computer, which reaches the testing host as `ssh testing` and the Lenovo on
# its network. A release has these steps, each of which the record below
# lists once it is done:
#
# * full: `full.yml` of M's branch runs every suite on both architectures
#   from clean at SHA, on a runner of the one machine that SHA's
#   `tools/ci/site` selects. The run is found by its title, `Release tests
#   of SHORT for milestone M` with SHORT the first eight digits of SHA, and a
#   start after the record was made, so a release started again does not
#   start a second. A run that fails gives the failures of its artifact
#   `failures`, `ARCH:ITEM` words, or `runner` when it has none. A run that
#   is cancelled or does not finish is started again, at most three runs in
#   all.
# * The other machines: tools/flow/machines names them and gives each run's
#   settings. Each runs every case of both architectures but those of the
#   style suite, which the full run checks once, with the selection that
#   `etude run -N` allocates in the same copy just before, on the in-house
#   build that the copy keeps between releases. The Mac's run, mac, is `etude
#   run routine-sequence phase_first_run`, started with the full run,
#   detached as `release.sh -m ID M SHA`, in the copy and the admission site
#   under build/machines/mac of this clone. Windows' run, windows, is on the
#   same testing host, in the copy the machines file names, which a bundle of
#   M's branch updates to SHA, and has two parts. `etude run routine-prepare
#   phase_first_run` builds its preparation from the start of the full run,
#   under a claim in WSL's admission, `etude-launch build` with Windows'
#   JOBS, of the memory and the block of ports that WSL's tests then leave to
#   it. Once the full run has ended and the preparation has passed, `etude
#   run routine-collect phase_first_run` runs its cases while a claim of the
#   whole host in WSL's admission, `etude-launch alone`, keeps WSL's tests
#   from starting; WSL refusing either claim ends the release. A release
#   that exits or is stopped stops Windows' preparation and ends both claims.
#   The Lenovo's run, lenovo, is the native cases of tests/native on the
#   trial boot, which `native-deploy run` starts from here with the payload
#   the build makes, after the acceptance. A run that ends without a status,
#   in Windows' preparation or its cases, runs again, at most three runs in
#   all.
# * A failed run adds a line `failure MACHINE RUN WORD...` to the record, the
#   WORDs being the failed `ARCH:SUITE`s or native cases, or `runner` when
#   the run named none, and the release goes on with the other machines'
#   runs. Once the runs of WSL, the Mac and Windows have ended, a failure of
#   any of them ends the release before the install, and a failure of the
#   Lenovo's ends it with the trial uncommitted. The last line printed is
#   then `release: failed: MACHINE run RUN: WORD...; ...`.
# * -r: the new record keeps the passes of WSL, the Mac and Windows that the
#   last record of M has, and runs again on each machine only what failed
#   there, at SHA: the record says `again MACHINE WORD...`. A
#   machine whose failures are not all `ARCH:SUITE`s of suites that SHA's
#   tests/registry has for ARCH, or native cases, runs again what its own
#   `again` line in that record named, or everything when it had none, and a
#   machine that has neither a pass nor a failure there runs everything. The
#   native cases run on every install, so the Lenovo's pass is not kept.
#   Without -r everything runs.
# * build: SHA is built from clean for the Lenovo on the testing host, in
#   the copy `build/worktrees/release` of the owner's, /home/testing/rach2, with
#   `ARCH=x86_64 make clean` and `make machine GNU_AGENTS=all`, whose GNU
#   root has every package of the agents layer, and `make native-assets` when
#   the machines file names the Lenovo, and the kernel, the recovery kernel
#   and image, the GNU root, compressed, and the native cases' payload are
#   fetched with their SHA-256 into a worktree of SHA, STATE/wt. The build
#   starts while the Mac's run goes on, once the runs on the testing host
#   have passed and the Lenovo has answered as itself and, before the
#   release has touched it, as running its system with no trial armed, so
#   that a Lenovo that does not answer holds up no machine's run.
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
#   boot, before the native cases, since it reads the boot's records out of
#   /dev/bootlog, a ring that their messages overwrite.
# * commit: once the acceptance and the native cases have passed, `bootctl
#   commit` makes the candidate the saved default, and `bootctl fallback`
#   copies it into the fallback slot, so that a later trial, which overwrites
#   the candidate, falls back to this release and not to an older kernel.
# * record: /etc/rach2/release on the machine, which runs the committed
#   candidate, is written with SHA, the milestone, the full run and the
#   root's digest, flushed to the disk with the directories that hold it, and
#   read back. A flush that fails fails the step, since the read-back may
#   come from the buffer cache.
# * close: the record says `closing`; SHA is pushed to master by
#   fast-forward, `git push origin SHA:refs/heads/master`, milestone M is
#   closed, its cards are archived with `board.sh archive M`, and the last
#   line printed is `release: passed SHA`. A start of the script that finds
#   a record with `closing` that did not end finishes that release first,
#   whatever M and SHA it was given.
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
# holds `record`, the record of the release under way, one line per fact:
# `milestone M`, `sha SHA`, `branch NAME`, `chosen TIME`, `again MACHINE
# WORD...` when only those run again on MACHINE, `dispatched TIME`, `run ID
# RESULT` for a full run, `MACHINE started RUN` for a run on another machine,
# `failure MACHINE RUN WORD...`, `card STEP N` for the card of each step
# on the board, and `card release N` for the release's, `STEP done TIME
# DETAIL`, `touched TIME`
# before the first change to the machine, `install started TIME`, `closing
# TIME`, and last `failed STEP: WHY` or `closed TIME`; `log`, everything the
# script prints; `wt`, the worktree; and the records of releases that ended,
# as `M-SHA-TIME`, with the build's digests beside each closed one as
# `M-SHA-TIME.sha256`. A release that closes its milestone copies the digests
# out of `wt` and then removes `wt`, with the build's products in it, last. A
# record that ended, or that belongs to another milestone or commit, is copied
# aside when the script starts on new arguments, and the new record then
# replaces it in one rename. The new record keeps the old one's `touched`
# line unless the old release closed, since the machine may still run what
# that release left, and its `card` lines when it is of the same milestone
# and did not close, so that the steps keep their cards. A done step is skipped when the script runs again, a
# transfer whose files still hash right on the machine is kept, and a step
# that writes the machine first asks it what it runs and which disk has the
# serial. A reboot counts as done when a boot younger than the request
# answers, by /proc/uptime. One release runs at a time, under a lock on
# STATE/lock.
#
# The exit status is 0 when the milestone was released; 1 when a step or a
# run failed, as the record's last line says; 2 when the arguments are
# refused, such as a SHA that is not on M's branch; and 75 when the release
# could not start or go on for a reason that passes, such as an unreachable
# host or a busy build copy, and the manager starts it again later.

REPO=toksaitov/rach2
OWNER=/home/testing/rach2
COPY=$OWNER/build/worktrees/release
ARCH_DIR=build/x86_64
RECOVERY_IMAGE=images/rach2-recovery-with-key.krffs
WSL='MSYS_NO_PATHCONV=1 wsl.exe -- bash -s'
USAGE='usage: release.sh [-r] [-s STATE] M SHA'

# say TEXT: print TEXT and append it to the log, with the time in UTC.
say()
{
    echo "$(date -u '+%F %T') release: $*" | tee -a "$state/log"
}

# finish TEXT: print the release's last line, `release: TEXT`, and log it.
finish()
{
    echo "release: $*"
    echo "$(date -u '+%F %T') release: $*" >> "$state/log"
}

# note LINE: append LINE to the record, and report what it says of a step
# to that step's card.
note()
{
    echo "$*" >> "$state/record"
    line=$*
    case $line in
        'dispatched '*) report full progress "The run on Ubuntu in WSL started." ;;
        'mac started '*|'windows started '*) report "${line%% *}" progress "The run ${line##* } started." ;;
        'lenovo started '*) report accept progress "The native cases' run ${line##* } started." ;;
        'build done '*) report trial progress "The build is done: $(echo "$line" | cut -d ' ' -f 4-)." ;;
        'full done '*|'mac done '*|'windows done '*|'trial done '*)
            report "${line%% *}" pass "Passed: $(echo "$line" | cut -d ' ' -f 4-)." ;;
        'accept done '*) report accept progress "Acceptance passed." ;;
        'commit done '*) report accept pass "Passed; the trial boot is committed and copied to the fallback slot." ;;
        'failure '*)
            m=$(echo "$line" | cut -d ' ' -f 2)
            [ "$m" != lenovo ] || m=accept
            report "$m" fail "The run $(echo "$line" | cut -d ' ' -f 3) failed: $(echo "$line" | cut -d ' ' -f 4-)." ;;
        'failed tests: '*) ;;
        'failed '*)
            m=$(echo "$line" | sed 's/^failed \([a-z]*\):.*/\1/')
            case $m in
                build|recovery|transfer|install|trial) m=trial ;;
                lenovo|accept|commit|record) m=accept ;;
                full|mac|windows) ;;
                *) m=release ;;
            esac
            report "$m" fail "Failed: ${line#failed }." ;;
        'closing '*) report close progress "Moving master to $sha." ;;
    esac
}

# report STEP STATE TEXT: report STEP to its card, the record's `card STEP N`,
# as `board.sh step N STATE TEXT` does. A card that cannot be reported is
# logged and does not stop the release.
report()
{
    n=$(sed -n "s/^card $1 //p" "$state/record" | tail -n 1)
    [ -n "$n" ] || return 0
    sh "$repo/tools/flow/board.sh" step "$n" "$2" "$3" > /dev/null 2>&1 ||
        echo "$(date -u '+%F %T') release: board: #$n: the $1 step could not be reported" >> "$state/log"
}

# cards: file on the board, in the milestone, the release's card and a card
# under it for each of its steps that the record has no card for, and pass
# each step the record has done.
cards()
{
    board=$repo/tools/flow/board.sh
    if ! grep -q '^card release ' "$state/record"; then
        n=$(sh "$board" card "$milestone" - \
            "Release milestone $milestone on every machine and move master to it" 2>> "$state/log")
        [ -n "$n" ] || { say "board: the release's card could not be filed"; return 0; }
        note "card release $n"
    fi
    report release progress "Releasing $sha."
    parent=$(sed -n 's/^card release //p' "$state/record" | tail -n 1)
    for s in full mac windows trial accept close; do
        case $s in
            full) t="Run every suite on Ubuntu in WSL" ;;
            mac) has mac || continue; t="Run every suite but style on the Mac" ;;
            windows) has windows || continue; t="Run every suite but style on Windows" ;;
            trial) t="Install the release on the Lenovo and start it on a trial boot" ;;
            accept) t="Run the Lenovo's native cases and acceptance on the trial boot" ;;
            close) t="Move master to the release and close the milestone" ;;
        esac
        grep -q "^card $s " "$state/record" && continue
        n=$(sh "$board" card "$milestone" "$parent" "$t" 2>> "$state/log")
        [ -n "$n" ] || { say "board: the card \"$t\" could not be filed"; continue; }
        note "card $s $n"
        ! done_ "$s" || report "$s" pass "Passed in an earlier run of this release."
    done
}

# done_ STEP: succeed when the record says STEP is done.
done_()
{
    grep -q "^$1 done " "$state/record"
}

# failed_ MACHINE: succeed when the record has a failure of MACHINE's run.
failed_()
{
    grep -q "^failure $1 " "$state/record"
}

# mark STEP DETAIL: record STEP as done now.
mark()
{
    note "$1 done $(date -u '+%FT%TZ') $2"
    say "$1: done: $2"
}

# fail STEP WHY: record that STEP failed, and exit 1.
fail()
{
    note "failed $1: $2"
    finish "failed: $1: $2"
    exit 1
}

# later WHY: say why the release cannot go on now, and exit 75.
later()
{
    say "waiting: $1"
    exit 75
}

# refuse WHY: refuse the arguments for WHY, and exit 2.
refuse()
{
    echo "release: refused: $1" >&2
    exit 2
}

# failure MACHINE RUN WORD...: record that the run RUN on MACHINE failed the
# WORDs, once.
failure()
{
    grep -q "^failure $1 $2 " "$state/record" || note "failure $*"
    m=$1 id=$2
    shift 2
    say "$m: run $id failed: $*"
}

# tests_failed: end the release on the failures of the record.
tests_failed()
{
    what=$(sed -n 's/^failure \([a-z]*\) \([^ ]*\) /\1 run \2: /p' "$state/record" |
        awk '{ printf "%s%s", (NR > 1 ? "; " : ""), $0 }')
    note "failed tests: $what"
    finish "failed: $what"
    exit 1
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

# plain ARCH:SUITE...: print the suites as `SUITE on ARCH, ...`.
plain()
{
    printf '%s\n' "$@" | awk -F: '{ printf "%s%s on %s", (NR > 1 ? ", " : ""), $2, $1 }'
}

# suites WORD...: succeed when each WORD is `ARCH:SUITE` of a suite that SHA's
# tests/registry has for ARCH.
suites()
{
    registry=$(git -C "$repo" show "$sha:tests/registry" 2> /dev/null) || return 1
    for w in "$@"; do
        case $w in
            i686:?*|x86_64:?*) ;;
            *) return 1 ;;
        esac
        printf '%s\n' "$registry" | awk -F'|' -v a="${w%%:*}" -v s="${w#*:}" '
            /^[a-z]/ && $1 == s && ($6 == "" || index("," $6 ",", "," a ",")) { found = 1 }
            END { exit !found }' || return 1
    done
}

# again: print the lines that make the new record of -r run again only what
# failed in the last record, as the header says: `again MACHINE WORD...` for
# each machine whose failures can run alone, and the passes the new record
# keeps.
again()
{
    for m in full mac windows lenovo; do
        # shellcheck disable=SC2046 # the line is words
        set -- $(grep "^failure $m " "$state/record" | tail -n 1)
        if [ $# -ge 4 ]; then
            shift 3
            # A run that ended without naming failed suites runs again what
            # the machine's earlier run did.
            case $m in
                lenovo)
                    case " $* " in
                        *" runner "*) grep "^again $m " "$state/record" | tail -n 1; continue ;;
                    esac ;;
                *) suites "$@" || { grep "^again $m " "$state/record" | tail -n 1; continue; } ;;
            esac
            echo "again $m $*"
        elif [ "$m" != lenovo ] && done_ "$m"; then
            grep "^$m done " "$state/record" | tail -n 1
            [ "$m" != full ] || grep -E '^run [0-9]+ success$' "$state/record" | tail -n 1
        fi
    done
}

# full_failed ID: record the failures of the full run ID, as its artifact
# `failures` gives them.
full_failed()
{
    rm -rf "$state/full-$1"
    words=
    if gh run download "$1" --repo "$REPO" --name failures --dir "$state/full-$1" > /dev/null 2>&1; then
        words=$(grep . "$state/full-$1/failures" 2> /dev/null | tr '\n' ' ' | sed 's/ $//')
    fi
    # shellcheck disable=SC2086 # the failures are words
    failure full "$1" ${words:-runner}
}

# full: find or start the full run of SHA, or of the suites of the record's
# `again full` line, and wait for it to end, first applying a result the
# record has and has not acted on.
full()
{
    only=$(sed -n 's/^again full //p' "$state/record")
    # shellcheck disable=SC2046 # the result is two words
    set -- $(grep -E '^run [0-9]+ (success|failure)$' "$state/record" | tail -n 1)
    case ${3-} in
        success) mark full "run $2${only:+, of $only only}"; return 0 ;;
        failure) full_failed "$2"; return 0 ;;
    esac
    title="Release tests of $(echo "$sha" | cut -c 1-8) for milestone $milestone"
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
                mac_told
                sleep 60
                continue
            fi
            note "run $1 $3"
            case $3 in
                success) mark full "run $1${only:+, of $only only}"; return 0 ;;
                failure) full_failed "$1"; return 0 ;;
            esac
            say "full: run $1 ended $3"
            continue
        fi
        last=$(sed -n 's/^dispatched //p' "$state/record" | tail -n 1)
        if [ -n "$last" ] && [ $(($(date +%s) - last)) -lt 600 ]; then
            sleep 30
            continue
        fi
        if [ "$(grep -c '^dispatched ' "$state/record")" -ge 3 ]; then
            failure full none runner
            return 0
        fi
        note "dispatched $(date +%s)"
        set -- -f sha="$sha" -f title="$title"
        [ -z "$only" ] || set -- "$@" -f suites="$only"
        gh workflow run full.yml --repo "$REPO" --ref "$branch" "$@" ||
            later "the full run could not be started"
        say "full: started a run of $sha${only:+ for $only only}"
        sleep 30
    done
}

# build: build SHA for the Lenovo on the testing host and fetch the results.
build()
{
    git -C "$repo" fetch -q origin || later "the fetch failed"
    git -C "$repo" merge-base --is-ancestor refs/remotes/origin/master "$sha" ||
        fail build "$sha does not contain origin/master, so master cannot move to it"
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
        # The archive's own prepared.json lacks the archive's digest, which
        # the preparation adds after it makes the archive; native-deploy
        # needs both it and the digest file.
        RACH2_TESTING_COPY=$COPY sh tools/dev/testing.sh get "$ARCH_DIR/$payload.sha256" \
            "$ARCH_DIR/tests/native/prepared/prepared.json" >> "$state/build.log" 2>&1 ||
            later "the build's results could not be fetched"
        if [ "$(cat "$files/../$payload.sha256")" != "$(digest "$payload")" ] ||
            ! grep -q "\"archive_sha256\":\"$(digest "$payload")\"" \
                "$files/../tests/native/prepared/prepared.json"; then
            fail build "the native cases' payload has no matching digest"
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
        status=$(lenovo 'cat /dev/shm/install.status 2> /dev/null; true') ||
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

# selection MACHINE: print the ARCH:SUITE words of MACHINE's run, those of the
# record's `again MACHINE` line or every suite of both architectures that SHA's
# tests/registry has, less the style suite, which the full run checks once for
# the release.
selection()
{
    words=$(sed -n "s/^again $1 //p" "$state/record")
    [ -n "$words" ] || words=$(git -C "$repo" show "$sha:tests/registry" | awk -F'|' '
        /^[a-z]/ { for (i = 0; i < 2; i++) { a = i ? "x86_64" : "i686"
            if ($6 == "" || index("," $6 ",", "," a ",")) print a ":" $1 } }')
    # shellcheck disable=SC2086 # the suites are words
    printf '%s\n' $words | grep -v ':style$' | tr '\n' ' '
}

# mac_run ID: make the run ID of the release tests on the Mac, of the words
# that `selection mac` prints, in the copy TREE with
# the admission site SITE, with its output in RUNS/ID.console and its status
# appended to RUNS/ID.times as `rc=N`.
mac_run()
{
    id=$1
    words=$(selection mac)
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
                -J "$runs/$id.selection.json" $words &&
            ETUDE_PHASE_SELECTION_EXPORT=$runs/$id.selection.json \
                "$site/bin/etude" run routine-sequence phase_first_run "$tree" "$runs/$id" both \
                "$id" "$site/etude-site" "$site/bin/etude" "$accel" $words
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
# `release.sh -m ID M SHA`, after stopping a run of an earlier record.
mac_start()
{
    [ -d "$tree" ] && [ -f "$site/etude-site" ] ||
        fail mac "the Mac has no copy at $tree or no admission site at $site"
    mac_stop
    if [ -z "$(selection mac)" ]; then
        mark mac "only style failed, which the full run checks"
        return 0
    fi
    git -C "$repo" fetch -q origin || later "the fetch failed"
    id=milestone$milestone-$(echo "$sha" | cut -c 1-12)-mac-$(date -u '+%m%d-%H%M%S')
    mkdir -p "$runs/$id"
    note "mac started $id"
    nohup perl -e 'setpgrp(0, 0); exec @ARGV' sh "$repo/tools/flow/release.sh" -s "$state" \
        -m "$id" "$milestone" "$sha" > /dev/null 2>&1 < /dev/null 9>&- &
    echo "$! $id" > "$macdir/run"
    say "mac: started the run $id"
}

# mac_told: say once, while the release waits for another machine, that the
# Mac's run of the record has failed, before mac_wait records it.
mac_told()
{
    id=$(sed -n 's/^mac started //p' "$state/record" | tail -n 1)
    [ -n "$id" ] && [ ! -e "$runs/$id.told" ] || return 0
    rc=$(sed -n 's/^rc=//p' "$runs/$id.times" 2> /dev/null)
    [ -n "$rc" ] && [ "$rc" != 0 ] || return 0
    say "mac: the run $id failed: $(grep '^run: result: failed: ' "$runs/$id.console" |
        tail -n 1 | sed 's/^run: result: failed: //')"
    : > "$runs/$id.told"
}

# mac_wait: wait for the Mac's run of the record to end, start it again when
# it ended without a status, three runs in all, and mark it done or record its
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
            if [ "$(grep -c '^mac started ' "$state/record")" -ge 3 ]; then
                failure mac "$id" runner
                return 0
            fi
            say "mac: the run $id ended without a status; starting another"
            mac_start
            continue
        fi
        say "mac: the run $id runs"
        sleep 60
    done
    rm -f "$macdir/run"
    if [ "$rc" = 0 ]; then
        mark mac "run $id"
        return 0
    fi
    # shellcheck disable=SC2046 # the suites are words
    failure mac "$id" $(grep '^run: result: failed: ' "$runs/$id.console" |
        tail -n 1 | sed 's/^run: result: failed: //' | grep . || echo runner)
}

# wsl COMMAND: run COMMAND in Ubuntu in WSL on the testing host.
wsl()
{
    echo "$1" | ssh -o BatchMode=yes testing "$WSL"
}

# windows_settings: read Windows' settings from the machines file: its copy,
# output directory, admission site and accelerator, its job count, and the
# output directory as WSL sees it.
windows_settings()
{
    wtree=$(setting windows tree) wout=$(setting windows out) wsite=$(setting windows site)
    waccel=$(setting windows accel)
    wjobs=$(setting windows env | sed -n 's/^JOBS=//p')
    wjobs=${wjobs:-4}
    wheld=/mnt/$(echo "$wout" | cut -c 1 | tr '[:upper:]' '[:lower:]')$(echo "$wout" | cut -c 3-)
}

# windows_stop: stop Windows' preparation that runs in the background and end
# the hold of WSL's whole host and its beat, so that a release that exits or is
# stopped leaves none of them behind; nothing of Windows' run is left running
# between its preparation and its cases.
windows_stop()
{
    if [ -n "$wprep" ]; then
        kill "$wprep" 2> /dev/null
        wait "$wprep"
        wprep=
    fi
    if [ -n "$wbeat" ]; then
        kill "$wbeat" 2> /dev/null
        wbeat=
    fi
    if [ -n "$wholder" ]; then
        wsl "touch $wheld/$wid.end" > /dev/null 2>&1
        kill "$wholder" 2> /dev/null
        wholder=
    fi
}

# beat ID: touch ID.alive in WSL once a minute while the release runs, the
# minute passing in WSL. A hold in WSL's admission ends when that file is ten
# minutes old, so that it lasts as long as the step it holds for and ends soon
# after a release that is gone. Run in the background, and kill it when the
# step ends.
beat()
{
    while kill -0 $$ 2> /dev/null; do
        wsl "touch $wheld/$1.alive; sleep 60" > /dev/null 2>&1 || sleep 60
    done
}

# windows_prepare ID: build the preparation of Windows' run ID with `etude run
# routine-prepare`, its output in STATE/ID.prepare, while a claim in WSL's
# admission, `etude-launch build` with Windows' JOBS, holds the memory that the
# preparation's own claim on Windows takes, JOBS GiB and one more, and the
# block of ports that WSL's launcher gives each claim, so that WSL's tests
# leave both to Windows; the tooling machine forwards no port. The claim ends
# with the preparation, or as beat says once the release is gone, and a stop
# ends both. Run in the background.
# downloads: print the Windows shell's lines that copy into the tree, from
# the owner's clone in WSL, each pinned tarball of the GNU layer that the tree
# lacks, kept only when it hashes as pinned. The tooling machine takes the
# tree's tarballs with its source, so it downloads none of them.
downloads()
{
    cat << END
mkdir -p vendor/gnu/downloads
awk '\$1 !~ /^#/ && NF >= 4 { n = \$3; sub(/.*\//, "", n); print n, \$4 }' \
    vendor/gnu/sources.txt vendor/gnu/sources-extra.txt |
    while read -r n s; do
        f=vendor/gnu/downloads/\$n
        [ "\$(sha256sum "\$f" 2> /dev/null | cut -c 1-64)" != "\$s" ] || continue
        MSYS_NO_PATHCONV=1 wsl.exe -- cat "$OWNER/vendor/gnu/downloads/\$n" < /dev/null > "\$f.part" 2> /dev/null
        if [ "\$(sha256sum "\$f.part" | cut -c 1-64)" = "\$s" ]; then mv "\$f.part" "\$f"; else rm -f "\$f.part"; fi
    done
END
}

windows_prepare()
{
    id=$1 claimer='' session='' beater=''
    trap 'kill $session $claimer $beater 2> /dev/null; wsl "touch $wheld/$id.prepared" > /dev/null 2>&1; exit 143' TERM
    wsl "cd /tmp && JOBS=$wjobs exec /usr/local/libexec/rach2/etude-launch build env sh -c 'touch $wheld/$id.claimed $wheld/$id.alive
        while [ ! -e $wheld/$id.prepared ] && find $wheld/$id.alive -mmin -10 2> /dev/null | grep -q .; do sleep 15; done'" \
        > "$state/$id.claim" 2>&1 &
    claimer=$!
    until wsl "test -e $wheld/$id.claimed" 2> /dev/null; do
        # A claimer may touch the file and end between the two tests.
        if ! kill -0 "$claimer" 2> /dev/null; then
            wsl "test -e $wheld/$id.claimed" 2> /dev/null && break
            echo "claim refused: $(tail -n 3 "$state/$id.claim" | tr '\n' ' ')" > "$state/$id.prepare"
            return 1
        fi
        sleep 30 &
        wait $!
    done
    beat "$id" &
    beater=$!
    # shellcheck disable=SC2086 # the suites are words
    {
        echo "cd $wtree || { echo 'status 1'; exit; }"
        echo "git fetch -q $wout/release.bundle 'refs/remotes/origin/$branch' && git checkout -q -f --detach $sha || { echo 'status 1'; exit; }"
        downloads
        environment windows
        echo "ETUDE_SITE=$wsite make build/tools/etude/etude > $wout/$id.make.log 2>&1 || { tail -n 20 $wout/$id.make.log; echo 'status 1'; exit; }"
        echo "e=$wout/etude-$id.exe"
        echo "cp build/tools/etude/etude \"\$e\" || { echo 'status 1'; exit; }"
        echo "unset ETUDE_SITE ETUDE_ADMITTED ETUDE_PREPARATION_CLEAN"
        echo "mkdir -p $wout/$id $wout/tmp"
        echo "ETUDE_SITE=$wsite \"\$e\" run -N -t $wtree -a $warch -A $waccel -J $wout/$id.selection.json $wwords > $wout/$id.selection.log 2>&1 || { tail -n 20 $wout/$id.selection.log; echo 'status 1'; exit; }"
        echo "ETUDE_PHASE_SELECTION_EXPORT=$wout/$id.selection.json \"\$e\" run routine-prepare phase_first_run $wtree $wout/$id $warch $id $wsite \"\$e\" $waccel $wwords > $wout/$id.prepare.console 2>&1"
        # shellcheck disable=SC2016 # the remote shell expands these
        echo 'rc=$?'
        echo "tail -n 40 $wout/$id.prepare.console"
        # shellcheck disable=SC2016 # the remote shell expands these
        echo 'echo "status $rc"'
    } | ssh -o BatchMode=yes testing 'C:/msys64/usr/bin/bash.exe -l -s' > "$state/$id.prepare" 2>&1 &
    session=$!
    wait "$session"
    session=
    kill "$beater" 2> /dev/null
    wsl "touch $wheld/$id.prepared" > /dev/null 2>&1
    i=0
    while kill -0 "$claimer" 2> /dev/null && [ "$i" -lt 12 ]; do
        i=$((i + 1))
        sleep 5
    done
    kill "$claimer" 2> /dev/null
    wait "$claimer"
}

# windows_start: start Windows' run of the words that `selection windows`
# prints, its preparation in the background, which windows_stop stops when the
# release exits first. Mark the run done when only style failed, and record
# its failure when three runs have started; return 1 in both cases.
windows_start()
{
    windows_settings
    wwords=$(selection windows)
    if [ -z "$wwords" ]; then
        mark windows "only style failed, which the full run checks"
        return 1
    fi
    # Prepare only the architectures the words name, so that a rerun of one
    # architecture's suites does not build the other's collection.
    warch=both
    case " $wwords" in *" x86_64:"*) ;; *) warch=i686 ;; esac
    case " $wwords" in *" i686:"*) ;; *) warch=x86_64 ;; esac
    if [ "$(grep -c '^windows started ' "$state/record")" -ge 3 ]; then
        failure windows "$(sed -n 's/^windows started //p' "$state/record" | tail -n 1)" runner
        return 1
    fi
    wid=milestone$milestone-$(echo "$sha" | cut -c 1-12)-windows-$(date -u '+%m%d-%H%M%S')
    if ! git -C "$repo" fetch -q origin ||
        ! git -C "$repo" bundle create "$state/windows.bundle" "refs/remotes/origin/$branch" > /dev/null 2>&1; then
        later "the bundle for Windows could not be made"
    fi
    scp -q -o BatchMode=yes "$state/windows.bundle" "testing:$wout/release.bundle" ||
        later "the bundle could not be copied to the testing host"
    note "windows started $wid"
    trap windows_stop EXIT
    trap 'windows_stop; trap - EXIT TERM; kill -TERM $$' TERM
    trap 'windows_stop; trap - EXIT INT; kill -INT $$' INT
    trap 'windows_stop; trap - EXIT HUP; kill -HUP $$' HUP
    windows_prepare "$wid" > /dev/null 2>&1 9>&- &
    wprep=$!
    say "windows: preparing the run $wid"
}

# windows: finish Windows' run on the testing host, which windows_start began:
# once its preparation has passed, run its cases with `etude run
# routine-collect` while a claim of the whole host in WSL's admission keeps
# WSL's tests from starting, and mark it done or record its failure. A run
# whose preparation or cases end without a status runs again, three runs in
# all.
windows()
{
    step=windows
    windows_settings
    while :; do
        if [ -z "$wprep" ]; then
            windows_start || return 0
        fi
        say "windows: waiting for the preparation of $wid"
        wait "$wprep"
        wprep='' id=$wid
        grep -q '^claim refused: ' "$state/$id.prepare" 2> /dev/null &&
            fail windows "WSL's admission did not give the preparation's claim: $(sed -n 's/^claim refused: //p' "$state/$id.prepare")"
        rc=$(tr -d '\r' < "$state/$id.prepare" 2> /dev/null | sed -n 's/^status //p' | tail -n 1)
        if [ -z "$rc" ]; then
            say "windows: the preparation of $id ended without a status"
            continue
        fi
        if [ "$rc" != 0 ]; then
            failure windows "$id" runner
            return 0
        fi
        wsl "exec /usr/local/libexec/rach2/etude-launch alone sh -c 'touch $wheld/$id.held $wheld/$id.alive
            while [ ! -e $wheld/$id.end ] && find $wheld/$id.alive -mmin -10 2> /dev/null | grep -q .; do sleep 15; done'" \
            > "$state/$id.hold" 2>&1 9>&- &
        wholder=$!
        until wsl "test -e $wheld/$id.held" 2> /dev/null; do
            # A holder may touch the file and end between the two tests.
            if ! kill -0 "$wholder" 2> /dev/null; then
                wsl "test -e $wheld/$id.held" 2> /dev/null && break
                fail windows "WSL's admission did not give the whole host: $(tail -n 3 "$state/$id.hold" | tr '\n' ' ')"
            fi
            say "windows: prepared; waiting for WSL's tests to end"
            sleep 30
        done
        say "windows: the cases of the run $id start"
        beat "$id" 9>&- &
        wbeat=$!
        # shellcheck disable=SC2086 # the suites are words
        {
            echo "cd $wtree || { echo 'status 1'; exit; }"
            environment windows
            echo "MSYS_NO_PATHCONV=1 wsl.exe -u root -e sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' < /dev/null"
            echo "e=$wout/etude-$id.exe"
            echo "unset ETUDE_SITE ETUDE_ADMITTED ETUDE_PREPARATION_CLEAN"
            echo "ETUDE_PHASE_SELECTION_EXPORT=$wout/$id.selection.json \"\$e\" run routine-collect phase_first_run $wtree $wout/$id $warch $id $wsite \"\$e\" $waccel $wwords > $wout/$id.console 2>&1"
            # shellcheck disable=SC2016 # the remote shell expands these
            echo 'rc=$?'
            echo "tail -n 40 $wout/$id.console"
            # shellcheck disable=SC2016 # the remote shell expands these
            echo 'echo "status $rc"'
        } | ssh -o BatchMode=yes testing 'C:/msys64/usr/bin/bash.exe -l -s' > "$state/$id.log" 2>&1
        kill "$wbeat" 2> /dev/null
        wbeat=
        wsl "touch $wheld/$id.end" > /dev/null 2>&1
        i=0
        while kill -0 "$wholder" 2> /dev/null && [ "$i" -lt 12 ]; do
            i=$((i + 1))
            sleep 5
        done
        kill "$wholder" 2> /dev/null
        wait "$wholder"
        wholder=
        rc=$(tr -d '\r' < "$state/$id.log" | sed -n 's/^status //p' | tail -n 1)
        [ -z "$rc" ] || break
        say "windows: the run $id ended without a status"
    done
    if [ "$rc" = 0 ]; then
        mark windows "run $id"
        return 0
    fi
    # shellcheck disable=SC2046 # the suites are words
    failure windows "$id" $(tr -d '\r' < "$state/$id.log" | grep '^run: result: failed: ' |
        tail -n 1 | sed 's/^run: result: failed: //' | grep . || echo runner)
}

# lenovo_run: run the native cases on the trial boot, every case or those of
# the record's `again lenovo` line, with native-deploy from this computer, and
# mark it done or end the release on its failure.
lenovo_run()
{
    step=lenovo
    words=$(sed -n 's/^again lenovo //p' "$state/record")
    make build/tools/dev/native-deploy build/tools/dev/native-hostpeer > "$state/native.log" 2>&1 ||
        fail lenovo "native-deploy did not build: $(tail -n 3 "$state/native.log" | tr '\n' ' ')"
    [ "$(grep -c '^lenovo started ' "$state/record")" -lt 3 ] ||
        fail lenovo "three runs of the native cases ended without a status"
    target=$(setting lenovo target)
    staged=$target/p-$(digest tests/native/prepared.tar.gz | cut -c 1-16)
    if ! lenovo "test -f $staged/archive.sha256" > /dev/null 2>&1; then
        id=milestone$milestone-$(echo "$sha" | cut -c 1-12)-lenovo-stage-$(date -u '+%m%d-%H%M%S')
        build/tools/dev/native-deploy stage "$files/../tests/native/prepared" "$address" candidate \
            "$files/../rach2/kernel" "$repo/krffs/rach2_demo_ed25519" "$target" "$id" \
            "$state/$id" > "$state/$id.out" 2>&1 ||
            fail lenovo "the native cases' payload could not be staged: $(tail -n 3 "$state/$id.out" | tr '\n' ' ')"
        say "lenovo: staged $staged"
    fi
    id=milestone$milestone-$(echo "$sha" | cut -c 1-12)-lenovo-$(date -u '+%m%d-%H%M%S')
    note "lenovo started $id"
    # shellcheck disable=SC2086 # the cases are words
    if build/tools/dev/native-deploy run "$files/../tests/native/prepared" "$address" candidate \
        "$files/../rach2/kernel" "$repo/krffs/rach2_demo_ed25519" "$target" "$id" \
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
    failure lenovo "$id" $({ [ -z "$log" ] ||
        sed -n 's/^native: \([^:]*\): \([a-z]*\).*/\1 \2/p' "$log"; } |
        awk '$1 != "result" && $2 != "pass" { print $1 }' | grep . || echo runner)
}

# check: fetch, and refuse SHA unless milestone M is open, its description
# names its branch, and SHA is on that branch and contains origin/master; set
# `branch`.
check()
{
    git -C "$repo" fetch -q origin || later "the fetch failed"
    branch=$(sh "$repo/tools/flow/board.sh" milestone "$milestone")
    case $? in
        0) ;;
        3) refuse "milestone $milestone is not open or names no branch" ;;
        *) later "milestone $milestone cannot be read" ;;
    esac
    git -C "$repo" merge-base --is-ancestor "$sha" "refs/remotes/origin/$branch" 2> /dev/null ||
        refuse "$sha is not on $branch"
    git -C "$repo" merge-base --is-ancestor refs/remotes/origin/master "$sha" 2> /dev/null ||
        refuse "$sha does not contain origin/master"
}

# preflight: refuse a machine that answers as another machine, wait for one
# that does not answer, and, before the release has touched it, refuse one
# that does not run its system or has a trial armed.
preflight()
{
    where || fail preflight "the machine at $address is not the Lenovo"
    [ -n "$now" ] || later "the machine at $address does not answer"
    grep -q '^touched ' "$state/record" && return 0
    case $now in
        "system candidate"|"system fallback") ;;
        *) fail preflight "the machine runs $now before the release has touched it" ;;
    esac
    lenovo 'bootctl status' | grep -Eq '^bootctl: next_entry: (empty|absent)' ||
        fail preflight "a trial is armed on the machine"
}

# close: move master to SHA, close milestone M and archive its cards.
close()
{
    git -C "$repo" fetch -q origin || later "the fetch failed"
    if [ "$(git -C "$repo" rev-parse refs/remotes/origin/master)" != "$sha" ]; then
        git -C "$repo" push origin "$sha:refs/heads/master" ||
            later "the push of $sha to master failed"
    fi
    gh api -X PATCH "repos/$REPO/milestones/$milestone" -f state=closed > /dev/null ||
        later "milestone $milestone could not be closed"
    report close pass "Master is at $sha, and milestone $milestone is closed."
    report release pass "Released $sha on every machine."
    sh "$repo/tools/flow/board.sh" archive "$milestone" > /dev/null ||
        later "board.sh archive $milestone failed"
}

main()
{
    state=
    macrun=
    rerun=
    while getopts m:rs: o; do
        case $o in
            m) macrun=$OPTARG ;;
            r) rerun=1 ;;
            s) state=$OPTARG ;;
            *) echo "$USAGE" >&2; exit 2 ;;
        esac
    done
    shift $((OPTIND - 1))
    [ $# -eq 2 ] || { echo "$USAGE" >&2; exit 2; }
    milestone=$1 sha=$2
    case $milestone in
        ''|*[!0-9]*) echo "$USAGE" >&2; exit 2 ;;
    esac
    case $sha in
        *[!0-9a-f]*) refuse "'$sha' is not a commit's whole hash" ;;
    esac
    [ "${#sha}" -eq 40 ] || refuse "'$sha' is not a commit's whole hash"
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
    [ -f "$repo/tools/flow/machines" ] || refuse "$repo/tools/flow/machines is missing"
    exec 9>> "$state/lock"
    perl -MFcntl=:flock -e 'open(L, ">&=9") && flock(L, LOCK_EX | LOCK_NB) or exit 1' ||
        later "another release runs"
    chmod 600 "$repo/krffs/rach2_demo_ed25519"

    if [ -f "$state/record" ] && grep -q '^closing ' "$state/record" &&
        ! grep -q '^closed \|^failed ' "$state/record"; then
        milestone=$(sed -n 's/^milestone //p' "$state/record")
        sha=$(sed -n 's/^sha //p' "$state/record")
        rerun=
        say "milestone $milestone: its release of $sha began to close and has not finished; finishing it"
    fi
    old=
    if [ -f "$state/record" ] && { ! grep -qx "milestone $milestone" "$state/record" ||
        ! grep -qx "sha $sha" "$state/record" || grep -q '^failed \|^closed ' "$state/record"; }; then
        if grep -q '^install started ' "$state/record" &&
            ! grep -q '^install done ' "$state/record"; then
            say "the record has an installer that was started and whose success no release saw; check the machine's root by hand, then delete the record's \`install started\` line"
            exit 1
        fi
        old=$(sed -n 's/^milestone //p' "$state/record")-$(sed -n 's/^sha //p' "$state/record" | cut -c 1-8)
    fi
    if [ -n "$old" ] || [ ! -f "$state/record" ]; then
        if [ -n "$rerun" ] && { [ -z "$old" ] || ! grep -qx "milestone $milestone" "$state/record"; }; then
            refuse "-r: the record has no earlier release of milestone $milestone"
        fi
        check
        kept=''
        [ -z "$rerun" ] || kept=$(again)
        {
            printf 'milestone %s\nsha %s\nbranch %s\nchosen %s\n' "$milestone" "$sha" "$branch" \
                "$(date -u '+%FT%TZ')"
            [ -z "$kept" ] || printf '%s\n' "$kept"
            [ -z "$old" ] || ! grep -qx "milestone $milestone" "$state/record" ||
                grep -q '^closed ' "$state/record" || grep '^card ' "$state/record"
            [ -z "$old" ] || grep -q '^closed ' "$state/record" ||
                grep '^touched ' "$state/record" | tail -n 1
        } > "$state/record.new"
        [ -z "$old" ] || cp "$state/record" "$state/$old-$(date -u '+%m%d-%H%M%S')" ||
            later "the old record could not be kept"
        mv "$state/record.new" "$state/record"
        cards
        again=$(sed -n 's/^again //p' "$state/record" | awk '{ printf "%s%s", (NR > 1 ? "; " : ""), $0 }')
        say "milestone $milestone: releasing $sha${again:+, running again only $again}"
    fi
    sha=$(sed -n 's/^sha //p' "$state/record")
    branch=$(sed -n 's/^branch //p' "$state/record")
    chosen=$(sed -n 's/^chosen //p' "$state/record")

    if has mac && ! done_ mac && ! failed_ mac && ! grep -q '^mac started ' "$state/record"; then
        mac_start
    fi
    wprep='' wholder='' wbeat=''
    if has windows && ! done_ windows && ! failed_ windows; then
        windows_start
    fi
    done_ full || failed_ full || full
    if has windows && ! done_ windows && ! failed_ windows; then
        windows
    fi
    if ! grep -q '^failure ' "$state/record" && ! done_ accept; then
        preflight
    fi
    if ! grep -q '^failure ' "$state/record" && ! done_ build; then
        ssh -o BatchMode=yes -o ConnectTimeout=10 testing true 2> /dev/null ||
            later "the testing host does not answer"
        build
    fi
    if has mac && ! done_ mac && ! failed_ mac; then
        mac_wait
    fi
    ! grep -q '^failure ' "$state/record" || tests_failed
    cd "$state/wt" || fail build "no worktree"
    chmod 600 krffs/rach2_demo_ed25519

    if ! done_ install; then
        step=recovery
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

    # Acceptance reads the boot's own records out of /dev/bootlog, a ring
    # that the native cases' messages overwrite, so it runs before them.
    if ! done_ accept; then
        step=accept
        mkdir -p build/images
        make krffs > "$state/krffs.log" 2>&1 || later "the host KRFFS tools did not build"
        sh tests/system/accept/metal_accept.sh "$address" > "$state/accept.log" 2>&1 ||
            fail accept "metal_accept.sh failed: $(grep -i 'fail' "$state/accept.log" | tail -n 3 | tr '\n' ' ')"
        mark accept "metal-accept: PASS"
    fi

    if has lenovo && ! done_ lenovo; then
        where || fail lenovo "the machine answered as another machine"
        [ -n "$now" ] || later "the machine does not answer"
        [ "$now" = "system candidate" ] || fail lenovo "the machine runs $now, not the trial boot"
        failed_ lenovo || lenovo_run
        ! failed_ lenovo || tests_failed
    fi

    if ! done_ commit; then
        step=commit
        lenovo 'bootctl commit' > "$state/commit.log" 2>&1
        grep -q '^bootctl: committed:' "$state/commit.log" ||
            fail commit "bootctl commit: $(tr '\n' ' ' < "$state/commit.log")"
        lenovo 'bootctl fallback' > "$state/fallback.log" 2>&1
        grep -q '^bootctl: fallback: .* read back' "$state/fallback.log" ||
            fail commit "bootctl fallback: $(tr '\n' ' ' < "$state/fallback.log")"
        mark commit "committed and copied to the fallback slot"
    fi

    if ! done_ record; then
        step=record
        where || fail record "the machine answered as another machine"
        [ -n "$now" ] || later "the machine does not answer"
        [ "$now" = "system candidate" ] ||
            fail record "the machine runs $now, not the committed candidate"
        run=$(sed -n 's/^run \([0-9]*\) success$/\1/p' "$state/record" | tail -n 1)
        want=$(printf 'commit %s\nmilestone %s\nfull %s\nroot %s' "$sha" "$milestone" "$run" \
            "$(digest images/os-real-gnu.ext2)")
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

    grep -q '^closing ' "$state/record" || note "closing $(date -u '+%FT%TZ')"
    close
    note "closed $(date -u '+%FT%TZ')"
    old=$milestone-$(echo "$sha" | cut -c 1-8)-$(date -u '+%m%d-%H%M%S')
    mv "$state/record" "$state/$old"
    if ! cp "$files/release.sha256" "$state/$old.sha256"; then
        say "the digests could not be copied, so the worktree $state/wt stays"
    elif ! { cd "$repo" && git worktree remove --force "$state/wt" && git worktree prune; }; then
        say "the worktree $state/wt could not be removed"
    fi
    finish "passed $sha"
}

main "$@"
