#!/bin/sh
#
# release.sh: release milestone M at commit SHA: test SHA with a full run on
# each machine, install it on the Lenovo, move master to it and close M.
#
# Usage: release.sh [-r] [-o] [-c N]... [-s STATE] M SHA
#        release.sh -p [-o] [-s STATE] M SHA
#        release.sh -a MACHINE [-s STATE] M SHA
#
# M is the number of a GitHub milestone, whose description names its branch
# as `board.sh milestone M` reads it, and SHA a commit already pushed to
# that branch that contains origin/master, so that master can move to it by
# fast-forward. Each -c N names another open milestone whose branch SHA
# contains, released with M: the record says `also N`, and the close closes N
# and archives its cards after M's, so that milestones whose branches were
# joined at their end, or that share M's branch as a stack, take one
# release. The manager starts the script detached in the tmux session
# `rach2-release`, and starts it again with the same arguments when it ended
# without finishing; it then resumes from its record. It runs on the owner's
# computer, which reaches the testing host as `ssh testing` and the Lenovo on
# its network. A release has these steps,
# each of which the record below lists once it is done:
#
# * full: `full.yml` of M's branch runs every suite on both architectures
#   from clean at SHA, and then the i686 cases on the x86-64 kernel, the ia32
#   profile of tests/Readme.md, on a runner of the one machine that SHA's
#   `tools/ci/site` selects. The run is found by its title, `Release tests
#   of SHORT for milestone M` with SHORT the first eight digits of SHA, and a
#   start after the record was made, so a release started again does not
#   start a second. A run that fails gives the failures of its artifact
#   `failures`, `ARCH:ITEM` words, ARCH being i686, x86_64 or ia32, or
#   `runner` when it has none. A run that is cancelled or does not finish is
#   started again, at most three runs in all.
# * The other machines: tools/flow/machines names them and gives each run's
#   settings. Each runs every case of both architectures but those of the
#   style suite, which the full run checks once, and those of a suite none of
#   whose cases runs on the machine's host system, with the selection that
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
#   from starting; WSL refusing either claim ends the release. Each part runs
#   on Windows apart from the ssh session that starts it, as windows_detach
#   says, and each claim in WSL apart from its own, as claim_start says, so that
#   a dropped link ends neither and a claim lasts as long as its part: the
#   release polls the part each minute, over ssh with keepalives, notes each
#   lost link in its output, and says why a part ended without a status, with
#   the last lines of its console. A release that exits or is stopped stops
#   Windows' preparation and cases, and once winclean.sh has confirmed that
#   nothing of them runs, ends both claims, a claim that still waits for
#   admission included, as claim_stop says; a claim of a run that is not
#   confirmed stopped stays, and ends once the run's ID.alive has not grown
#   for ten minutes. Each run, on the Mac and in each Windows part, has a
#   directory of its own, build/tmp/ID in its copy,
#   ID ending in the time and a random suffix, which ETUDE_RUN_DIR names and
#   which holds its TMPDIR and etude's scratch. A failure first writes ID/KEPT,
#   with the reason and the evidence, as etude does when it keeps evidence; a
#   run that passed removes the directory unless it holds KEPT. A Windows run
#   that a stop ends is stopped there too, and its directory removed unless
#   kept, as winclean.sh says; a stopped Mac run's directory is removed the
#   same way. sweep.sh removes a kept one once its milestone's release
#   closed.
#   The Lenovo's run, lenovo, is the native cases of tests/native on the
#   trial boot, which `native-deploy run` starts from here with the payload
#   the build makes, after the acceptance. The trial boot's console session
#   is `term`, which holds the framebuffer that the cases of `console` open,
#   so the run stops it first; init then starts the kernel console's login in
#   its place, and ending that login once the run is over starts `term`
#   again. A run that ends without a status,
#   in Windows' preparation or its cases, runs again, at most three runs in
#   all. No Windows run, nor -p's preparation there, starts before
#   winclean.sh has confirmed that no part of an earlier one still runs in
#   Windows' copy; while that is not confirmed, the release waits, as exit
#   75 says.
# * A failed run adds a line `failure MACHINE RUN WORD...` to the record, the
#   WORDs being the failed `ARCH:SUITE`s or native cases, or `runner` when
#   the run named none, and the release goes on with the other machines'
#   runs. Once the runs of WSL, the Mac and Windows have ended, a failure of
#   any of them ends the release before the install, and a failure of the
#   Lenovo's ends it with the trial uncommitted. The last line printed is
#   then `release: failed: MACHINE run RUN: WORD...; ...`.
# * -r: the new record keeps the passes of WSL, the Mac and Windows that the
#   last record of M has, and runs again on each machine only what failed
#   there, at SHA: the record says `again MACHINE WORD...`. A machine whose
#   failures are not all `ARCH:SUITE`s of suites that SHA's tests/registry
#   has for ARCH, i686 for ia32, or native cases, runs again what its own
#   `again` line in that record named, or everything when it had none. A Mac
#   or Windows run that the record started last and that has no result there,
#   as a run of -a may not, is taken over: the new record keeps a Mac run that
#   has since passed and runs again only the suites of one that has failed,
#   and it waits for a run that still runs; one that no longer runs and left
#   no status runs again what its `again` line named. A machine that has
#   neither a run nor a result there runs everything. A run that -a started
#   at a commit of which SHA is an earlier commit is not kept, and its machine
#   runs again what its `again` line named; a SHA that a rebase onto master
#   made keeps it, as -r keeps any pass. The native cases run on every
#   install, so the Lenovo's pass is not kept. Without -r everything runs.
# * -p: prepare, which the manager runs at the end of a milestone or a stack,
#   before the release, to find what would stop the release at its setup. It
#   reads no record and writes none, and touches neither GitHub's runners nor
#   the Lenovo. Three parts run at once. wsl: in a copy on the testing host,
#   `build/worktrees/prepare` of the owner's, made new from a worktree of SHA
#   here, it builds both architectures as the full run does and then runs
#   `tests/run style host` on both. windows: Windows' preparation as the
#   release makes it, in its copy and under its claim in WSL's admission,
#   with `routine-prepare`. mac: the Mac's run as the release makes it, with
#   `routine-prepare` in place of `routine-sequence`, detached as `release.sh
#   -p -m ID M SHA`. Both select as the release does, from every suite. SHA
#   must be on M's branch, but need not contain master. A line `release:
#   prepare: MACHINE: passed` or `failed` reports each part, a failed one
#   followed by the lines of its output that say what failed, which
#   STATE/prepare/MACHINE.log keeps whole; the last line is `release:
#   prepare: passed SHA` or `release: prepare: failed: MACHINE...`. On every
#   exit it stops what still runs and removes the copy on the host, the
#   worktree, the runs' output on Windows and the Mac, and their run
#   directories unless kept, which sweep.sh removes later; the vendor
#   and GNU builds stay in each machine's step store. It takes the release's
#   lock and STATE/again.lock, and waits while a release's run on the Mac, or
#   a Windows run of -a, still runs.
# * -o: only WSL and the Lenovo: the release, or -p, leaves out the Mac and
#   Windows, whatever the machines file names, and files no cards for them.
#   The record says `only wsl lenovo`. Every later start of the same release,
#   -r among them, takes -o again; the owner decides when a release may skip
#   them.
# * -a MACHINE: while the release of M runs, or after it ended on a failure,
#   start the failed run of MACHINE, mac or windows, again at SHA, a commit of
#   M's branch that contains the record's SHA; it need not contain
#   origin/master, which a release of SHA with -r checks. MACHINE's
#   last run must have failed, as a failure in the record or as the status of
#   a Mac run that ended. The record says `MACHINE started ID` of the new run,
#   as for any run, so that the release under way waits for it as for its own
#   and records its pass or failure. When SHA is not the record's commit the
#   record first says `failure newer MACHINE ID SHA`: the release then ends
#   once its runs are over, before the build, and a release of SHA with -r
#   keeps the run's pass. The Mac's run starts as the release starts its own.
#   Windows' run, preparation and cases, runs detached as `release.sh -w ID
#   M SHA` under a hold.pl that leads a process group of its own, in a run
#   directory of its own that STATE/windows.run names, with
#   the claims in WSL that the release's own run takes; it writes its result
#   to the record itself. A release under way that has recorded Windows'
#   failure waits for it after the Mac's run; a release started on new
#   arguments that does not take it over stops it.
# * build: SHA is built from clean for the Lenovo on the testing host, in
#   the copy `build/worktrees/release` of the owner's, /home/testing/rach2, with
#   `ARCH=x86_64 make clean` and `make machine GNU_AGENTS=all`, whose GNU
#   root has every package of the agents layer and Doom's WAD, the host's
#   /etc/rach2/doom1.wad copied into the copy before the build; the build
#   fails without that file, or when the root's /demo/doom1.wad, read with
#   debugfs, differs from it. Then `make native-assets` runs when
#   the machines file names the Lenovo, and the kernel, the recovery kernel
#   and image, the GNU root, compressed, and the native cases' payload are
#   fetched with their SHA-256 into a worktree of SHA, STATE/wt. The build
#   starts while the Mac's run goes on, once the runs on the testing host
#   have passed and the Lenovo has answered as itself and, before the
#   release has touched it, as running its system with no trial armed, so
#   that a Lenovo that does not answer holds up no machine's run.
# * hold: while STATE/hold exists, the release does not first touch the
#   Lenovo; it says the file's line and exits 75, and is started again once a
#   person's check of a trial boot there has ended and the file is removed.
# * lock: each time it comes to the Lenovo, before the preflight and again
#   before its first step there, the release takes the Lenovo's lock,
#   `build/flow/lenovo` of the main working tree as the machine skill says,
#   with `release M` in its `issue`, or finds it its own, and exits 75 while
#   a task holds it. An exit 75 before the release has touched the Lenovo
#   removes the lock. It keeps the lock through a failure, which may leave a
#   trial armed, and through a release started again, and removes it once
#   the Lenovo runs the committed release and names SHA.
# * recovery: the Lenovo boots its recovery system from its recovery
#   partition; the recovery kernel and image on that partition are replaced
#   when they differ from the build's, keeping the partition's recovery key
#   and the archive of the machine's accounts, and the machine boots the
#   recovery system again, which is then the build's. The old kernel and
#   root stay as they were, so a recovery system that does not start leaves
#   a machine that a power cycle returns to them.
# * transfer: the kernel and the root go to the recovery system's /dev/shm,
#   the root decompressed on the way, and each is checked by SHA-256.
# * kernel and install: a build whose `rach2-install` has `--slots` moves a
#   disk with kernel slots smaller than the build's to them first, since the
#   build's bootctl refuses a kernel in a smaller slot; the move saves the
#   machine to recovery while it runs, so a reset returns there, and the run
#   again finishes it. Then `bootctl kernel` writes the kernel to the
#   candidate slot, and `rach2-install --console term --update` writes the
#   root, keeps the accounts and keys, sets term as its console and arms the
#   candidate for one trial boot; with `--i915`, which the script passes
#   when the build's installer has it, the root's lines also bind the Intel
#   graphics driver, `rach2.i915`, so that the native cases run on the GPU.
#   That installer is the one that runs, from the build's recovery system,
#   and the step fails unless every line of the menu on the boot partition
#   that boots the candidate or fallback slot then carries the word. The
#   installer runs detached on the machine, with its log and its status in
#   /dev/shm, so that a lost connection does not stop it; it is started once,
#   and a release started again finds it by its log and waits for it. An
#   install that a reboot interrupted, or that ended without success, is
#   never started again by this script: it ends the release for a person.
#   While the record has its `install started` line and no `install done`,
#   every later start of the script stops before it changes anything; the
#   person checks the machine's root by hand and then deletes that line.
# * trial: the machine boots the trial, its /proc/cmdline must have
#   rach2.slot=candidate and rach2.console=term, which the install's
#   `--console term` puts in the menu, and rach2.i915 when the install was
#   given `--i915`; machine_identity.sh must find the new kernel running from
#   the candidate slot. A line without one of these words fails the step,
#   naming the line, and commits nothing, so the next boot falls back to the
#   release the machine had.
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
#   fast-forward, `git push origin SHA:refs/heads/master`, milestone M and
#   each N of -c are closed when no issue of theirs but the release's own
#   cards is open, their cards are archived with `board.sh archive`,
#   sweep.sh removes what the release left, with its output kept beside the
#   record, and the last line printed is `release: passed SHA`. A milestone
#   with an open issue, such as a task that needs the installed release,
#   stays open, and the last line ends `; milestone N stays open for #A #B`;
#   the task that closes its last issue closes the milestone. A start of the
#   script that finds a record with `closing` that did not end finishes that
#   release first, whatever M and SHA it was given.
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
# `failure MACHINE RUN WORD...`, `failure newer MACHINE RUN SHA` for a run of -a
# at another commit, `card STEP N` for the card of each step on the board, and
# `card release N` for the release's, `STEP done TIME DETAIL`, `touched TIME`
# before the first change to the machine, `install started TIME`, `closing
# TIME`, `windows stopped RUN` for a Windows run that a stop ended, and last
# `failed STEP: WHY` or `closed TIME`; `record.lock`, which each append to the
# record and -r's rewrite of it hold; `log`, everything the script prints;
# `wt`, the worktree; `held`, a run directory for each start of the release
# and each Windows run of -a, made new, as `release-STAMP` and as the run's
# ID, each of a hold.pl that holds its run and alone signals it, as hold.pl
# says; `run`, the directory of the hold.pl whose child is the release that
# holds `lock`, which sends it TERM once that directory's stop exists, as
# stop.sh -r asks; `windows.run`, the directory of the hold.pl that leads
# the group of a Windows run of -a, which the release removes with the
# record once that hold.pl has ended; `jobs`, a run directory for each job of
# the release's shell, which hold.pl holds and signals, removed once its lock
# says the job ended; `prepare/windows`, the ID of -p's
# Windows preparation until its cleanup confirmed it stopped; `again.lock`,
# which one -a holds at a time; and the records of releases that ended, as
# `M-SHA-TIME`, with the build's digests beside each closed one as
# `M-SHA-TIME.sha256`. A release that closes its milestone copies the digests
# out of `wt` and then removes `wt`, with the build's products in it, last. A
# record that ended, or that belongs to another milestone or commit, is copied
# aside when the script starts on new arguments, and the new record then
# replaces it in one rename. The new record keeps the old one's `touched`
# line unless the old release closed, since the machine may still run what
# that release left, and its `card` lines when it is of the same milestone
# and did not close, so that the steps keep their cards. A done step is
# skipped when the script runs again, a transfer whose files still hash right
# on the machine is kept, and a step that writes the machine first asks it
# what it runs and which disk has the serial. A reboot counts as done when a
# boot younger than the request answers, by /proc/uptime. One release runs at
# a time, under a lock on STATE/lock.
#
# The exit status is 0 when the milestone was released; 1 when a step or a
# run failed, as the record's last line says; 2 when the arguments are
# refused, such as a SHA that is not on M's branch; and 75 when the release
# could not start or go on for a reason that passes, such as an unreachable
# host or a busy build copy, and the manager starts it again later. With -p
# the status is 0 when every part passed and 1 when one failed.

REPO=toksaitov/rach2
OWNER=/home/testing/rach2
COPY=$OWNER/build/worktrees/release
ARCH_DIR=build/x86_64
RECOVERY_IMAGE=images/rach2-recovery-with-key.krffs
WSL='MSYS_NO_PATHCONV=1 wsl.exe -- bash -s'
USAGE='usage: release.sh [-r] [-o] [-c N]... [-s STATE] M SHA
       release.sh -p [-o] [-s STATE] M SHA
       release.sh -a MACHINE [-s STATE] M SHA'
PREPARE_COPY=$OWNER/build/worktrees/prepare
# The commands that put the testing host's Doom WAD in a copy there. Git
# keeps no WAD and a sync from a fresh worktree sends none, so the copy takes
# the host's.
WAD="{ cp /etc/rach2/doom1.wad ports/doom/doom1.wad ||
            { echo 'release: wad: the testing host has no /etc/rach2/doom1.wad' >&2; false; }; } &&
        sh ports/doom/fetch_wad.sh"

# stamp: print the end of a run's ID that makes it unique, the time in UTC and
# six random hex digits, since the ID names the run's directory, which its
# cleanup removes whole.
stamp()
{
    printf '%s-%s\n' "$(date -u '+%m%d-%H%M%S')" "$(od -An -N3 -tx1 /dev/urandom | tr -d ' \n')"
}

# say TEXT: print TEXT and append it to the log, with the time in UTC.
say()
{
    echo "$(date -u '+%F %T') release: $*" | tee -a "$log"
}

# finish TEXT: print the release's last line, `release: TEXT`, and log it.
finish()
{
    echo "release: $*"
    echo "$(date -u '+%F %T') release: $*" >> "$log"
}

# record_lock: wait for STATE/record.lock, which each append to the record
# and -r's rewrite of it hold, so that a -w process's line never goes to a
# record that -r is replacing. record_unlock lets it go.
record_lock()
{
    exec 7>> "$state/record.lock"
    perl -MFcntl=:flock -e 'open(L, ">&=7") && flock(L, LOCK_EX) or exit 1'
}

record_unlock()
{
    exec 7>&-
}

# note LINE: append LINE to the record, and report what it says of a step
# to that step's card.
note()
{
    record_lock
    echo "$*" >> "$state/record"
    record_unlock
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
        'failure newer '*)
            report "$(echo "$line" | cut -d ' ' -f 3)" progress \
                "The run $(echo "$line" | cut -d ' ' -f 4) tests $(echo "$line" | cut -d ' ' -f 5), not the release's commit, so this release ends before the install; a release of that commit with -r keeps the run." ;;
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

# latest MACHINE: print the last line of the record that starts, passes or
# fails a run of MACHINE: `MACHINE started ID`, `MACHINE done ...` or `failure
# MACHINE ...`. The last of them gives the state of MACHINE's run, since -a
# starts a run again after its failure.
latest()
{
    awk -v m="$1" '($1 == "failure" && $2 == m) ||
        ($1 == m && ($2 == "done" || $2 == "started")) { l = $0 }
        END { if (l != "") print l }' "$state/record"
}

# failed_ MACHINE: succeed when the last run of MACHINE that the record has
# ended failed, and no run was started after it.
failed_()
{
    case $(latest "$1") in
        "failure $1 "*) return 0 ;;
    esac
    return 1
}

# runs_failed: succeed when a machine's run has failed, or a run started by
# -a tested a commit other than the release's, as its `failure newer` line
# says.
runs_failed()
{
    grep -q '^failure newer ' "$state/record" && return 0
    for m in full mac windows lenovo; do
        ! failed_ "$m" || return 0
    done
    return 1
}

# starts MACHINE: print how many runs of MACHINE the record has started since
# its last pass or failure, less those that a stop ended.
starts()
{
    awk -v m="$1" '($1 == "failure" && $2 == m) || ($1 == m && $2 == "done") { n = 0 }
        $1 == m && $2 == "started" { n++ }
        $1 == m && $2 == "stopped" { n-- }
        END { print n + 0 }' "$state/record"
}

# mark STEP DETAIL: record STEP as done now.
mark()
{
    note "$1 done $(date -u '+%FT%TZ') $2"
    say "$1: done: $2"
}

# fail STEP WHY: record that STEP failed, and exit 1. A Windows run that -a
# started fails as a run, so that the release it belongs to goes on.
fail()
{
    [ -z "$detached" ] || { failure windows "$wid" runner; say "windows: $2"; exit 1; }
    note "failed $1: $2"
    finish "failed: $1: $2"
    exit 1
}

# later WHY: say why the release cannot go on now, and exit 75. A Windows run
# that -a started fails as a run instead, since nobody starts it again.
later()
{
    [ -z "$detached" ] || { failure windows "$wid" runner; say "windows: $1"; exit 1; }
    grep -q '^touched ' "$state/record" 2> /dev/null || free_lenovo
    say "waiting: $1"
    exit 75
}

# net COMMAND...: run COMMAND, a gh or git call over the network, and while it
# fails for want of a connection run it again every 15 seconds, for up to five
# minutes, so that a short outage of GitHub does not stop the release. Any
# other failure returns COMMAND's status at once. COMMAND's standard error
# goes to the release's.
net()
{
    net_tries=20
    while :; do
        net_rc=0
        "$@" 2> "$state/net.$$" || net_rc=$?
        net_err=$(cat "$state/net.$$")
        rm -f "$state/net.$$"
        [ -z "$net_err" ] || printf '%s\n' "$net_err" >&2
        [ "$net_rc" -ne 0 ] || return 0
        net_tries=$((net_tries - 1))
        [ "$net_tries" -gt 0 ] &&
            printf '%s\n' "$net_err" | grep -qiE 'error connecting|could not resolve|failed to connect|connection (reset|refused|timed out)|timed out|tls handshake|http (502|503|504)' ||
            return "$net_rc"
        say "waiting: $1 cannot reach the network; trying again in 15 seconds" >&2
        sleep 15
    done
}

# refuse WHY: refuse the arguments for WHY, and exit 2.
refuse()
{
    echo "release: refused: $1" >&2
    exit 2
}

# failure MACHINE RUN WORD...: record that the run RUN on MACHINE failed the
# WORDs, once.
# failed_in CONSOLE: print the suites that a machine's run in CONSOLE failed,
# as ARCH:SUITE words, ARCH being i686, x86_64 or ia32, or nothing when it
# names none. The runner's line
# `run: result: failed: SUITE...` gives them while each of its words is one.
# On Windows the processes of a run share the console and can write into one
# another's lines, so when the line is broken the suites come from the lines
# that begin with a suite's own failure, `run: ARCH:SUITE: CASE: failure:` or
# `incomplete:`, CASE with no colon, or `run: ARCH:SUITE: FAIL in` or
# `INCOMPLETE in`, of which every failed suite prints at least two.
failed_in()
{
    words=$(tr -d '\r' < "$1" 2> /dev/null | grep '^run: result: failed: ' |
        tail -n 1 | sed 's/^run: result: failed: //')
    # shellcheck disable=SC2086 # the suites are words
    if [ -n "$words" ] && ! printf '%s\n' $words | grep -qvE '^(i686|x86_64|ia32):[a-z0-9-]+$'; then
        # shellcheck disable=SC2086 # the suites are words
        echo $words
        return 0
    fi
    tr -d '\r' < "$1" 2> /dev/null |
        sed -nE 's/^run: ((i686|x86_64|ia32):[a-z0-9-]+): ([^:]+: (failure|incomplete): |FAIL in |INCOMPLETE in ).*/\1/p' |
        sort -u | tr '\n' ' ' | sed 's/ $//'
}

failure()
{
    grep -q "^failure $1 $2 " "$state/record" || note "failure $*"
    m=$1 id=$2
    shift 2
    say "$m: run $id failed: $*"
}

# tests_failed: end the release on the failures of the record: each machine's
# run that failed last, and each run that -a started at another commit.
tests_failed()
{
    what=$({
        for m in full mac windows lenovo; do
            ! failed_ "$m" || latest "$m"
        done
        grep '^failure newer ' "$state/record"
    } | sed -e 's/^failure newer \([a-z]*\) \([^ ]*\) \([^ ]*\)$/\1 run \2: tested \3, not the release'"'"'s commit; release that commit with -r/' \
        -e 's/^failure \([a-z]*\) \([^ ]*\) /\1 run \2: /' |
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
# `recovery`, `cmdline` to its /proc/cmdline, `disk` to the number of the disk
# with the serial, and `up` to the seconds since it booted; set `now` empty
# when it does not answer. Fail, with the reason on stderr, when it answers as
# another machine.
where()
{
    now='' cmdline='' disk='' up=''
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
    cmdline=$(printf '%s\n' "$facts" | sed -n 2p)
    slot=$(printf '%s\n' "$cmdline" | tr ' ' '\n' | sed -n 's/^rach2\.slot=//p')
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
                "$1"*)
                    # HDA starts at 100, and the owner works beside the machine.
                    lenovo 'mixer vol 1' > /dev/null 2>&1 || say "lenovo: its volume could not be set"
                    return 0 ;;
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
# tests/registry has for ARCH, or `ia32:SUITE` of one it has for i686, which
# the ia32 profile runs on the x86-64 kernel.
suites()
{
    registry=$(git -C "$repo" show "$sha:tests/registry" 2> /dev/null) || return 1
    for w in "$@"; do
        case $w in
            i686:?*|x86_64:?*|ia32:?*) ;;
            *) return 1 ;;
        esac
        sarch=${w%%:*}
        [ "$sarch" != ia32 ] || sarch=i686
        printf '%s\n' "$registry" | awk -F'|' -v a="$sarch" -v s="${w#*:}" '
            /^[a-z]/ && $1 == s && ($6 == "" || index("," $6 ",", "," a ",")) { found = 1 }
            END { exit !found }' || return 1
    done
}

# again: print the lines that make the new record of -r run again only what
# failed in the last record, as the header says: `again MACHINE WORD...` for
# each machine whose failures can run alone, the passes the new record keeps,
# and the runs it waits for.
again()
{
    for m in full mac windows lenovo; do
        last=$(latest "$m")
        # shellcheck disable=SC2086 # the line is words
        set -- $last
        case $last in
            "failure $m "*)
                shift 3
                # A run that ended without naming failed suites runs again
                # what the machine's earlier run did.
                case $m in
                    lenovo)
                        case " $* " in
                            *" runner "*) previous "$m"; continue ;;
                        esac ;;
                    *) suites "$@" || { previous "$m"; continue; } ;;
                esac
                echo "again $m $*" ;;
            "$m done "*)
                [ "$m" != lenovo ] || continue
                if newer "$m" "$(echo "$last" | awk '{ print $NF }')"; then
                    previous "$m"
                    continue
                fi
                echo "$last"
                [ "$m" != full ] || grep -E '^run [0-9]+ success$' "$state/record" | tail -n 1 ;;
            "$m started "*) adopt "$m" "$3" ;;
        esac
    done
}

# previous MACHINE: print the record's last `again MACHINE` line, if any.
previous()
{
    grep "^again $1 " "$state/record" | tail -n 1
}

# newer MACHINE ID: succeed when -a started MACHINE's run ID at a commit of
# which SHA is an earlier commit, so that -r at SHA does not keep that run.
newer()
{
    at=$(sed -n "s/^failure newer $1 $2 //p" "$state/record" | tail -n 1)
    [ -n "$at" ] && [ "$at" != "$sha" ] &&
        git -C "$repo" merge-base --is-ancestor "$sha" "$at" 2> /dev/null
}

# adopt MACHINE ID: print the lines that carry into the new record of -r the
# run ID of MACHINE, which ended neither as a pass nor as a failure in the last
# record: a pass or the failed suites of a Mac run that has since ended, or
# the run itself while it runs, which the new release then waits for. A run
# that no longer runs and left no status runs again what its machine's
# earlier run did.
adopt()
{
    newer "$1" "$2" && { previous "$1"; return 0; }
    case $1 in
        mac)
            rc=$(sed -n 's/^rc=//p' "$runs/$2.times" 2> /dev/null)
            case $rc in
                0) echo "mac done $(date -u '+%FT%TZ') run $2" ;;
                '')
                    previous mac
                    ! mac_alive "$2" || echo "mac started $2" ;;
                *)
                    words=$(failed_in "$runs/$2.console")
                    # shellcheck disable=SC2086 # the suites are words
                    if [ -n "$words" ] && suites $words; then
                        echo "again mac $words"
                    else
                        previous mac
                    fi ;;
            esac ;;
        windows)
            previous windows
            ! windows_detached || echo "windows started $2" ;;
    esac
}

# full_failed ID: record the failures of the full run ID, as its artifact
# `failures` gives them.
full_failed()
{
    rm -rf "$state/full-$1"
    words=
    if net gh run download "$1" --repo "$REPO" --name failures --dir "$state/full-$1" > /dev/null 2>&1; then
        words=$(grep . "$state/full-$1/failures" 2> /dev/null | tr '\n' ' ' | sed 's/ $//')
    fi
    # shellcheck disable=SC2086 # the failures are words
    failure full "$1" ${words:-runner}
}

# full: find or start the full run of SHA, or of the suites of the record's
# `again full` line, and wait for it to end, first applying a result the
# record has and has not acted on. GitHub cuts a run's title past 509
# characters and ends it with `...`, so a long title matches by that prefix.
full()
{
    listed=$(sed -n 's/^again full //p' "$state/record")
    # shellcheck disable=SC2046 # the result is two words
    set -- $(grep -E '^run [0-9]+ (success|failure)$' "$state/record" | tail -n 1)
    case ${3-} in
        success) mark full "run $2${listed:+, of $listed only}"; return 0 ;;
        failure) full_failed "$2"; return 0 ;;
    esac
    title="Release tests of $(echo "$sha" | cut -c 1-8) for milestone $milestone"
    # shellcheck disable=SC2086 # the suites are words
    [ -z "$listed" ] || title="$title: again only $(plain $listed)"
    while :; do
        run=$(net gh run list --repo "$REPO" --workflow full.yml --limit 50 \
            --json databaseId,displayTitle,status,conclusion,createdAt \
            --jq "\"$title\" as \$t | [.[] | select(.displayTitle as \$d |
                (\$d == \$t or ((\$d | endswith(\"...\")) and
                 (\$t | startswith(\$d[:-3])))) and
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
                success) mark full "run $1${listed:+, of $listed only}"; return 0 ;;
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
        [ -z "$listed" ] || set -- "$@" -f suites="$listed"
        net gh workflow run full.yml --repo "$REPO" --ref "$branch" "$@" ||
            later "the full run could not be started"
        say "full: started a run of $sha${listed:+ for $listed only}"
        sleep 30
    done
}

# build: build SHA for the Lenovo on the testing host and fetch the results.
build()
{
    net git -C "$repo" fetch -q origin || later "the fetch failed"
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
    # The manifest stages the WAD as an optional file, so a root built
    # without it has no game; the root is read to be sure.
    staged="{ ( . tools/setup/e2fsprogs_path.sh &&
            debugfs -R 'cat /demo/doom1.wad' $ARCH_DIR/images/os-real-gnu.ext2 2> /dev/null |
                cmp -s - ports/doom/doom1.wad ) ||
            { echo 'release: wad: the root has no /demo/doom1.wad equal to ports/doom/doom1.wad' >&2; false; }; }"
    # The build runs as a job that hold.pl holds, so that a stop of the
    # release stops it here and, through testing.sh, on the host.
    bd_job=$(job_dir build) || later "no directory for a job under $state/jobs"
    (
        RACH2_TESTING_COPY=$COPY
        export RACH2_TESTING_COPY
        held "$bd_job" sh tools/dev/testing.sh run \
            "$WAD && ARCH=x86_64 make clean && ARCH=x86_64 make machine GNU_AGENTS=all && $staged &&
            $native cd $ARCH_DIR &&
            gzip -1 -c images/os-real-gnu.ext2 > images/os-real-gnu.ext2.gz &&
            sha256sum rach2/kernel rach2-recovery/kernel utils/sbin/bootctl $RECOVERY_IMAGE images/os-real-gnu.ext2 $payload \
                > images/release.sha256"
    ) > "$state/build.log" 2>&1 &
    if ! job_wait "$! $bd_job"; then
        if grep -q 'testing: the copy at .* is in use\|testing: the host did not answer' "$state/build.log"; then
            later "the build copy is busy or the host does not answer"
        fi
        if grep -q '^preflight: result: refused: .* GiB the build needs' "$state/build.log"; then
            later "the testing host is short of space"
        fi
        if grep -q '^release: wad: ' "$state/build.log"; then
            fail build "$(sed -n 's/^release: wad: //p' "$state/build.log" | head -n 1)"
        fi
        fail build "the build failed: $(tail -n 3 "$state/build.log" | tr '\n' ' ')"
    fi
    RACH2_TESTING_COPY=$COPY sh tools/dev/testing.sh get "$ARCH_DIR/rach2/kernel" \
        "$ARCH_DIR/rach2-recovery/kernel" "$ARCH_DIR/utils/sbin/bootctl" "$ARCH_DIR/$RECOVERY_IMAGE" \
        "$ARCH_DIR/images/os-real-gnu.ext2.gz" "$ARCH_DIR/images/release.sha256" \
        ${payload:+"$ARCH_DIR/$payload"} >> "$state/build.log" 2>&1 ||
        later "the build's results could not be fetched"
    for f in rach2/kernel rach2-recovery/kernel utils/sbin/bootctl "$RECOVERY_IMAGE" $payload; do
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
        fail install "the installer was started and its log is gone, so a reboot interrupted it; run rach2-install --console term --update by hand from recovery"
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
            if lenovo 'grep -q -e "--slots DISK" /bin/rach2-install'; then
                lenovo "rach2-install --slots /dev/disk$disk" > "$state/slots.log" 2>&1 ||
                    fail kernel "rach2-install --slots: $(tr '\n' ' ' < "$state/slots.log")"
                cat "$state/slots.log" >> "$state/log"
            fi
            lenovo 'bootctl kernel /dev/shm/kernel' > "$state/kernel.log" 2>&1
            grep -q '^bootctl: installed:' "$state/kernel.log" ||
                fail kernel "$(tr '\n' ' ' < "$state/kernel.log")"
            mark kernel "candidate slot"
        fi
        step=install
        note "install started $(date -u '+%FT%TZ')"
        install_words='--console term'
        if i915; then
            install_words="$install_words --i915"
        fi
        lenovo "setsid sh -c 'rach2-install $install_words --update /dev/shm/root.ext2 /dev/disk$disk \
            > /dev/shm/install.log 2>&1; echo \$? > /dev/shm/install.status' \
            > /dev/null 2>&1 < /dev/null &" || fail install "the installer could not be started"
        say "install: rach2-install $install_words --update runs on the machine"
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
    if i915; then
        p1=/dev/disk${disk}p1
        menu=$(lenovo "mkdir -p /mnt/p1 && mount -t vfat -o ro $p1 /mnt/p1 || exit 1
            cat /mnt/p1/boot/grub/grub.cfg; s=\$?; umount /mnt/p1; exit \$s") ||
            fail install "the menu on $p1 could not be read"
        # The root's lines, by slot, are those the installer gives the token.
        bare=$(printf '%s\n' "$menu" | awk '
            /^[ \t]*(multiboot|multiboot2)[ \t]/ &&
            /[ \t]rach2\.slot=(candidate|fallback)([ \t]|$)/ {
                roots++
                if (!/[ \t]rach2\.i915([ \t]|$)/ && bare == "") bare = $0
            }
            END {
                if (roots == 0) print "no line boots the root"
                else if (bare != "") print bare
                exit (roots == 0 || bare != "")
            }') || fail install "the installed menu on $p1 lacks rach2.i915: $bare"
    fi
    mark install "root $(digest images/os-real-gnu.ext2)"
}

# i915: succeed when the build's installer takes --i915, which puts rach2.i915
# on the installed root's lines; recovery checks that the machine's recovery
# system runs this installer.
i915()
{
    grep -q -e '\[--i915\]' "$state/wt/utils/rach2-install" 2> /dev/null
}

# setting MACHINE NAME: print each value that the machines file gives NAME for
# MACHINE, one a line.
setting()
{
    awk -v m="$1" -v n="$2" '$1 == m && $2 == n { sub(/^[^ ]+ +[^ ]+ +/, ""); print }' \
        "$repo/tools/flow/machines"
}

# has MACHINE: succeed when the machines file has a setting for MACHINE and
# -o does not leave it out.
has()
{
    case $1 in
        mac|windows) [ -z "$only" ] || return 1 ;;
    esac
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
# record's `again MACHINE` line, which -p does not read, or every suite of both
# architectures that SHA's tests/registry has, less the style suite, which the
# full run checks once for the release, and less a suite none of whose cases
# runs on MACHINE's host system, which etude would refuse; say on stderr which
# those are.
selection()
{
    words=
    [ -n "$prepare" ] || words=$(sed -n "s/^again $1 //p" "$state/record")
    [ -n "$words" ] || words=$(git -C "$repo" show "$sha:tests/registry" | awk -F'|' '
        /^[a-z]/ { for (i = 0; i < 2; i++) { a = i ? "x86_64" : "i686"
            if ($6 == "" || index("," $6 ",", "," a ",")) print a ":" $1 } }')
    case $1 in
        mac) host=macos ;;
        windows) host=windows ;;
        *) host=linux ;;
    esac
    # Without the manifests etude refuses the suite itself, and says why.
    unhosted=$(unhosted "$host" | tr '\n' ' ' | sed 's/ $//')
    [ -z "$unhosted" ] ||
        say "$1: leaving out $unhosted, whose cases do not run on $host" >&2
    # shellcheck disable=SC2086 # the suites are words
    printf '%s\n' $words | grep -v ':style$' |
        awk -v out=" $unhosted " '{ s = $0; sub(/^[^:]*:/, "", s) }
            index(out, " " s " ") == 0' | tr '\n' ' '
}

# unhosted HOST: print the suites of SHA's tests/cases none of whose cases runs
# on the host system HOST, as etude reads a case's `requirements.hosts`: a
# suite NAME's manifest is the first file in tests/cases whose name ends in
# `.NAME.json`.
unhosted()
{
    git -C "$repo" ls-tree --name-only "$sha" tests/cases/ |
        perl -MJSON::PP -e '
            my ($repo, $sha, $host) = @ARGV;
            my %seen;
            while (my $path = <STDIN>) {
                chomp $path;
                my ($suite) = $path =~ m{^tests/cases/.+\.([^./]+)\.json$} or next;
                next if $seen{$suite}++;
                my $text = `git -C "$repo" show "$sha:$path"`;
                $? == 0 or exit 1;
                my $doc = decode_json($text);
                my $runs = 0;
                for my $case (@{$doc->{cases} || []}) {
                    my $hosts = $case->{requirements}{hosts};
                    $runs = 1 if !defined $hosts || grep { $_ eq $host } @$hosts;
                }
                print "$suite\n" unless $runs;
            }' "$repo" "$sha" "$1"
}

# mac_run ID: make the run ID of the release tests on the Mac, of the words
# that `selection mac` prints, in the copy TREE with the admission site SITE,
# with its output in RUNS/ID.console and its status appended to RUNS/ID.times
# as `rc=N`; with -p make only its preparation, with `routine-prepare`. Its
# scratch and the evidence of a failed preparation go to its run directory,
# TREE/build/tmp/ID, not to the TMPDIR that macOS gives under /var/folders: a
# failure marks it kept first, and a pass removes it unless etude kept
# evidence there.
mac_run()
{
    id=$1
    rdir=$tree/build/tmp/$id
    routine='routine-sequence'
    [ -z "$prepare" ] || routine='routine-prepare'
    words=$(selection mac)
    accel=$(setting mac accel)
    date '+start %FT%T%z' > "$runs/$id.times"
    # shellcheck disable=SC2086 # the suites are words
    (
        eval "$(environment mac)"
        unset ETUDE_SITE ETUDE_ADMITTED ETUDE_PREPARATION_CLEAN
        export ETUDE_RUN_DIR="$rdir" TMPDIR="$rdir/tmp"
        cd "$tree" && git checkout -q -f --detach "$sha" && mkdir -p "$TMPDIR" &&
            make build/tools/etude/etude build/tools/etude/etude-volume &&
            cp build/tools/etude/etude build/tools/etude/etude-volume "$site/bin/" &&
            ETUDE_SITE=$site/etude-site "$site/bin/etude" run -N -t "$tree" -a both -A "$accel" \
                -J "$runs/$id.selection.json" $words &&
            ETUDE_PHASE_SELECTION_EXPORT=$runs/$id.selection.json \
                "$site/bin/etude" run "$routine" phase_first_run "$tree" "$runs/$id" both \
                "$id" "$site/etude-site" "$site/bin/etude" "$accel" $words
    ) > "$runs/$id.console" 2>&1
    rc=$?
    [ "$rc" = 0 ] || { mkdir -p "$rdir" &&
        echo "failed: the run exited $rc; evidence: $runs/$id $runs/$id.console" >> "$rdir/KEPT"; }
    [ "$rc" != 0 ] || mac_clean "$id"
    echo "rc=$rc" >> "$runs/$id.times"
    date '+end %FT%T%z' >> "$runs/$id.times"
}

# mac_clean ID: remove the run directory of the Mac's run ID, which has
# stopped, unless it holds KEPT.
mac_clean()
{
    [ -n "$1" ] && [ ! -e "$tree/build/tmp/$1/KEPT" ] || return 0
    rm -rf "${tree:?}/build/tmp/$1"
}

# mac_stop: stop the Mac's run that still runs, which belongs to an earlier
# record, with its process group, through the hold.pl that leads the group
# in the run's directory that build/machines/mac/run names, and check that
# the ledger has none of its claims. A run that does not end in five
# minutes, or whose holder was killed, ends the release for a person.
mac_stop()
{
    [ -f "$macdir/run" ] || return 0
    read -r ms_dir last < "$macdir/run"
    if mac_live "$ms_dir"; then
        ms_state=$(perl "$repo/tools/flow/hold.pl" stop "$ms_dir" 300) ||
            fail mac "the earlier run on the Mac, $last, did not stop: $ms_state"
        say "mac: stopped the earlier run $last"
        mac_clean "$last"
    fi
    held_gone "$ms_dir" ||
        fail mac "the holder of the earlier run on the Mac, $last, ended without its status, so what it held may still run; stop.sh lists it"
    rm -f "$macdir/run"
    [ -z "$last" ] || ! grep -qF "$last" "$site/ledger/claims" 2> /dev/null ||
        fail mac "the stopped run $last keeps a claim in $site/ledger/claims; a person checks it and removes it"
}

# mac_live DIR: succeed while the Mac's run whose directory is DIR runs: its
# hold.pl holds DIR/live, which it does until nothing else is in the group
# it leads, or is about to take it.
mac_live()
{
    case $(perl "$repo/tools/flow/hold.pl" state "${1:-/nonexistent}") in
        running | starting) return 0 ;;
    esac
    return 1
}

# held_gone DIR: remove DIR, a run's directory, once its hold.pl's lock says
# that the run ended; succeed then, or when there is no DIR, and fail while
# the run may still run.
held_gone()
{
    case $(perl "$repo/tools/flow/hold.pl" state "${1:-/nonexistent}") in
        "ended "*) rm -rf "$1" ;;
        absent) ;;
        *) return 1 ;;
    esac
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
    net git -C "$repo" fetch -q origin || later "the fetch failed"
    mac_launch "milestone$milestone-$(echo "$sha" | cut -c 1-12)-mac-$(stamp)" "$sha"
}

# mac_launch ID AT: record that the Mac's run ID started, and start it at the
# commit AT, detached, as `release.sh -m ID M AT`, under a hold.pl -g that
# leads a process group of its own, in the run's own directory,
# build/machines/mac/held/ID, made new, which the Mac's `run` file names with
# ID before the start; the hold.pl sends the group TERM once that
# directory's stop exists, and KILL half an hour later.
mac_launch()
{
    mkdir -p "$runs/$1"
    note "mac started $1"
    ml_dir=$macdir/held/$1
    perl "$repo/tools/flow/hold.pl" sweep "$macdir/held"
    { mkdir -p "$macdir/held" && mkdir "$ml_dir"; } || fail mac "$ml_dir cannot be made"
    { echo "$ml_dir $1" > "$macdir/run.new" && mv "$macdir/run.new" "$macdir/run"; } ||
        fail mac "$macdir/run cannot be written"
    nohup perl "$repo/tools/flow/hold.pl" run -g -k 1800 "$ml_dir" sh "$repo/tools/flow/release.sh" \
        -s "$state" -m "$1" "$milestone" "$2" > /dev/null 2>&1 < /dev/null 8>&- 9>&- &
    say "mac: started the run $1"
}

# mac_alive ID: succeed when the Mac's run ID still runs.
mac_alive()
{
    ma_dir=$(awk -v id="$1" '$2 == id { print $1 }' "$macdir/run" 2> /dev/null)
    [ -n "$ma_dir" ] && mac_live "$ma_dir"
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
        # The status is read once the run's holder has ended, so that
        # nothing of the run is left when the release goes on.
        mw_dir=$(awk -v id="$id" '$2 == id { print $1 }' "$macdir/run" 2> /dev/null)
        if [ -n "$mw_dir" ] && mac_live "$mw_dir"; then
            say "mac: the run $id runs"
            sleep 60
            continue
        fi
        rc=$(sed -n 's/^rc=//p' "$runs/$id.times" 2> /dev/null)
        if [ -n "$mw_dir" ] && ! held_gone "$mw_dir"; then
            say "mac: the holder of the run $id ended without its status, so what it held may still run; stop.sh lists it"
            failure mac "$id" runner
            return 0
        fi
        [ -z "$rc" ] || break
        rm -f "$macdir/run"
        if [ "$(starts mac)" -ge 3 ]; then
            failure mac "$id" runner
            return 0
        fi
        say "mac: the run $id ended without a status; starting another"
        mac_start
    done
    rm -f "$macdir/run"
    if [ "$rc" = 0 ]; then
        mark mac "run $id"
        return 0
    fi
    # shellcheck disable=SC2046 # the suites are words
    failure mac "$id" $(failed_in "$runs/$id.console" | grep . || echo runner)
}

# job_dir NAME: make a directory of its own for a job named NAME, under
# STATE/jobs, and print it.
job_dir()
{
    mkdir -p "$state/jobs" && mktemp -d "$state/jobs/$1.XXXXXX"
}

# held DIR COMMAND...: run COMMAND, a function of this script or a program,
# as a job that hold.pl holds in its run directory DIR, from job_dir; run as
# `held DIR COMMAND... &`, and keep `$! DIR` as the job. The subshell that
# `&` makes forks the job, which opens the FIFO DIR/go for reading and runs
# COMMAND only once it reads `go` there, and then execs hold.pl in its own
# place with the FIFO's write end on descriptor 6, so that the job is
# hold.pl's unreaped child and only hold.pl signals it: the job cannot end
# before hold.pl holds it, as it could if a shell that reaps in its SIGCHLD
# handler, as bash does, reaped it before the exec. hold.pl reads DIR/stop
# before it writes `go`, and a stop, or an exec of hold.pl that fails, ends
# the FIFO, and the job exits 143 having run nothing. hold.pl stops the job
# once DIR/stop exists or this shell has gone, TERM and KILL HOLD_KILL
# seconds later. A program runs in the job's place, a function in the job.
held()
{
    hd_dir=$1
    shift
    mkfifo "$hd_dir/go" || exit 127
    {
        {
            IFS= read -r hd_go < "$hd_dir/go" && [ "$hd_go" = go ] || exit 143
            case $(command -v "$1") in
                /*) exec "$@" ;;
                *) "$@" ;;
            esac
        } <&4 4<&- &
    } 4<&0
    exec 6<> "$hd_dir/go"
    exec perl "$repo/tools/flow/hold.pl" run -P -k "${HOLD_KILL:-3600}" "$hd_dir" -p "$!" < /dev/null
}

# job_ask JOB: ask hold.pl to stop the job JOB, `PID DIR` as held gives it,
# by its tombstone DIR/stop.
job_ask()
{
    perl "$repo/tools/flow/hold.pl" stop "${1#* }" 0 > /dev/null
}

# job_wait JOB: wait until the job JOB, `PID DIR`, has ended and return its
# status, which hold.pl writes to DIR/end; remove DIR once hold.pl's lock
# says that it ended. A signal that the shell traps ends a wait with a
# status above 128 while hold.pl still holds the job, or is about to, which
# its lock tells, and the wait goes on; hold.pl's KILL bounds it, and a
# holder that has not taken its lock in half a minute ends it. A holder
# that ended without DIR/end, killed or never started, gives 127, and DIR
# stays.
job_wait()
{
    jw_pid=${1%% *} jw_dir=${1#* }
    wait "$jw_pid"
    jw_rc=$?
    jw_turns=0
    while [ "$jw_rc" -gt 128 ]; do
        case $(perl "$repo/tools/flow/hold.pl" state "$jw_dir") in
            running) ;;
            starting) [ "$jw_turns" -lt 30 ] || break ;;
            *) break ;;
        esac
        jw_turns=$((jw_turns + 1))
        sleep 1
        wait "$jw_pid"
        jw_rc=$?
    done
    jw_state=$(perl "$repo/tools/flow/hold.pl" state "$jw_dir")
    case $jw_state in
        "ended "*)
            jw_rc=${jw_state#ended }
            rm -rf "$jw_dir" ;;
        *) jw_rc=127 ;;
    esac
    return "$jw_rc"
}

# job_stop JOB: stop the job JOB, `PID DIR`, through hold.pl, and wait for
# it to end, as job_ask and job_wait do.
job_stop()
{
    job_ask "$1"
    job_wait "$1"
}

# tssh ARG...: ssh to the testing host with ARGs. The keepalives end a session
# whose link has dropped within a minute, with ssh's status 255, where TCP
# alone would leave it waiting for hours.
tssh()
{
    ssh -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=4 testing "$@"
}

# wsl COMMAND: run COMMAND in Ubuntu in WSL on the testing host, for five
# minutes at most, as bound.pl bounds it, since a stop waits for it.
wsl()
{
    echo "$1" | perl "$repo/tools/flow/bound.pl" 300 ssh -o BatchMode=yes -o ConnectTimeout=10 \
        -o ServerAliveInterval=15 -o ServerAliveCountMax=4 testing "$WSL"
}

# lost FILE: print why the Windows part whose output FILE holds ended without
# a status, from the line `lost: WHY` that windows_detach wrote, and how many
# times its link was lost.
lost()
{
    why=$(tr -d '\r' < "$1" 2> /dev/null | sed -n 's/^lost: //p' | tail -n 1)
    n=$(grep -c '^release: .*: link lost: ' "$1" 2> /dev/null)
    printf '%s' "${why:-its output has no status line}"
    [ "${n:-0}" -eq 0 ] || printf '; the link was lost %s times' "$n"
    echo
}

# windows_detach ID PART FILE CONSOLE: run standard input, a script for the
# testing host's MSYS2 bash, on Windows apart from any ssh session, and write
# what it printed to FILE once it has ended. OpenSSH for Windows ends every
# process of a session when the session ends, by the session's Job Object, so
# a run fed to a session's bash ends with a dropped link. The script goes to
# OUT/ID.PART.sh. The start first writes a fresh nonce to OUT/ID.PART.token,
# failing when that file exists, as a stop's token leaves it, and then reads
# OUT/windows.current, which windows_gate wrote, starting nothing unless it
# names ID; only then does WMI's Win32_Process.Create start winjob.exe, in
# no Job Object, which makes the job Global\rach2-ID.PART, joins it, reads
# the token and, while it still holds the nonce, starts the script in bash,
# so that everything the part starts is born in the job and ends with it, as
# winclean.sh says. The bash writes its output to OUT/ID.PART.out, appends a
# byte to OUT/ID.alive each minute while it runs, which keeps WSL's claims
# for ID,
# and once the script has ended with a line `status N`, writes N to
# OUT/ID.PART.rc, so that no line of the output that begins `status ` reads
# as the part's end. A TERM while the start runs ends the start's ssh before
# this ends. A poll each minute reads the job's state and the output: a part
# whose job is absent is done when its status file exists, and otherwise
# ended, or did not start when it wrote no output; a part whose status file
# exists runs while its job is not absent, since its helper may not yet have
# ended the job, and so the caller's next step, which ends the claim in WSL,
# comes only after the job's end. A poll that ssh ends with any
# status but 0, or where Windows cannot say, is never read, since its reply
# may lack its end: FILE notes it, as a lost link when the status is 255,
# with ssh's own lines, and the next poll connects again; after sixty polls
# in a row without a whole answer, an hour or more, the run is given up, as
# FILE says, and windows_clean stops it
# once the host answers. FILE gets the output, with each of its lines that
# begins `status ` marked `output: `, and ends with `status N` from
# OUT/ID.PART.rc, or, when the process ended without one, did not start or
# was given up, with a line `lost: WHY` and, when the host answers, the last
# 40 lines of CONSOLE, the run's console under OUT.
windows_detach()
{
    wd_base=$wout/$1.$2 wd_file=$3 wd_console=$wout/$4
    wd_poll=$state/$1.$2.poll wd_err=$state/$1.$2.err wd_start=''
    wd_nonce=$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')
    wd_job=$(job_dir start) || { echo "lost: no directory for its start" > "$wd_file"; return; }
    : > "$wd_file"
    exec 3<&0
    trap '[ -z "$wd_start" ] || job_stop "$wd_start"; exit 143' TERM
    {
        echo "b=$wd_base"
        echo "n=$wd_nonce id=$1 o=$wout"
        cat << 'END'
if ! (set -C && echo "$n" > "$b.token") 2> /dev/null; then
    echo 'start: cancelled'
    exit 0
fi
if [ "$(cat "$o/windows.current" 2> /dev/null)" != "$id" ]; then
    echo stop > "$b.token"
    : > "$b.stopped"
    echo 'start: cancelled'
    exit 0
fi
rm -f "$b.out" "$b.rc"
END
        echo "cat > $wd_base.sh << 'END_OF_RUN'"
        echo "exec > $wd_base.out 2>&1 < /dev/null"
        echo "while kill -0 \$\$ 2> /dev/null; do printf . >> $wout/$1.alive; sleep 60; done &"
        echo "("
        cat <&3
        echo ")"
        echo "b=$wd_base"
        cat << 'END'
s=$(tail -n 1 "$b.out" | sed -n 's/^status //p')
case $s in '' | *[!0-9]*) ;; *) echo "$s" > "$b.rc.part" && mv "$b.rc.part" "$b.rc" ;; esac
END
        echo "END_OF_RUN"
        sh "$repo/tools/flow/winjob.sh" "$wout"
        # shellcheck disable=SC2016 # the remote shells expand these
        cat << 'END'
if [ ! -x "$winjob" ]; then
    echo stop > "$b.token"
    : > "$b.stopped"
    echo 'start: no winjob program'
    exit 0
fi
j="Global\\rach2-${b##*/}"
l=$(MSYS_NO_PATHCONV=1 powershell.exe -NoProfile -NonInteractive -Command "
    \$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{CommandLine='$winjob run $j $b.token $n C:/msys64/usr/bin/bash.exe -l $b.sh'}
    'created {0}' -f \$r.ReturnValue" < /dev/null | tr -d '[:cntrl:]')
case $l in
'created 0')
    # The job, or the output of a part that already ended.
    i=0
    while [ ! -e "$b.out" ] && [ "$("$winjob" state "$j")" = absent ] && [ "$i" -lt 30 ]; do
        i=$((i + 1))
        sleep 1
    done
    echo 'start: status 0' ;;
'created '*) echo "start: status ${l#created }" ;;
*) echo "start: no status from Win32_Process.Create: $l" ;;
esac
END
    } | held "$wd_job" tssh 'C:/msys64/usr/bin/bash.exe -l -s' > "$wd_err.start" 2>&1 &
    wd_start="$! $wd_job"
    job_wait "$wd_start"
    wd_start=
    trap - TERM
    exec 3<&-
    if grep -q '^start: cancelled' "$wd_err.start"; then
        echo "lost: it did not start, since winclean.sh had stopped the run or another run had started" >> "$wd_file"
        rm -f "$wd_err.start"
        return
    fi
    # Polls in a row without a whole answer, each a minute's sleep apart.
    wd_lost=0
    while :; do
        sleep 60 9>&- &
        wd_sleep=$!
        wait "$wd_sleep"
        wd_sleep=
        {
            echo "b=$wd_base c=$wd_console"
            sh "$repo/tools/flow/winjob.sh" "$wout"
            # shellcheck disable=SC2016 # the remote shell expands these
            cat << 'END'
s=$("$winjob" state "Global\\rach2-${b##*/}" 2> /dev/null)
a=unknown
# The bash writes the status file before it exits, and the helper ends the
# job only after that, so a part is done once its job is absent too.
case $s in
'active '*) a=running ;;
absent)
    if [ -e "$b.rc" ]; then a=done; elif [ -e "$b.out" ]; then a=ended; else a=unstarted; fi ;;
esac
[ "$a" != unknown ] || { echo "winjob: Windows cannot say whether ${b##*/} runs: ${s:-no answer}" >&2; exit 3; }
echo "poll $a"
case $a in done | ended) sed 's/^status /output: status /' "$b.out" ;; esac
[ "$a" != done ] || echo "status $(cat "$b.rc")"
[ "$a" != ended ] || { echo 'poll console'; tail -n 40 "$c"; }
exit 0
END
        } | tssh 'C:/msys64/usr/bin/bash.exe -l -s' > "$wd_poll" 2> "$wd_err"
        wd_rc=$?
        wd_state=$(tr -d '\r' < "$wd_poll" | sed -n '1s/^poll //p')
        if [ "$wd_rc" != 0 ] || [ -z "$wd_state" ]; then
            if [ "$wd_rc" = 255 ]; then
                echo "release: $(date -u '+%FT%TZ'): link lost: $(tr -d '\r' < "$wd_err" | tr '\n' ' ')" >> "$wd_file"
            else
                echo "release: $(date -u '+%FT%TZ'): the poll ended with status $wd_rc: $(tr -d '\r' < "$wd_err" | tr '\n' ' ')" >> "$wd_file"
            fi
            wd_lost=$((wd_lost + 1))
            if [ "$wd_lost" -ge 60 ]; then
                echo "lost: the testing host has not answered sixty polls in a row, an hour or more; the run may still run there" >> "$wd_file"
                break
            fi
            continue
        fi
        wd_lost=0
        case $wd_state in
            done)
                tr -d '\r' < "$wd_poll" | sed 1d >> "$wd_file"
                break ;;
            ended)
                tr -d '\r' < "$wd_poll" | sed -e 1d -e '/^poll console$/,$d' >> "$wd_file"
                echo "lost: its process ended without a status; the last lines of $wd_console:" >> "$wd_file"
                tr -d '\r' < "$wd_poll" | sed '1,/^poll console$/d' >> "$wd_file"
                break ;;
            unstarted)
                echo "lost: it did not start: $(tr -d '\r' < "$wd_err.start" | tr '\n' ' ')" >> "$wd_file"
                break ;;
        esac
    done
    rm -f "$wd_poll" "$wd_err" "$wd_err.start"
}

# windows_settings: read Windows' settings from the machines file: its copy,
# output directory, admission site and accelerator, its job count, and the
# output directory as WSL sees it, and the directory of WSL's claims' locks.
windows_settings()
{
    wtree=$(setting windows tree) wout=$(setting windows out) wsite=$(setting windows site)
    waccel=$(setting windows accel)
    wjobs=$(setting windows env | sed -n 's/^JOBS=//p')
    wjobs=${wjobs:-4}
    wheld=/mnt/$(echo "$wout" | cut -c 1 | tr '[:upper:]' '[:lower:]')$(echo "$wout" | cut -c 3-)
    wlock=$OWNER/build/flow/claims
}

# windows_stop: stop Windows' preparation that runs in the background, the
# observer of its cases and the beat, and then end the claims of WSL that
# cover them, so that a release that exits or is stopped leaves none of them
# behind; nothing of Windows' run is left running between its preparation and
# its cases. When it stopped a part, it first stops what of the run still
# runs on Windows and removes its scratch there, so that the whole host's
# claim lasts until the cases have stopped; when windows_clean cannot confirm
# that, the claim stays until ID.alive, which the cases append to while they
# run, has not grown for ten minutes.
windows_stop()
{
    wstopped='' wleft=''
    for wx_job in "$wprep" "$wobs" "$wbeat"; do
        [ -z "$wx_job" ] || job_ask "$wx_job"
    done
    if [ -n "$wprep" ]; then
        job_wait "$wprep"
        wprep='' wstopped=1
    fi
    if [ -n "$wobs" ]; then
        job_wait "$wobs"
        wobs='' wstopped=1
    fi
    if [ -n "$wbeat" ]; then
        job_wait "$wbeat"
        wbeat=
    fi
    [ -z "$wholding" ] || wstopped=1
    # A run that a stop ended counts as none of the three.
    [ -z "$wstopped" ] || [ -z "$wid" ] || note "windows stopped $wid"
    if [ -n "$wstopped" ] && [ -n "$wid" ] && ! windows_clean "$wid"; then
        wleft=1
        say "windows: $(unstopped "$wid")"
    fi
    if [ -n "$wholding" ]; then
        wholding=
        if [ -z "$wleft" ]; then
            wsl "touch $wheld/$wid.end" > /dev/null 2>&1
            claim_stop "$wid" hold | while read -r line; do say "windows: $line"; done
        fi
    fi
    [ -z "$wid" ] || rm -f "$state/$wid".*.poll "$state/$wid".*.err "$state/$wid".*.err.start
}

# windows_runner: print the run directory that STATE/windows.run names while
# the hold.pl of the Windows run of -a holds it, or is about to (G12).
windows_runner()
{
    wr_dir=$(cat "$state/windows.run" 2> /dev/null)
    [ -n "$wr_dir" ] || return 0
    case $(perl "$repo/tools/flow/hold.pl" state "$wr_dir") in
        running | starting) echo "$wr_dir" ;;
    esac
}

# windows_clean ID...: stop what is left of each of Windows' runs ID on
# Windows and remove the scratch it made outside its own files, as
# winclean.sh does. Succeed once winclean.sh has confirmed that nothing of
# them runs there or can start; fail when something may, when the host does
# not answer, or when it has not answered within twenty minutes, so that a
# trap that waits for it ends.
windows_clean()
{
    winclean "$@" > /dev/null 2>&1
}

# winclean [-s | -l | -g] ID...: run winclean.sh on Windows with Windows'
# copy and output directory, after the lines that build its winjob program,
# for twenty minutes at most.
winclean()
{
    wc_mode=
    case $1 in -s | -l | -g) wc_mode=$1; shift ;; esac
    {
        sh "$repo/tools/flow/winjob.sh" "$wout"
        cat "$repo/tools/flow/winclean.sh"
    } | perl "$repo/tools/flow/bound.pl" 1200 ssh -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 \
        -o ServerAliveCountMax=4 testing "C:/msys64/usr/bin/bash.exe -l -s $wc_mode $wtree $wout $*"
}

# claim_pl ARG...: print the lines of a WSL script that run tools/flow/claim.pl
# with ARG..., the program read from the script itself.
claim_pl()
{
    echo "perl - $* << 'END_OF_CLAIM'"
    cat "$repo/tools/flow/claim.pl"
    echo "END_OF_CLAIM"
}

# claim_ssh SECONDS: run standard input as a script in WSL on the testing
# host, as bound.pl bounds it, so that a trap that waits for a claim's ssh
# waits no longer than SECONDS, though a live sshd's wsl.exe hangs.
claim_ssh()
{
    perl "$repo/tools/flow/bound.pl" "$1" ssh -o BatchMode=yes -o ConnectTimeout=10 \
        -o ServerAliveInterval=15 -o ServerAliveCountMax=4 testing "$WSL"
}

# claim_start ID KIND N LAUNCH DONE: start attempt N of a claim of WSL's
# admission for Windows' run ID apart from any ssh session, in one ssh that
# lasts two minutes at most. The claim's files are LOCKS/ID.KIND.N.* and its
# tombstones LOCKS/ID.KIND.stop and LOCKS/ID.KIND.N.stop, LOCKS being
# build/flow/claims of the owner's clone in WSL, which is ext4, where flock
# holds; claim.pl's header says what each is. The ssh creates
# LOCKS/ID.KIND.N.pl, a copy of claim.pl, failing when it exists, and runs
# it as the runner in a session of its own, since WSL ends what a session
# started when wsl.exe ends, with its output in LOCKS/ID.KIND.N.out. The
# runner starts the launcher LAUNCH unless a tombstone exists or
# OUT/windows.current names another run; once admitted, the launcher runs
# claim.pl's holder, which holds the claim while OUT/DONE is missing, no
# tombstone exists and OUT/ID.alive grew in the last ten minutes, which the
# part's script on Windows and beat make it do. The caller has stopped every
# earlier attempt. Return once the runner has started the launcher or decided
# to start nothing, or after half a minute; fail when the copy could not be
# made.
claim_start()
{
    c=$wlock/$1.$2.$3
    {
        echo "mkdir -p $wlock && (set -C && cat > $c.pl) << 'END_OF_CLAIM' || exit 1"
        cat "$repo/tools/flow/claim.pl"
        echo "END_OF_CLAIM"
        echo "cd /tmp && setsid nohup perl $c.pl run $wlock $wheld $1 $2 $3 env $4 \\"
        echo "    perl $c.pl hold $wlock $wheld $1 $2 $3 $5 > $c.out 2>&1 < /dev/null &"
        echo "i=0; until [ -e $c.go ] || [ -e $c.none ] || [ \$i -ge 30 ]; do i=\$((i + 1)); sleep 1; done"
    } | claim_ssh 120
}

# claim_state ID KIND N: print the state of attempt N of the claim ID.KIND
# as claim.pl's state gives it: `granted`, `refused`, `unstarted`, `dead`, or
# nothing while it waits, while its runner cleans up after its launcher, when
# a lock cannot be probed or the host does not answer within two minutes. On
# `unstarted` or `dead` the caller retires the attempt with claim_stop and
# starts another, three in all.
claim_state()
{
    claim_pl state "$wlock" "$1" "$2" "$3" | claim_ssh 120 2> /dev/null | tr -d '\r'
}

# claim_stop ID KIND [N]: end the claim ID.KIND, every attempt that
# claim_start began, granted, waiting for admission or not yet started, or
# with N its attempt N alone, and confirm that each has left WSL's ledger, as
# claim.pl's stop does: it creates the tombstone, on which each runner sends
# its launcher TERM, each holder ends and a late runner or holder starts
# nothing, waits two and a half minutes at most for each attempt's liveness
# and grant locks to be free, and then has etude status recover the ledger
# and looks up each attempt's launcher there. A free lock does not prove the
# ledger clean, since a restart of WSL leaves no lock and etude's keeper and
# bridge close the descriptor that carries it. Print a line for what stays,
# and fail when an attempt keeps a lock, recovery fails, a launcher stays in
# the ledger, or the host does not answer within five minutes. A stop
# removes nothing, so a confirmed stop is final.
claim_stop()
{
    # shellcheck disable=SC2086 # N is a word or none
    claim_pl stop "$wlock" "$1" "$2" ${3-} | claim_ssh 300 > "$state/$1.$2.stop" 2>&1
    cs_rc=$?
    tr -d '\r' < "$state/$1.$2.stop"
    rm -f "$state/$1.$2.stop"
    case $cs_rc in
        0 | 1) ;;
        *) echo "the testing host did not answer within five minutes, so the end of the claim $1.$2 is not confirmed" ;;
    esac
    return "$cs_rc"
}

# beat ID: append a byte to ID.alive in WSL once a minute, the minute passing
# in WSL. A claim's holder in WSL's admission ends once that file has not
# grown in ten minutes of its looks, so that it lasts as long as the step it
# holds for and ends soon after a release that is gone. Run it as a job that
# held starts, whose hold.pl stops it when the step ends or once the shell
# that started it has gone.
beat()
{
    while :; do
        wsl "printf . >> $wheld/$1.alive; sleep 60" > /dev/null 2>&1 || sleep 60
    done
}

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

# windows_run ID: print the Windows shell's lines that give a session of the
# run ID its run directory, TREE/build/tmp/ID, as ETUDE_RUN_DIR with its
# TMPDIR inside, make it and OUT/ID, and define `kept REASON PATH...`, which
# marks the directory kept with a line of the reason and the evidence.
windows_run()
{
    echo "run=$wtree/build/tmp/$1"
    # shellcheck disable=SC2016 # the remote shell expands these
    echo 'export ETUDE_RUN_DIR=$run TMPDIR=$run/tmp'
    echo "mkdir -p $wout/$1 \$TMPDIR"
    # shellcheck disable=SC2016 # the remote shell expands these
    echo 'kept() { r=$1; shift; echo "failed: $r; evidence: $*" >> "$run/KEPT"; }'
}

# windows_ended ID CONSOLE: print the Windows shell's lines that, once etude's
# status is in rc, mark the run directory kept first when it failed, with the
# run's output OUT/ID and its OUT/ID.CONSOLE as evidence, and remove the
# directory when it passed and etude kept nothing there.
windows_ended()
{
    echo "[ \"\$rc\" = 0 ] || kept \"etude exited \$rc\" $wout/$1 $wout/$1.$2"
    # shellcheck disable=SC2016 # the remote shell expands these
    echo '[ "$rc" != 0 ] || [ -e "$run/KEPT" ] || rm -rf "$run"'
}

# wp_halt: stop windows_prepare's jobs, and, once windows_clean has
# confirmed that its preparation on Windows has stopped or when it never
# started, end its claim; otherwise say in STATE/ID.claim what stays.
wp_halt()
{
    for wh_job in "$beater" "$wd_sleep" "$wd_obs"; do
        [ -z "$wh_job" ] || job_ask "$wh_job"
    done
    for wh_job in "$beater" "$wd_sleep" "$wd_obs"; do
        [ -z "$wh_job" ] || job_wait "$wh_job"
    done
    beater='' wd_sleep='' wd_obs=''
    if [ -z "$wstarted" ] || windows_clean "$id"; then
        wsl "touch $wheld/$id.prepared" > /dev/null 2>&1
        claim_stop "$id" claim >> "$state/$id.claim" 2>&1
    else
        echo "release: $(unstopped "$id")" >> "$state/$id.claim"
    fi
}

# windows_prepare ID: build the preparation of Windows' run ID with `etude run
# routine-prepare`, its output in STATE/ID.prepare, while a claim in WSL's
# admission, `etude-launch build` with Windows' JOBS, holds the memory that the
# preparation's own claim on Windows takes, JOBS GiB and one more, and the
# block of ports that WSL's launcher gives each claim, so that WSL's tests
# leave both to Windows; the tooling machine forwards no port. The claim,
# ID.claim, runs as claim_start says and the preparation as windows_detach
# says, so that a dropped link ends neither, and its observer runs in the
# background, so that the wait builtin, which a signal interrupts, takes the
# TERM of a stop at once. The claim ends with the preparation, which touches
# OUT/ID.prepared once the preparation has ended or, when it ended without a
# status, windows_clean has stopped it, or as beat says once both the release
# and the preparation are gone. A stop stops its jobs, stops the
# preparation on Windows with windows_clean once it has started, and then
# ends the claim with claim_stop, whether it was granted or still waits, so
# that WSL admits other work only once Windows has stopped. When windows_clean
# cannot confirm that, OUT/ID.prepared stays missing and the claim lasts
# until OUT/ID.alive, which the preparation appends to, has not grown for ten
# minutes. A job directory that cannot be made ends the preparation as a
# stop does, without a status.
# STATE/ID.claim gets the claim's output. Run in the background.
windows_prepare()
{
    id=$1 beater='' wd_sleep='' wd_obs='' wstarted='' wp_job=''
    trap 'wp_halt; exit 143' TERM
    tries=1
    claim_start "$id" claim "$tries" "JOBS=$wjobs /usr/local/libexec/rach2/etude-launch build env" \
        "$id.prepared" > "$state/$id.claim" 2>&1
    while :; do
        case $(claim_state "$id" claim "$tries") in
            granted) break ;;
            refused)
                wsl "cat $wlock/$id.claim.$tries.out" >> "$state/$id.claim" 2>&1
                echo "claim refused: $(tail -n 3 "$state/$id.claim" | tr '\n' ' ')" > "$state/$id.prepare"
                return 1 ;;
            unstarted | dead)
                # The attempt's tombstone exists and its locks are free
                # before another attempt starts.
                if ! claim_stop "$id" claim "$tries" >> "$state/$id.claim" 2>&1 || [ "$tries" -ge 3 ]; then
                    echo "claim refused: its script did not start or hold in WSL in $tries tries" > "$state/$id.prepare"
                    return 1
                fi
                tries=$((tries + 1))
                claim_start "$id" claim "$tries" "JOBS=$wjobs /usr/local/libexec/rach2/etude-launch build env" \
                    "$id.prepared" >> "$state/$id.claim" 2>&1
                continue ;;
        esac
        wp_job=$(job_dir sleep) || { wp_halt; return 1; }
        held "$wp_job" sleep 30 &
        wd_sleep="$! $wp_job"
        job_wait "$wd_sleep"
        wd_sleep=
    done
    wp_job=$(job_dir beat) || { wp_halt; return 1; }
    held "$wp_job" beat "$id" &
    beater="$! $wp_job"
    wstarted=1
    wp_obs=$(job_dir observer) || { wp_halt; return 1; }
    # shellcheck disable=SC2086 # the suites are words
    {
        echo "cd $wtree || { echo 'status 1'; exit; }"
        echo "git fetch -q $wout/release.bundle 'refs/remotes/origin/$branch' && git checkout -q -f --detach $sha || { echo 'status 1'; exit; }"
        downloads
        environment windows
        windows_run "$id"
        echo "ETUDE_SITE=$wsite make build/tools/etude/etude > $wout/$id.make.log 2>&1 || { kept 'etude did not build' $wout/$id.make.log; tail -n 20 $wout/$id.make.log; echo 'status 1'; exit; }"
        echo "e=$wout/etude-$id.exe"
        echo "cp build/tools/etude/etude \"\$e\" || { kept 'etude could not be copied'; echo 'status 1'; exit; }"
        echo "unset ETUDE_SITE ETUDE_ADMITTED ETUDE_PREPARATION_CLEAN"
        echo "ETUDE_SITE=$wsite \"\$e\" run -N -t $wtree -a $warch -A $waccel -J $wout/$id.selection.json $wwords > $wout/$id.selection.log 2>&1 || { kept 'the selection failed' $wout/$id.selection.log; tail -n 20 $wout/$id.selection.log; echo 'status 1'; exit; }"
        echo "ETUDE_PHASE_SELECTION_EXPORT=$wout/$id.selection.json \"\$e\" run routine-prepare phase_first_run $wtree $wout/$id $warch $id $wsite \"\$e\" $waccel $wwords > $wout/$id.prepare.console 2>&1"
        # shellcheck disable=SC2016 # the remote shell expands these
        echo 'rc=$?'
        windows_ended "$id" prepare.console
        echo "tail -n 40 $wout/$id.prepare.console"
        # shellcheck disable=SC2016 # the remote shell expands these
        echo 'echo "status $rc"'
    } | held "$wp_obs" windows_detach "$id" prepare "$state/$id.prepare" "$id.prepare.console" &
    wd_obs="$! $wp_obs"
    job_wait "$wd_obs"
    wd_obs=
    job_stop "$beater"
    beater=
    if ! grep -q '^status ' "$state/$id.prepare" 2> /dev/null && ! windows_clean "$id"; then
        echo "release: $(unstopped "$id")" >> "$state/$id.claim"
        return 1
    fi
    wsl "touch $wheld/$id.prepared" > /dev/null 2>&1
    i=0
    until wsl "test -e $wlock/$id.claim.$tries.rc" 2> /dev/null || [ "$i" -ge 12 ]; do
        i=$((i + 1))
        sleep 5
    done
    wsl "cat $wlock/$id.claim.$tries.out" >> "$state/$id.claim" 2>&1
    [ "$i" -lt 12 ] ||
        echo "release: the claim had not ended a minute after the preparation" >> "$state/$id.claim"
    claim_stop "$id" claim >> "$state/$id.claim" 2>&1
}

# unstopped ID: say that Windows' run ID is not confirmed stopped, so that
# its claims in WSL stay until they end by themselves.
unstopped()
{
    echo "the run $1 is not confirmed stopped on Windows; its claims in WSL stay until its $1.alive has not grown for ten minutes"
}

# windows_gate ID: let Windows' run ID alone start in Windows' copy, and say
# why the release cannot go on now when that is not confirmed. With
# `winclean.sh -g` it writes ID to OUT/windows.current and stops each other
# run of OUT that may still run, as winclean.sh says. A part's start writes
# its token before it reads OUT/windows.current, so a start of an earlier run
# that the gate does not list, as one still on its way leaves it, reads ID
# and starts nothing; the gate needs no list of the runs that were started.
windows_gate()
{
    wg_out=$(winclean -g "$1" 2> /dev/null)
    wg_rc=$?
    wg_ids=$(printf '%s\n' "$wg_out" | tr -d '\r' | sed -n 's/^left //p' | sort -u | tr '\n' ' ' |
        sed 's/ $//')
    if [ -n "$wg_ids" ]; then
        later "the Windows runs $wg_ids are not confirmed stopped there, so no other starts in Windows' copy"
    fi
    [ "$wg_rc" = 0 ] ||
        later "the testing host did not confirm that no other Windows run runs in Windows' copy"
}

# windows_words: set wwords to the words of Windows' run, as `selection
# windows` prints them, and warch to the architectures they name, so that a
# rerun of one architecture's suites does not prepare the other's collection.
windows_words()
{
    wwords=$(selection windows)
    warch=both
    case " $wwords" in *" x86_64:"*) ;; *) warch=i686 ;; esac
    case " $wwords" in *" i686:"*) ;; *) warch=x86_64 ;; esac
}

# windows_bundle: put a bundle of M's branch on the testing host, where
# Windows' copy fetches SHA from it.
windows_bundle()
{
    if ! net git -C "$repo" fetch -q origin ||
        ! git -C "$repo" bundle create "$state/windows.bundle" "refs/remotes/origin/$branch" > /dev/null 2>&1; then
        later "the bundle for Windows could not be made"
    fi
    scp -q -o BatchMode=yes "$state/windows.bundle" "testing:$wout/release.bundle" ||
        later "the bundle could not be copied to the testing host"
}

# windows_start: start Windows' run of the words that `selection windows`
# prints, its preparation in the background, which windows_stop stops when the
# release exits first, once windows_gate has confirmed that no earlier run
# still runs in Windows' copy. Mark the run done when only style failed, and
# record its failure when three runs have started; return 1 in both cases.
windows_start()
{
    windows_settings
    windows_words
    if [ -z "$wwords" ]; then
        mark windows "only style failed, which the full run checks"
        return 1
    fi
    # The first run of a process that -a started is its own, which -a has
    # recorded.
    if [ -n "$wgiven" ]; then
        wid=$wgiven
    else
        if [ "$(starts windows)" -ge 3 ]; then
            failure windows "$(sed -n 's/^windows started //p' "$state/record" | tail -n 1)" runner
            return 1
        fi
        wid=milestone$milestone-$(echo "$sha" | cut -c 1-12)-windows-$(stamp)
    fi
    windows_gate "$wid"
    windows_bundle
    [ -n "$wgiven" ] || note "windows started $wid"
    wgiven=
    trap windows_stop EXIT
    trap 'windows_stop; trap - EXIT TERM; kill -TERM $$' TERM
    trap 'windows_stop; trap - EXIT INT; kill -INT $$' INT
    trap 'windows_stop; trap - EXIT HUP; kill -HUP $$' HUP
    ws_job=$(job_dir prepare) || later "no directory for a job under $state/jobs"
    held "$ws_job" windows_prepare "$wid" > /dev/null 2>&1 9>&- &
    wprep="$! $ws_job"
    say "windows: preparing the run $wid"
}

# windows: finish Windows' run on the testing host, which windows_start began:
# once its preparation has passed, run its cases with `etude run
# routine-collect` while a claim of the whole host in WSL's admission keeps
# WSL's tests from starting, and mark it done or record its failure. A run
# whose preparation or cases end without a status runs again, three runs in
# all, once windows_gate has confirmed that it stopped.
windows()
{
    step=windows
    windows_settings
    while :; do
        if [ -z "$wprep" ]; then
            windows_start || return 0
        fi
        say "windows: waiting for the preparation of $wid"
        job_wait "$wprep"
        wprep='' id=$wid
        grep -q '^claim refused: ' "$state/$id.prepare" 2> /dev/null &&
            fail windows "WSL's admission did not give the preparation's claim: $(sed -n 's/^claim refused: //p' "$state/$id.prepare")"
        rc=$(tr -d '\r' < "$state/$id.prepare" 2> /dev/null | sed -n 's/^status //p' | tail -n 1)
        if [ -z "$rc" ]; then
            say "windows: the preparation of $id ended without a status: $(lost "$state/$id.prepare")"
            continue
        fi
        if [ "$rc" != 0 ]; then
            failure windows "$id" runner
            return 0
        fi
        wholding=1
        tries=1
        claim_start "$id" hold "$tries" "/usr/local/libexec/rach2/etude-launch alone" "$id.end" \
            > "$state/$id.hold" 2>&1
        while :; do
            case $(claim_state "$id" hold "$tries") in
                granted) break ;;
                refused)
                    wsl "cat $wlock/$id.hold.$tries.out" >> "$state/$id.hold" 2>&1
                    fail windows "WSL's admission did not give the whole host: $(tail -n 3 "$state/$id.hold" | tr '\n' ' ')" ;;
                unstarted | dead)
                    # As windows_prepare's.
                    claim_stop "$id" hold "$tries" >> "$state/$id.hold" 2>&1 ||
                        fail windows "the whole host's claim did not start or hold in WSL, and its end is not confirmed"
                    [ "$tries" -lt 3 ] ||
                        fail windows "the whole host's claim did not start or hold in WSL in three tries"
                    tries=$((tries + 1))
                    claim_start "$id" hold "$tries" "/usr/local/libexec/rach2/etude-launch alone" \
                        "$id.end" >> "$state/$id.hold" 2>&1
                    continue ;;
            esac
            say "windows: prepared; waiting for WSL's tests to end"
            sleep 30
        done
        say "windows: the cases of the run $id start"
        ws_job=$(job_dir beat) || later "no directory for a job under $state/jobs"
        held "$ws_job" beat "$id" 9>&- &
        wbeat="$! $ws_job"
        ws_job=$(job_dir observer) || later "no directory for a job under $state/jobs"
        # shellcheck disable=SC2086 # the suites are words
        {
            echo "cd $wtree || { echo 'status 1'; exit; }"
            environment windows
            windows_run "$id"
            echo "MSYS_NO_PATHCONV=1 wsl.exe -u root -e sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' < /dev/null"
            echo "e=$wout/etude-$id.exe"
            echo "unset ETUDE_SITE ETUDE_ADMITTED ETUDE_PREPARATION_CLEAN"
            echo "ETUDE_PHASE_SELECTION_EXPORT=$wout/$id.selection.json \"\$e\" run routine-collect phase_first_run $wtree $wout/$id $warch $id $wsite \"\$e\" $waccel $wwords > $wout/$id.console 2>&1"
            # shellcheck disable=SC2016 # the remote shell expands these
            echo 'rc=$?'
            windows_ended "$id" console
            echo "tail -n 40 $wout/$id.console"
            # The lines that failed_in reads, wherever they are in the console.
            echo "grep -aE '^run: (result: failed: |(i686|x86_64):[a-z0-9-]+: ([^:]+: (failure|incomplete): |FAIL in |INCOMPLETE in ))' $wout/$id.console"
            # shellcheck disable=SC2016 # the remote shell expands these
            echo 'echo "status $rc"'
        } | held "$ws_job" windows_detach "$id" cases "$state/$id.log" "$id.console" 9>&- &
        wobs="$! $ws_job"
        job_wait "$wobs"
        wobs=
        job_stop "$wbeat"
        wbeat=
        rc=$(tr -d '\r' < "$state/$id.log" | sed -n 's/^status //p' | tail -n 1)
        if [ -z "$rc" ] && ! windows_clean "$id"; then
            wholding=
            say "windows: $(unstopped "$id")"
        else
            windows_unhold "$id" "$tries"
        fi
        [ -z "$rc" ] || break
        say "windows: the run $id ended without a status: $(lost "$state/$id.log")"
    done
    if [ "$rc" = 0 ]; then
        mark windows "run $id"
        return 0
    fi
    # shellcheck disable=SC2046 # the suites are words
    failure windows "$id" $(failed_in "$state/$id.log" | grep . || echo runner)
}

# windows_unhold ID N: end the whole host's claim ID.hold, granted to attempt
# N, once Windows' cases have stopped, appending its output to STATE/ID.hold,
# and then end it with claim_stop, which confirms its end and removes its
# lock, and stops it when it has not ended a minute later.
windows_unhold()
{
    wsl "touch $wheld/$1.end" > /dev/null 2>&1
    i=0
    until wsl "test -e $wlock/$1.hold.$2.rc" 2> /dev/null || [ "$i" -ge 12 ]; do
        i=$((i + 1))
        sleep 5
    done
    wsl "cat $wlock/$1.hold.$2.out" >> "$state/$1.hold" 2>&1
    [ "$i" -lt 12 ] || say "windows: the whole host's claim had not ended a minute after the cases"
    claim_stop "$1" hold | while read -r line; do say "windows: $line"; done
    wholding=
}

# windows_detached: succeed when the record's last Windows run is one that -a
# started and its hold.pl, in the run directory that STATE/windows.run names,
# still holds it.
windows_detached()
{
    case $(latest windows) in
        "windows started "*) ;;
        *) return 1 ;;
    esac
    [ -n "$(windows_runner)" ]
}

# windows_wait: wait for the Windows run that -a started to end, and start
# Windows' run here when its process ended without recording a result.
windows_wait()
{
    while windows_detached; do
        say "windows: the run $(sed -n 's/^windows started //p' "$state/record" | tail -n 1) of -a runs"
        sleep 60
    done
    windows_forget
    if ! done_ windows && ! failed_ windows; then
        say "windows: the run of -a ended without a result; starting another"
        windows
    fi
}

# windows_halt: stop the Windows run that -a started, with its process group,
# when the record no longer waits for it, through the hold.pl that leads the
# group, which sends it TERM once the stop of its run directory, which
# STATE/windows.run names, exists, and KILL an hour later.
windows_halt()
{
    [ -f "$state/windows.run" ] && ! windows_detached || return 0
    wh_dir=$(windows_runner)
    if [ -n "$wh_dir" ]; then
        wh_state=$(perl "$repo/tools/flow/hold.pl" stop "$wh_dir" 300) ||
            later "the Windows run of -a in $wh_dir did not stop: $wh_state"
        say "windows: stopped the run of -a in $wh_dir"
    fi
    windows_forget
}

# windows_forget: remove STATE/windows.run, and the run directory it names
# once that run's hold.pl has ended; say so when the holder ended without its
# status, since what it held may then still run, as stop.sh lists it.
windows_forget()
{
    wf_dir=$(cat "$state/windows.run" 2> /dev/null)
    [ -z "$wf_dir" ] || held_gone "$wf_dir" ||
        say "windows: the holder of the run of -a in $wf_dir ended without its status; what it held may still run"
    rm -f "$state/windows.run"
}

# screen_free: stop the trial boot's console session, `term`, which holds the
# framebuffer, and set `login` to `PID COMM BOOT`, the process of the kernel
# console's login that init starts in its place, its name as its stat gives
# it, and the boot's /proc/sys/kernel/random/boot_id; leave `login` empty when
# no `term` runs. The look and the TERM to `term` are one script on the
# machine. Rach2 gives no process number twice within a boot before two
# thousand million processes, its pid_next only counting up
# (rach2/sys/proc/Readme.md), so a number with the boot's ID names one
# process.
screen_free()
{
    # shellcheck disable=SC2016 # the script expands on the Lenovo
    login=$(lenovo 'kids() { for p in /proc/[0-9]*; do
            read -r pid comm s ppid r < $p/stat 2> /dev/null && [ "$ppid" = 1 ] && echo "$pid $comm"; done; }
        t=$(kids | sed -n "s/ (term)\$//p")
        [ -n "$t" ] || exit 0
        read -r b < /proc/sys/kernel/random/boot_id || exit 1
        before=" $(kids | cut -d " " -f 1 | tr "\n" " ")"
        kill $t || exit 1
        n=0
        while [ $n -lt 20 ]; do
            sleep 1
            n=$((n + 1))
            [ ! -d /proc/$t ] || continue
            new=$(kids | while read -r k c; do
                case $before in *" $k "*) ;; *) echo "$k $c"; break ;; esac
            done)
            [ -z "$new" ] || { echo "$new $b"; exit 0; }
        done
        exit 1') || fail lenovo "the console's term would not give up the screen"
    [ -z "$login" ] || say "lenovo: stopped the console's term for the run; login ${login%% *} stands in"
}

# screen_back: end the kernel console's login that screen_free left, so that
# init starts `term` again: HUP, in one script on the machine, only while the
# boot's ID is the one screen_free read and the process with that number is
# init's child with the name screen_free read.
screen_back()
{
    [ -n "$login" ] || return 0
    # shellcheck disable=SC2086 # the login is three words
    set -- $login
    lenovo "read -r b < /proc/sys/kernel/random/boot_id && [ \"\$b\" = '$3' ] &&
        read -r pid comm s ppid r < /proc/$1/stat 2> /dev/null && [ \"\$ppid\" = 1 ] &&
        [ \"\$comm\" = '$2' ] && kill -HUP $1" > /dev/null 2>&1 ||
        say "lenovo: the console's login $1 was gone, or the machine had booted again"
    login=''
}

# route_address HOST: print this computer's address on the interface that
# reaches HOST, which DHCP may change between releases, or nothing.
route_address()
{
    route -n get "$1" 2> /dev/null | awk '$1 == "interface:" { print $2 }' |
        while read -r interface; do ipconfig getifaddr "$interface"; done
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
        # native-deploy refuses a run's name past 48 characters.
        id=milestone$milestone-$(echo "$sha" | cut -c 1-12)-stage-$(date -u '+%m%d-%H%M%S')
        build/tools/dev/native-deploy stage "$files/../tests/native/prepared" "$address" candidate \
            "$files/../rach2/kernel" "$repo/krffs/rach2_demo_ed25519" "$target" "$id" \
            "$state/$id" > "$state/$id.out" 2>&1 ||
            fail lenovo "the native cases' payload could not be staged: $(tail -n 3 "$state/$id.out" | tr '\n' ' ')"
        say "lenovo: staged $staged"
    fi
    screen_free
    peer=$(setting lenovo peer)
    [ "$peer" != route ] || peer=$(route_address "$address")
    [ -n "$peer" ] || fail lenovo "this computer has no address on the way to $address"
    id=milestone$milestone-$(echo "$sha" | cut -c 1-12)-lenovo-$(date -u '+%m%d-%H%M%S')
    note "lenovo started $id"
    # shellcheck disable=SC2086 # the cases are words
    if build/tools/dev/native-deploy run "$files/../tests/native/prepared" "$address" candidate \
        "$files/../rach2/kernel" "$repo/krffs/rach2_demo_ed25519" "$target" "$id" \
        "$state/$id" --hostpeer build/tools/dev/native-hostpeer --peer "$peer" \
        --echo-port "$(setting lenovo echo-port)" --network "$(setting lenovo network)" \
        --interface "$(setting lenovo interface)" --gateway "$(setting lenovo gateway)" \
        --far "$(setting lenovo far)" --empty "$(setting lenovo empty)" \
        --silent "$(setting lenovo silent)" --purpose phase_first_run -- $words \
        > "$state/$id.out" 2>&1; then
        screen_back
        mark lenovo "run $id"
        return 0
    fi
    screen_back
    collection=''
    for f in "$state/$id"/collection-*.out; do
        [ ! -f "$f" ] || collection=$f
    done
    # shellcheck disable=SC2046 # the cases are words
    failure lenovo "$id" $({ [ -z "$collection" ] ||
        sed -n 's/^native: \([^:]*\): \([a-z]*\).*/\1 \2/p' "$collection"; } |
        awk '$1 != "result" && $2 != "pass" { print $1 }' | grep . || echo runner)
}

# failing FILE: print, indented, the lines of FILE that say what failed, or
# its last twenty lines when none does.
failing()
{
    # A parallel make prints other jobs' configure noise after the error, so
    # the twelve lines before the first make error come first.
    { tr -d '\r' < "$1" | awk '
        { line[NR] = $0 }
        !first && /make(\[[0-9]+\])?: \*\*\* \[/ { first = NR }
        END {
            if (first)
                for (i = (first > 12 ? first - 12 : 1); i <= first; i++)
                    print line[i]
        }'
        tr -d '\r' < "$1" |
            grep -E 'error:|Error [0-9]|FAIL|result: fail|step: .*failed|refused|No such' |
            tail -n 8 | grep . || tr -d '\r' < "$1" | tail -n 20; } 2> /dev/null | sed 's/^/    /'
}

# prepare_wsl: build SHA for both architectures, as the full run does, and run
# the style suite and the host tests of both, in a copy on the testing host
# that the worktree STATE/prepare/wt makes new. Run in the background; it
# ends as testing.sh, whose status is the command's.
prepare_wsl()
{
    cd "$pdir/wt" || exit 1
    RACH2_TESTING_COPY=$PREPARE_COPY
    export RACH2_TESTING_COPY
    exec sh tools/dev/testing.sh run "$WAD && ARCH=i686 make && ARCH=x86_64 make &&
        ARCH='i686 x86_64' tests/run style host"
}

# prepare_host: stop what of prepare_wsl still runs in its copy on the
# testing host, and remove the copy once that is confirmed, as root in WSL,
# which reads every process's directory. Its build and suites run under the
# copy's hold.pl in a run directory under the copy's build/held, as
# testing.sh says, which holds everything they started: each such run is
# stopped through its directory's stop, five minutes at most. Then procs.pl,
# whose scan fails rather than read part of /proc, lists and stops through a
# pidfd each other process whose directory is in the copy, as the sync lock's
# shell of a lost connection may be, and waits a minute for them to end.
# Once something was stopped, etude status recovers the admission ledger, a
# minute at most, which drops a dead owner, and each process that procs.pl
# stopped is looked up there by its number and its start, as the ledger
# keeps them. The copy is removed only when every run there has ended, the
# scan succeeded and nothing is left in the copy; a line says what stays.
# The whole is bounded at fifteen minutes.
prepare_host()
{
    {
        echo "copy=$PREPARE_COPY"
        echo "hold() { perl - \"\$@\" << 'END_OF_HOLD'"
        cat "$repo/tools/flow/hold.pl"
        echo 'END_OF_HOLD'
        echo '}'
        echo "procs() { perl - \"\$@\" << 'END_OF_PROCS'"
        cat "$repo/tools/flow/procs.pl"
        echo 'END_OF_PROCS'
        echo '}'
        cat << 'END'
ledger=/var/lib/rach2-etude/ledger
[ -d "$copy" ] || exit 0
left='' stopped=''
for h in "$copy"/build/held/*; do
    [ -d "$h" ] || continue
    case $(hold state "$h") in "ended "*) continue ;; esac
    stopped=1
    s=$(hold stop "$h" 300) || { echo "the run in $h is not confirmed stopped: $s"; left=1; }
done
if ! out=$(procs -k -d "$copy"); then
    echo "the processes in $copy could not be read"
    left=1
elif [ -n "$out" ]; then
    stopped=1
    i=0
    while now=$(procs -d "$copy") && [ -n "$now" ] && [ "$i" -lt 30 ]; do i=$((i + 1)); sleep 2; done
    if ! now=$(procs -d "$copy") || [ -n "$now" ]; then
        echo "processes in $copy did not end: $(printf '%s\n' "$now" | cut -d ' ' -f 1 | tr '\n' ' ')"
        left=1
    fi
fi
if [ -n "$stopped" ]; then
    timeout 60 /usr/local/libexec/rach2/etude status "$ledger" > /dev/null 2>&1 ||
        echo "etude status did not recover $ledger/claims"
    printf '%s\n' "$out" | while read -r p st rest; do
        [ -n "$p" ] || continue
        ! grep -Eq "^owner $p ($st |[0-9]+ 0\$)" "$ledger/claims" 2> /dev/null ||
            echo "the stopped process $p keeps a claim in $ledger/claims; a person checks it and removes it"
    done
fi
if [ -n "$left" ]; then
    echo "the copy $copy stays"
    exit 1
fi
rm -rf "$copy"
END
    } | perl "$repo/tools/flow/bound.pl" 900 ssh -o BatchMode=yes -o ConnectTimeout=10 \
        -o ServerAliveInterval=15 -o ServerAliveCountMax=4 testing 'MSYS_NO_PATHCONV=1 wsl.exe -u root -- sh -s'
}

# prepare_windows_clean: stop Windows' preparation pwid where it still runs on
# Windows, which a stopped ssh leaves running, and once windows_clean has
# confirmed that it has stopped, end its claim in WSL, and remove its files
# there once claim_stop has confirmed that the claim and any request of it
# have left WSL's ledger; otherwise say what stays and keep them. The tokens
# OUT/ID.PART.token, which say `stop`, and the stopped files stay, so that a
# start still on its way starts nothing and no gate lists the run, until
# sweep.sh removes them with the milestone's other files there.
prepare_windows_clean()
{
    if ! windows_clean "$pwid"; then
        say "prepare: windows: $(unstopped "$pwid"), and its files in $wout stay"
        return
    fi
    if pw_left=$(claim_stop "$pwid" claim); then
        echo "cd $wout && rm -rf $pwid etude-$pwid.exe \$(ls -d $pwid.* | grep -v -e '[.]token\$' -e '[.]stopped\$')" |
            tssh 'C:/msys64/usr/bin/bash.exe -l -s' > /dev/null 2>&1
    else
        printf '%s\n' "$pw_left" | while read -r line; do say "prepare: windows: $line"; done
        say "prepare: windows: the files of $pwid in $wout stay"
    fi
}

# prepare_stop: stop each part of -p that still runs, check that none keeps a
# claim, and remove what the parts made. The EXIT trap of -p, which a signal
# reaches too.
prepare_stop()
{
    for px_job in "$pwsl" "$pwin"; do
        [ -z "$px_job" ] || job_ask "$px_job"
    done
    if [ -n "$pwsl" ]; then
        job_wait "$pwsl"
        pwsl=
    fi
    if [ -n "$pwin" ]; then
        job_wait "$pwin"
        pwin=
    fi
    if [ -n "$pmid" ]; then
        # The hold.pl that leads the group sends it TERM, KILL half an hour
        # later, and ends once nothing else is in it.
        if ! pm_state=$(perl "$repo/tools/flow/hold.pl" stop "$pmdir" 300) || ! held_gone "$pmdir"; then
            say "prepare: mac: the preparation $pmid is not confirmed stopped: $pm_state; its output stays, and a person stops what stop.sh lists"
        else
            rm -f "$macdir/run"
            mac_clean "$pmid"
            rm -rf "${runs:?}/$pmid" "$runs/$pmid".*
        fi
        ! grep -qF "$pmid" "$site/ledger/claims" 2> /dev/null ||
            say "prepare: mac: $pmid keeps a claim in $site/ledger/claims; a person checks it and removes it"
        pmac='' pmid=''
    fi
    if [ -n "$pwid" ]; then
        prepare_windows_clean
        rm -f "$state/$pwid".*
        pwid=
    fi
    if [ -n "$pclean" ]; then
        prepare_host 2>&1 | while read -r line; do say "prepare: wsl: $line"; done
        git -C "$repo" worktree remove --force "$pdir/wt" 2> /dev/null
        rm -rf "$pdir/wt"
        git -C "$repo" worktree prune
        pclean=
    fi
}

# prepare_report MACHINE STATUS LOG: report the part MACHINE of -p, which
# ended with STATUS and whose whole output LOG keeps.
prepare_report()
{
    if [ "$2" = 0 ]; then
        say "prepare: $1: passed"
        return 0
    fi
    say "prepare: $1: failed; $3 has its output:"
    failing "$3" | tee -a "$log"
    pfailed="$pfailed $1"
}

# prepare_all: make the release's preparation of SHA on each machine, and the
# end checks of the milestone in a clean copy, as the header's -p says.
prepare_all()
{
    pdir=$state/prepare log=$pdir/log
    pwsl='' pwin='' pmac='' pwid='' pmid='' pmdir='' pclean='' pfailed=''
    mkdir -p "$pdir"
    : > "$log"
    check
    if [ -f "$macdir/run" ] && mac_live "$(cut -d ' ' -f 1 "$macdir/run")"; then
        later "a release's run on the Mac, $(cut -d ' ' -f 2 "$macdir/run"), still runs"
    fi
    # -a starts runs on the machines that -p prepares, in the same copies.
    exec 8>> "$state/again.lock"
    perl -MFcntl=:flock -e 'open(L, ">&=8") && flock(L, LOCK_EX | LOCK_NB) or exit 1' ||
        later "release.sh -a is starting a run"
    pw_dir=$(windows_runner)
    [ -z "$pw_dir" ] || later "a Windows run of release.sh -a, in $pw_dir, still runs"
    trap prepare_stop EXIT
    trap 'prepare_stop; trap - EXIT TERM; kill -TERM $$' TERM
    trap 'prepare_stop; trap - EXIT INT; kill -INT $$' INT
    trap 'prepare_stop; trap - EXIT HUP; kill -HUP $$' HUP
    short=$(echo "$sha" | cut -c 1-12)
    say "prepare: milestone $milestone: preparing $sha on each machine"
    if has windows; then
        windows_settings
        windows_words
        pwid=milestone$milestone-$short-windows-prepare-$(stamp)
        windows_gate "$pwid"
        windows_bundle
        ps_job=$(job_dir prepare) || later "no directory for a job under $state/jobs"
        held "$ps_job" windows_prepare "$pwid" > /dev/null 2>&1 9>&- &
        pwin="$! $ps_job"
        say "prepare: windows: preparing $pwid"
    fi
    if has mac; then
        [ -d "$tree" ] && [ -f "$site/etude-site" ] ||
            later "the Mac has no copy at $tree or no admission site at $site"
        pmid=milestone$milestone-$short-mac-prepare-$(stamp)
        mkdir -p "$runs/$pmid"
        pmdir=$macdir/held/$pmid
        { mkdir -p "$macdir/held" && mkdir "$pmdir"; } || later "$pmdir cannot be made"
        { echo "$pmdir $pmid" > "$macdir/run.new" && mv "$macdir/run.new" "$macdir/run"; } ||
            later "$macdir/run cannot be written"
        perl "$repo/tools/flow/hold.pl" run -g -k 1800 "$pmdir" sh "$repo/tools/flow/release.sh" -s "$state" \
            -p -m "$pmid" "$milestone" "$sha" > /dev/null 2>&1 < /dev/null 8>&- 9>&- &
        pmac=$!
        say "prepare: mac: preparing $pmid"
    fi
    pclean=1
    prepare_host > "$pdir/wsl.log" 2>&1
    git -C "$repo" worktree remove --force "$pdir/wt" 2> /dev/null
    rm -rf "$pdir/wt"
    git -C "$repo" worktree prune
    if git -C "$repo" worktree add -q --detach "$pdir/wt" "$sha" >> "$pdir/wsl.log" 2>&1; then
        ps_job=$(job_dir wsl) || later "no directory for a job under $state/jobs"
        held "$ps_job" prepare_wsl >> "$pdir/wsl.log" 2>&1 9>&- &
        pwsl="$! $ps_job"
        say "prepare: wsl: building in $PREPARE_COPY"
        job_wait "$pwsl"
        rc=$?
        pwsl=
    else
        rc=1
    fi
    prepare_report wsl "$rc" "$pdir/wsl.log"
    if [ -n "$pwin" ]; then
        job_wait "$pwin"
        pwin=
        rc=$(tr -d '\r' < "$state/$pwid.prepare" 2> /dev/null | sed -n 's/^status //p' | tail -n 1)
        {
            cat "$state/$pwid.claim" "$state/$pwid.prepare"
            echo "cd $wout && cat $pwid.make.log $pwid.selection.log $pwid.prepare.console" |
                tssh 'C:/msys64/usr/bin/bash.exe -l -s'
        } 2> /dev/null | tr -d '\r' > "$pdir/windows.log"
        [ -n "$rc" ] || say "prepare: windows: ended without a status: $(lost "$state/$pwid.prepare")"
        rm -f "$state/$pwid.prepare" "$state/$pwid.claim"
        prepare_report windows "${rc:-1}" "$pdir/windows.log"
    fi
    if [ -n "$pmac" ]; then
        wait "$pmac"
        pmac=
        cp "$runs/$pmid.console" "$pdir/mac.log" 2> /dev/null || : > "$pdir/mac.log"
        rc=$(sed -n 's/^rc=//p' "$runs/$pmid.times" 2> /dev/null)
        prepare_report mac "${rc:-1}" "$pdir/mac.log"
    fi
    prepare_stop
    trap - EXIT TERM INT HUP
    if [ -n "$pfailed" ]; then
        finish "prepare: failed:$pfailed"
        exit 1
    fi
    finish "prepare: passed $sha"
    exit 0
}

# check: fetch, and refuse SHA unless milestone M is open, its description
# names its branch, and SHA is on that branch and, but with -p, contains
# origin/master; set `branch`.
check()
{
    net git -C "$repo" fetch -q origin || later "the fetch failed"
    branch=$(net sh "$repo/tools/flow/board.sh" milestone "$milestone")
    case $? in
        0) ;;
        3) refuse "milestone $milestone is not open or names no branch" ;;
        *) later "milestone $milestone cannot be read" ;;
    esac
    git -C "$repo" merge-base --is-ancestor "$sha" "refs/remotes/origin/$branch" 2> /dev/null ||
        refuse "$sha is not on $branch"
    [ -n "$prepare" ] ||
        git -C "$repo" merge-base --is-ancestor refs/remotes/origin/master "$sha" 2> /dev/null ||
        refuse "$sha does not contain origin/master"
    for n in $also; do
        b=$(net sh "$repo/tools/flow/board.sh" milestone "$n")
        case $? in
            0) ;;
            3) refuse "-c: milestone $n is not open or names no branch" ;;
            *) later "milestone $n cannot be read" ;;
        esac
        [ "$b" = "$branch" ] ||
            git -C "$repo" merge-base --is-ancestor "refs/remotes/origin/$b" "$sha" 2> /dev/null ||
            refuse "-c: $sha does not contain $b"
    done
}

# take_lenovo: take the Lenovo's lock as `release M`, or keep it when this
# release already holds it; exit 75 while another holder has it.
take_lenovo()
{
    if mkdir "$lock" 2> /dev/null; then
        echo "release $milestone" > "$lock/issue" || later "the Lenovo's lock could not be written"
    elif [ "$(cat "$lock/issue" 2> /dev/null)" != "release $milestone" ]; then
        later "the Lenovo's lock is held by $(cat "$lock/issue" 2> /dev/null || echo 'an unnamed holder')"
    fi
    lenovo_mine=1
}

# free_lenovo: remove the Lenovo's lock when this run of the release took it
# or found it its own, so that a second run of M that stops on STATE/lock
# leaves the first one's alone.
free_lenovo()
{
    [ -n "$lenovo_mine" ] || return 0
    [ "$(cat "$lock/issue" 2> /dev/null)" != "release $milestone" ] || rm -rf "$lock"
    lenovo_mine=
}

# enter_lenovo: before the release first touches the Lenovo, exit 75 while
# STATE/hold exists; then take the Lenovo's lock or find it this release's.
enter_lenovo()
{
    grep -q '^touched ' "$state/record" || [ ! -e "$state/hold" ] ||
        later "the Lenovo is held: $(sed -n 1p "$state/hold")"
    take_lenovo
}

# unfailed MACHINE: print why MACHINE's last run in the record cannot start
# again with -a, or nothing when it failed: a failure that the record has, or a
# Mac run that ended with a status other than 0, which the release records
# only once the full run and the build are over.
unfailed()
{
    last=$(latest "$1")
    case $last in
        "failure $1 "*) return 0 ;;
        "$1 done "*) echo "its run passed"; return 0 ;;
        '') echo "it has no run in this release"; return 0 ;;
    esac
    id=${last##* }
    free=
    perl -MFcntl=:flock -e 'open(L, ">>", $ARGV[0]) && flock(L, LOCK_EX | LOCK_NB) or exit 1' \
        "$state/lock" && free=1
    case $1 in
        mac)
            rc=$(sed -n 's/^rc=//p' "$runs/$id.times" 2> /dev/null)
            if mac_alive "$id"; then
                echo "its run $id still runs"
            elif [ "$rc" = 0 ]; then
                echo "its run $id passed"
            elif [ -z "$rc" ] && [ -z "$free" ]; then
                echo "its run $id ended without a status, and the release starts it again"
            fi ;;
        windows)
            if windows_detached; then
                echo "its run $id still runs"
            elif [ -z "$free" ]; then
                echo "its run $id has not ended"
            fi ;;
    esac
}

# restart MACHINE: start MACHINE's run again at SHA, a commit of the
# release's branch that contains the release's own, as the record's run of the
# release under way, as -a does in the header.
restart()
{
    m=$1 at=$sha
    case $m in
        mac|windows) ;;
        *) refuse "-a takes mac or windows, not '$m'" ;;
    esac
    has "$m" || refuse "the machines file has no $m"
    exec 8>> "$state/again.lock"
    perl -MFcntl=:flock -e 'open(L, ">&=8") && flock(L, LOCK_EX | LOCK_NB) or exit 1' ||
        later "another -a starts a run, or -p prepares"
    [ -f "$state/record" ] || refuse "no release is under way in $state"
    grep -qx "milestone $milestone" "$state/record" ||
        refuse "the release under way is not of milestone $milestone"
    ! grep -q '^closing \|^closed ' "$state/record" || refuse "the release under way closes"
    # Master may have moved since the release began; -r at SHA checks it.
    net git -C "$repo" fetch -q origin || later "the fetch failed"
    branch=$(sed -n 's/^branch //p' "$state/record")
    git -C "$repo" merge-base --is-ancestor "$at" "refs/remotes/origin/$branch" 2> /dev/null ||
        refuse "$at is not on $branch"
    sha=$(sed -n 's/^sha //p' "$state/record")
    git -C "$repo" merge-base --is-ancestor "$sha" "$at" 2> /dev/null ||
        refuse "$at does not contain $sha, the release's commit"
    why=$(unfailed "$m")
    [ -z "$why" ] || refuse "$m: $why"
    if [ "$m" = mac ]; then
        [ -d "$tree" ] && [ -f "$site/etude-site" ] ||
            refuse "the Mac has no copy at $tree or no admission site at $site"
        [ ! -f "$macdir/run" ] || {
            read -r rs_dir last < "$macdir/run"
            ! mac_live "$rs_dir" || refuse "the run $last still runs on the Mac"
            held_gone "$rs_dir" ||
                refuse "the holder of the run $last on the Mac ended without its status, so what it held may still run; a person checks what stop.sh lists"
            [ -z "$last" ] || ! grep -qF "$last" "$site/ledger/claims" 2> /dev/null ||
                refuse "the run $last keeps a claim in $site/ledger/claims; a person checks it and removes it"
        }
    fi
    id=milestone$milestone-$(echo "$at" | cut -c 1-12)-$m-$(stamp)
    # The line comes first, so that a release that waits on the run never
    # takes it for one of its own commit.
    [ "$at" = "$sha" ] || note "failure newer $m $id $at"
    case $m in
        mac) mac_launch "$id" "$at" ;;
        windows)
            note "windows started $id"
            rs_dir=$state/held/$id
            { mkdir -p "$state/held" && mkdir "$rs_dir"; } || later "$rs_dir cannot be made"
            { echo "$rs_dir" > "$state/windows.run.new" && mv "$state/windows.run.new" "$state/windows.run"; } ||
                later "$state/windows.run cannot be written"
            nohup perl "$repo/tools/flow/hold.pl" run -g -k 3600 "$rs_dir" sh "$repo/tools/flow/release.sh" \
                -s "$state" -w "$id" "$milestone" "$at" > /dev/null 2>&1 < /dev/null 8>&- 9>&- &
            say "windows: started the run $id" ;;
    esac
    finish "again: $m: started the run $id at $at"
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

# open_query N: write the numbers of milestone N's open issues, not pull
# requests, to $state/open, afresh at each try, since net keeps no output of
# a failed one.
open_query()
{
    gh api --paginate "repos/$REPO/issues?milestone=$1&state=open&per_page=100" \
        --jq '.[] | select(.pull_request == null) | .number' > "$state/open"
}

# open_issues N: print the open issues of milestone N, as `#A #B`, but for
# the release's own cards, which the close passes after it; fail when they
# cannot be read, or when what came back is not a list of numbers.
open_issues()
{
    net open_query "$1" || return 1
    o=$(cat "$state/open") && rm -f "$state/open" || return 1
    ! printf '%s\n' "$o" | grep -qv '^[0-9]*$' || return 1
    for i in $o; do
        grep -q "^card [a-z]* $i\$" "$state/record" || echo "$i"
    done | sort -n | sed 's/^/#/' | tr '\n' ' ' | sed 's/ $//'
}

# close: move master to SHA, close milestone M and each milestone of the
# record's `also` lines that has no open issue, and archive their cards. A
# milestone with an open issue stays open, and `kept` names it with its open
# issues for the last line; the task that closes its last issue closes it.
close()
{
    net git -C "$repo" fetch -q origin || later "the fetch failed"
    if [ "$(git -C "$repo" rev-parse refs/remotes/origin/master)" != "$sha" ]; then
        net git -C "$repo" push origin "$sha:refs/heads/master" ||
            later "the push of $sha to master failed"
    fi
    names='' kept=''
    for n in $milestone $(sed -n 's/^also //p' "$state/record"); do
        open=$(open_issues "$n") || later "the open issues of milestone $n could not be read"
        if [ -n "$open" ]; then
            kept="$kept${kept:+; }milestone $n stays open for $open"
            continue
        fi
        net gh api -X PATCH "repos/$REPO/milestones/$n" -f state=closed > /dev/null ||
            later "milestone $n could not be closed"
        names="$names${names:+ and }$n"
    done
    case $names in
        '') closed="no milestone is closed" ;;
        *' and '*) closed="milestones $names are closed" ;;
        *) closed="milestone $names is closed" ;;
    esac
    report close pass "Master is at $sha, and $closed${kept:+; $kept}."
    report release pass "Released $sha on every machine."
    for n in $milestone $(sed -n 's/^also //p' "$state/record"); do
        net sh "$repo/tools/flow/board.sh" archive "$n" > /dev/null ||
            later "board.sh archive $n failed"
    done
}

main()
{
    state=
    lock=
    lenovo_mine=
    macrun=
    rerun=
    prepare=
    restart=
    detached=
    also=
    only=
    while getopts a:c:m:oprs:w: o; do
        case $o in
            a) restart=$OPTARG ;;
            c)
                case $OPTARG in
                    ''|*[!0-9]*) echo "$USAGE" >&2; exit 2 ;;
                esac
                also="$also $OPTARG" ;;
            m) macrun=$OPTARG ;;
            o) only=1 ;;
            p) prepare=1 ;;
            r) rerun=1 ;;
            s) state=$OPTARG ;;
            w) detached=$OPTARG ;;
            *) echo "$USAGE" >&2; exit 2 ;;
        esac
    done
    shift $((OPTIND - 1))
    if [ $# -ne 2 ] || { [ -n "$prepare" ] && [ -n "$rerun$also" ]; } ||
        { [ -n "$restart" ] && [ -n "$prepare$rerun$macrun$detached$also$only" ]; }; then
        echo "$USAGE" >&2
        exit 2
    fi
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
    lock=$(cd "$repo" && cd "$(git rev-parse --git-common-dir)/.." && pwd)/build/flow/lenovo ||
        exit 1
    address=${RACH2_MACHINE_ADDRESS:-192.168.0.106}
    mac=${RACH2_MACHINE_MAC:-28:d2:44:7e:e0:af}
    serial=${RACH2_MACHINE_SERIAL:-S6P2NX0T381638Z}
    mkdir -p "$state"
    state=$(cd "$state" && pwd)
    log=$state/log
    [ -z "$prepare" ] || { mkdir -p "$state/prepare"; log=$state/prepare/log; }
    files=$state/wt/$ARCH_DIR/images
    macdir=$repo/build/machines/mac
    tree=$macdir/tree site=$macdir/site runs=$macdir/runs
    if [ -n "$macrun" ]; then
        mac_run "$macrun"
        exit 0
    fi
    [ -f "$repo/tools/flow/machines" ] || refuse "$repo/tools/flow/machines is missing"
    if [ -n "$restart" ]; then
        restart "$restart"
        exit 0
    fi
    wprep='' wobs='' wholding='' wbeat='' wgiven=''
    if [ -n "$detached" ]; then
        branch=$(sed -n 's/^branch //p' "$state/record")
        wgiven=$detached wid=$detached
        trap windows_stop EXIT
        trap 'windows_stop; trap - EXIT TERM; kill -TERM $$' TERM
        windows
        exit 0
    fi
    if [ -z "${RACH2_RELEASE_HELD-}" ]; then
        exec 9>> "$state/lock"
        perl -MFcntl=:flock -e 'open(L, ">&=9") && flock(L, LOCK_EX | LOCK_NB) or exit 1' ||
            later "another release runs"
        # The release goes on as the child of a hold.pl in a run directory
        # of its own, made new, which STATE/run names before the start; the
        # hold.pl sends it TERM once that directory's stop exists, as stop.sh
        # -r asks, and KILL an hour later. Both keep the lock on descriptor 9.
        # The run directories of runs and jobs that have ended go first.
        perl "$repo/tools/flow/hold.pl" sweep "$state/held"
        perl "$repo/tools/flow/hold.pl" sweep "$state/jobs"
        rl_dir=$state/held/release-$(stamp)
        { mkdir -p "$state/held" && mkdir "$rl_dir"; } || later "$rl_dir cannot be made"
        { echo "$rl_dir" > "$state/run.new" && mv "$state/run.new" "$state/run"; } ||
            later "$state/run cannot be written"
        RACH2_RELEASE_HELD=1
        export RACH2_RELEASE_HELD
        rl_also=
        for n in $also; do
            rl_also="$rl_also -c $n"
        done
        # shellcheck disable=SC2086 # the flags are a word each
        exec perl "$repo/tools/flow/hold.pl" run -k 3600 "$rl_dir" sh "$repo/tools/flow/release.sh" \
            ${rerun:+-r} ${only:+-o} ${prepare:+-p} $rl_also -s "$state" "$milestone" "$sha"
    fi
    unset RACH2_RELEASE_HELD
    [ -z "$prepare" ] || prepare_all
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
        record_lock
        [ -z "$rerun" ] || kept=$(again)
        {
            printf 'milestone %s\nsha %s\nbranch %s\nchosen %s\n' "$milestone" "$sha" "$branch" \
                "$(date -u '+%FT%TZ')"
            for n in $also; do printf 'also %s\n' "$n"; done
            [ -z "$only" ] || echo 'only wsl lenovo'
            [ -z "$kept" ] || printf '%s\n' "$kept"
            [ -z "$old" ] || ! grep -qx "milestone $milestone" "$state/record" ||
                grep -q '^closed ' "$state/record" || grep '^card ' "$state/record"
            [ -z "$old" ] || grep -q '^closed ' "$state/record" ||
                grep '^touched ' "$state/record" | tail -n 1
        } > "$state/record.new"
        [ -z "$old" ] || cp "$state/record" "$state/$old-$(date -u '+%m%d-%H%M%S')" ||
            later "the old record could not be kept"
        mv "$state/record.new" "$state/record"
        record_unlock
        windows_halt
        cards
        again=$(sed -n 's/^again //p' "$state/record" | awk '{ printf "%s%s", (NR > 1 ? "; " : ""), $0 }')
        say "milestone $milestone: releasing $sha${again:+, running again only $again}"
    fi
    sha=$(sed -n 's/^sha //p' "$state/record")
    branch=$(sed -n 's/^branch //p' "$state/record")
    chosen=$(sed -n 's/^chosen //p' "$state/record")

    if has mac && ! done_ mac && ! failed_ mac && [ -z "$(latest mac)" ]; then
        mac_start
    fi
    if has windows && ! done_ windows && ! failed_ windows && ! windows_detached; then
        windows_start
    fi
    done_ full || failed_ full || full
    if has windows && ! done_ windows && ! failed_ windows && ! windows_detached; then
        windows
    fi
    if ! runs_failed && ! done_ accept; then
        enter_lenovo
        preflight
    fi
    if ! runs_failed && ! done_ build; then
        ssh -o BatchMode=yes -o ConnectTimeout=10 testing true 2> /dev/null ||
            later "the testing host does not answer"
        build
    fi
    if has mac && ! done_ mac && ! failed_ mac; then
        mac_wait
    fi
    if has windows && windows_detached; then
        windows_wait
    fi
    ! runs_failed || tests_failed
    enter_lenovo
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
                # A reset just after the slots were moved leaves the old root,
                # whose bootctl cannot walk the larger slots; the build's can.
                if ! lenovo 'bootctl trial recovery' > /dev/null; then
                    send "$files/../utils/sbin/bootctl" /dev/shm/bootctl
                    lenovo 'chmod 755 /dev/shm/bootctl && /dev/shm/bootctl trial recovery' > /dev/null ||
                        fail recovery "a trial of recovery could not be armed"
                fi
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
        where || fail trial "the machine answered as another machine"
        if ! printf '%s\n' "$cmdline" | tr ' ' '\n' | grep -qx 'rach2\.slot=candidate' ||
            ! printf '%s\n' "$cmdline" | tr ' ' '\n' | grep -qx 'rach2\.console=term'; then
            fail trial "the trial boot's command line lacks rach2.slot=candidate or rach2.console=term: ${cmdline:-none read}"
        fi
        if i915 && ! printf '%s\n' "$cmdline" | tr ' ' '\n' | grep -qx 'rach2\.i915'; then
            fail trial "the trial boot's command line lacks rach2.i915: ${cmdline:-none read}"
        fi
        RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL=$files/../rach2/kernel \
            sh tools/dev/machine_identity.sh "$state/identity" ssh -i "$repo/krffs/rach2_demo_ed25519" \
            -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR -o UserKnownHostsFile=/dev/null \
            -o StrictHostKeyChecking=no "root@$address" > /dev/null 2>&1 ||
            fail trial "the machine does not run the new kernel from the candidate slot: $(tail -n 3 "$state/identity" | tr '\n' ' ')"
        mark trial "the new kernel runs from the candidate slot with term as its console"
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

    free_lenovo
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
    (cd "$repo" && sh tools/flow/sweep.sh) > "$state/$old.sweep" 2>&1 ||
        say "the sweep failed; $state/$old.sweep has its output"
    finish "passed $sha${kept:+; $kept}"
}

main "$@"
