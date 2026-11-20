#!/bin/sh
#
# testing.sh: put this tree on the testing host and run a command there.
#
# Usage: testing.sh sync
#        testing.sh run COMMAND...
#        testing.sh get PATH...
#        testing.sh sh
#        testing.sh wait NAME [SEEN]
#
# `sync` replaces the tracked files of the copy on the testing host with the
# tree here. Whatever `.gitignore` names, `build/`, the extracted vendor trees
# and their stamps, the compilation database, is not sent, so the host
# builds with its own compiler, and what it built is left in place, so a run
# after a small edit rebuilds only what the edit touched. A sync refuses, and
# says why, while anything on the host works in the copy (see hold_copy). The
# one ignored file that is sent is the Doom WAD, which a person supplies and
# `ports/doom/fetch_wad.sh` checks, because the Doom suite reads it from the
# image. The filter's first rule protects the copy's WAD from deletion, ahead
# of the rule that sends it, since rsync applies the first rule that matches a
# name; so a sync from a tree that has no WAD, such as a fresh git worktree,
# leaves the copy's in place, and a sync from a tree that has one replaces it.
# The copy's `.git` is a repository of its own, so that git works there: a
# tree's `.git` directory is sent with the tree, and a git worktree's shared
# repository with the worktree's HEAD and index, as send_git describes.
# Each build and tests/run enters the installed site policy separately.
# Direct tools outside those entry points use the installed launcher.
#
# `run` syncs and then runs COMMAND in the copy; the words of COMMAND are
# joined with spaces and evaluated by bash there as one string, so quote it as
# that shell will see it, as run_held describes. `get` copies each PATH,
# relative to the copy, from the host to the same path here, compressed on the
# wire, which is how a product that only the Linux host builds, such as the
# GNU system's ext2 root, reaches this machine. Its runs of zeros are written
# as holes, which the Mac's APFS keeps only for a long run, so an image that
# is sparse on the host is not sparse here. `sh` opens an interactive shell in
# the copy.
#
# The copy is the owner's, `/home/testing/rach2`, or the path RACH2_TESTING_COPY
# names, so that callers on this machine that must not wait for each other, such
# as the owner's Codex loop and the manager's loops, each have a copy of their
# own, under the owner's `build/worktrees`. No tracked file writes out a path
# under the owner's copy whole: the GNU host checks copy every tracked file into
# the step store, which refuses a product that has the path of the copy that
# built it. A process in such a copy does not make the copy around it busy. The
# check that the copy is free and the sync are one step, under a lock in the
# copy, `COPY/build/.sync.lock`, which the sync leaves alone; two callers
# started at once would otherwise both find the copy free, and the later sync
# would land under the other's run. A caller takes the lock before it checks,
# keeps it until its sync ends, or for `run` until the command ends, and records
# itself in `COPY/build/.sync.owner`. A second caller is refused, not made to
# wait, as a busy copy refuses it, and prints that record: the first may hold
# the copy for a run of hours, and the second learns at once that its tree did
# not reach the host. The lock is flock(1)'s, taken by a shell in WSL whose
# input comes from this script, so it ends when this script ends, however it
# ends, or when the connection closes, and a caller that is gone holds nothing.
# That shell writes to the connection every ten seconds, so that a connection
# whose other end is gone without having closed it, as when this machine loses
# power, fails when TCP gives up on the data nobody acknowledges, not after the
# hours the host's TCP keepalive takes, and the shell ends with it. A lost
# connection frees the lock while this caller still works in the copy, so the
# caller watches it: a sync whose holder has ended fails and says so, `run`
# starts its command only while the holder lives, and a command whose holder
# ends first is stopped here and on the host, and the run fails and says so.
#
# Each process this script starts in the background, the lock's connection, the
# command's and the feeder of the command's input, runs under tools/flow/hold.pl
# in a run directory of its own under the temporary directory, which alone
# signals it and whose lock says whether it runs, so that no number is read or
# signalled here; each such hold.pl stops what it holds once this script has
# gone. At its end this script closes the lock's connection and, unless it has
# ended half a minute later, stops it through its hold.pl; it removes the
# temporary directory only once each run there has ended, and otherwise keeps
# it, says so and fails, since what a run whose holder was killed held may still
# run. On the host the command runs under the copy's own tools/flow/hold.pl, in
# the run directory build/held/NAME of the copy, NAME being RACH2_TESTING_HELD
# or one made of this machine's name, this script's number and a random part,
# made new only once `hold.pl clear` has removed the directories of the runs
# there that have ended and finds none that stays. The same check runs first
# under the sync lock, before anything is sent, for `sync` and `run` alike,
# and a run directory that stays refuses the copy, as hold_copy says: a run
# there that runs, or whose holder was killed, may still work in the copy,
# and nothing is sent or started beside it. A run that this
# script stops, or whose lock's connection ends, is stopped there
# through that directory's stop, with everything it started, which that hold.pl,
# a child subreaper, holds; and a caller that stops what works in the copy, as
# release.sh -p does, stops each run under build/held the same way.
#
# `wait` watches a run started on the host as RUNS/NAME.sh, by any path to
# it, such as `sh build/runs/NAME.sh` from the copy or `sh NAME.sh` in RUNS,
# RUNS being the owner's copy's `build/runs`. The run writes its results as
# lines beginning `NAME: ` to RUNS/NAME.out and its work under RUNS/NAME/.
# `wait` returns as soon as there is something to act on: more than SEEN
# result lines (0 unless given), the run ended, nothing under its work
# directory changed for 15 minutes, which is a step stuck, or 100 minutes
# gone, which is before the two hours a background command of an agent may
# last. It prints the new lines and the reason, so that the caller reads each
# result when it lands and waits again with the new count. The probe it sends
# ends with a command that succeeds, whether or not NAME.out exists yet, so a
# status other than 0 means that ssh or the copy's shell failed; `wait` then
# exits 1 and says so, since it cannot tell whether the run has ended.
#
# The host is reached as `ssh testing`. Its ssh server is Windows's OpenSSH,
# whose default shell, `C:\msys64\sshd_default_shell.cmd`, is a batch file
# that cmd.exe runs and that starts MSYS's bash with the command; the copy
# lives in the WSL distribution, so the receiving rsync and the command both
# start through `wsl.exe`, and MSYS's rewriting of POSIX paths is switched off
# for the rsync it launches.
#
# The rsync here must be the real one, 3.x from Homebrew. macOS ships
# openrsync under the same name, and it deletes on the host what the filter
# excludes, the whole build and every vendor tree, on every sync.
#
# The copy is compared by content, and a file that differs arrives with the
# time it lands, not the time it was edited here. A file edited while the
# host was building from its older text would otherwise arrive older than the
# objects built from that text, and make would take them for up to date.
#
# `run` asks the runner for the emulator. The host's KVM is nested under
# Hyper-V, and the ATA driver polls a port after every sector it writes, so
# each poll exits through two hypervisors: a 64 MiB write measured 236 s
# under that KVM and 25 s emulated, and the 4 KiB write rows ran past the
# runner's silence limit. This is the host's setting, as the worker count is;
# the runner itself takes KVM wherever it opens. `run` also sets
# RACH2_MIN_FREE_GIB to 40, so that a build in the copy stops, as
# tools/setup/preflight.sh says, before it fills the host's disk.
set -e

REPO=$(cd "$(dirname "$0")/../.." && pwd)
HOST=testing
OWNER=/home/testing/rach2
DEST=${RACH2_TESTING_COPY:-$OWNER}
case $DEST in
    /*[!A-Za-z0-9/._-]*|*/..*|/) echo "testing: not a copy's path: $DEST" >&2; exit 2 ;;
    /*) ;;
    *) echo "testing: not a copy's path: $DEST" >&2; exit 2 ;;
esac
RSYNC_PATH='MSYS_NO_PATHCONV=1 wsl.exe -- rsync'
HELD=${RACH2_TESTING_HELD:-run-$(hostname -s | tr -cd 'A-Za-z0-9-')-$$-$(od -An -N3 -tx1 /dev/urandom | tr -d ' \n')}
case $HELD in
    ''|*[!A-Za-z0-9._-]*) echo "testing: not a run's name: $HELD" >&2; exit 2 ;;
esac
hold=
holder=
feeder=
command=
trap 'at_exit "$?"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Exit 2 unless the rsync here is rsync 3.
need_rsync3()
{
    case "$(rsync --version 2>/dev/null | head -n 1)" in
        "rsync  version 3"*) ;;
        *)
            echo "testing: rsync is not rsync 3; install it (brew install rsync):" >&2
            echo "testing: openrsync deletes the host's build and vendor trees" >&2
            exit 2
            ;;
    esac
}

# hold_script WHO: the script hold_copy runs on the host in bash, given the
# copy's path. It takes the sync lock, or prints the owner file beside it,
# which describes the holder, and ends. Holding the lock, it writes WHO, which
# describes this caller, to the owner file, prints whatever works in the copy,
# then `held`, and keeps the lock until its input ends. A caller takes the
# lock or reads the owner file under a short lock on that file, so that a
# refused caller reads the record of the holder that refused it and not of the
# one before. The copy's run registry, build/held, is read by this tree's
# hold.pl, which the script carries in a here-document, since the copy's own
# may be older than its `clear` or absent until the first sync. Bash reads a
# script from a pipe a byte at a time, and the call to hold is the script's
# last line, so the read in hold finds no text of the script left in its
# input.
hold_script()
{
    printf "who='%s'\n" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
    echo '# clear_held ROOT: hold.pl clear ROOT, by the sender of this script.'
    echo 'clear_held()'
    echo '{'
    echo "    perl - clear \"\$1\" <<'HOLD_PL'"
    cat "$REPO/tools/flow/hold.pl"
    echo 'HOLD_PL'
    echo '}'
    echo
    cat <<'EOF'
# probe COPY: print what works in COPY: each run directory under its run
# registry, build/held, that `hold.pl clear` does not read ended, which it
# removes once ended, or that the registry cannot be read; its run lock while
# its owner runs, by the start that the owner record's identity gives and not
# by the number alone, or the group the record names has a process left, as
# etude's lock tree decides before it takes the lock over; and each process
# with its working directory in it but a person's shell.
probe()
{
    if ! registry=$(clear_held "$1/build/held" 2>&1); then
        printf '%s\n' "${registry:-hold.pl clear failed}" |
            sed 's/^/the run registry, not proved ended: /'
    fi
    lock=$1/build/.run.lock
    if [ -d "$lock" ]; then
        pid=$(sed -n 's/^pid //p' "$lock/owner" 2>/dev/null)
        start=$(sed -n 's/^identity //p' "$lock/owner" 2>/dev/null | cut -d ' ' -f 3)
        group=$(sed -n 's/^job //p' "$lock/owner" 2>/dev/null)
        case $group in ''|0) group=$(sed -n 's/^pgid //p' "$lock/owner" 2>/dev/null) ;; esac
        now=$(sed 's/.*) //' "/proc/${pid:-0}/stat" 2>/dev/null | cut -d ' ' -f 20)
        args=$(sed -n 's/^args //p' "$lock/owner" 2>/dev/null)
        if [ -n "$now" ] && { [ -z "$start" ] || [ "$now" = "$start" ]; }; then
            echo "the run lock, held by pid $pid: $args"
        elif [ -n "$group" ] && ps -A -o pgid= | grep -qx " *$group"; then
            echo "the run lock, left by pid $pid, whose group $group has processes left: $args"
        fi
    fi
    for d in /proc/[0-9]*; do
        case "$(readlink "$d/cwd" 2>/dev/null)" in
            "$1"/build/worktrees/*) continue ;;
            "$1"|"$1"/*) ;;
            *) continue ;;
        esac
        command=$(tr '\0' ' ' 2>/dev/null < "$d/cmdline")
        case "$(cat "$d/comm" 2>/dev/null)" in
            bash|sh|dash|zsh)
                # A shell that runs no script of its own is a person's prompt.
                echo "$command" | awk '$2 != "" && $2 !~ /^-/ {f = 1} END {exit !f}' || continue ;;
        esac
        echo "pid ${d#/proc/}: $(echo "$command" | cut -c1-120)"
    done
}

# hold COPY: take the sync lock for COPY, or print its holder, as hold_script
# describes.
hold()
{
    mkdir -p "$1/build" || exit 1
    exec 8>> "$1/build/.sync.owner" 9>> "$1/build/.sync.lock" || exit 1
    flock 8 || exit 1
    flock -n 9 || {
        [ $? -eq 1 ] && echo "the sync lock, held by $(cat "$1/build/.sync.owner")"
        exit 0
    }
    echo "pid $$ since $(date '+%Y-%m-%d %H:%M:%S'): $who" > "$1/build/.sync.owner"
    exec 8>&-
    probe "$1"
    echo held
    while :; do
        read -r -t 10 _
        case $? in
            0) ;;
            1) exit 0 ;;
            *) echo . >&2 || exit 1 ;;
        esac
    done
}

hold "$@"
EOF
}

# hold_copy WHAT: take the sync lock on the host and check that nothing there
# works in the copy, as one step, and keep the lock until release_copy; or
# say why not and fail. A sync rewrites the sources under whatever works
# there, and a build that reads half of the old tree and half of the new one
# fails in ways that look like faults of the code. The copy is busy while a
# run directory under its build/held is not proved ended, since a command
# whose holder was killed may still run and reach the copy by absolute paths
# from a working directory outside it, which no scan of working directories
# finds; while a run or a script holds its run lock and the holder lives; or
# while any process other than a shell has its working directory in it: a
# make, a compiler, QEMU, or a script between two runs. The owner file records
# WHAT, `sync` or `run`, and this tree's path. The holder's input is its
# script and then whatever reaches fd 3, which is nothing: closing fd 3 ends
# the input, and the holder releases the lock.
hold_copy()
{
    hold=$(mktemp -d "${TMPDIR:-/tmp}/rach2-testing.XXXXXX") || return 1
    mkfifo "$hold/in" "$hold/out" || return 1
    mkdir "$hold/holder" || return 1
    { hold_script "$1 from $(hostname -s):$REPO"; cat; } < "$hold/in" |
        perl "$REPO/tools/flow/hold.pl" run -P -k 10 "$hold/holder" \
            ssh -o BatchMode=yes -o ServerAliveInterval=15 -o ServerAliveCountMax=4 "$HOST" "MSYS_NO_PATHCONV=1 wsl.exe -- bash -s -- $DEST" \
            > "$hold/out" 2> "$hold/err" &
    holder=$!
    exec 3> "$hold/in" 4< "$hold/out"
    verdict=$(sed '/^held$/q' <&4)
    if [ "$verdict" = held ]; then
        exec 4<&-
        return 0
    fi
    release_copy
    if [ -z "$verdict" ]; then
        cat "$hold/err" >&2
        echo "testing: the host did not answer; nothing was synced" >&2
        return 1
    fi
    echo "testing: the copy at $DEST is in use; nothing was synced:" >&2
    printf '%s\n' "$verdict" | sed '/^held$/d; s/^/testing:   /' >&2
    return 1
}

# release_copy: end the holder's input, which releases the sync lock, and
# wait for its connection to close, so that the lock is free by the time
# this script ends: thirty turns of a second, and then a stop of its
# hold.pl, which sends TERM and KILL ten seconds later, half a minute at
# most. A connection whose end is not confirmed is said, and fails.
release_copy()
{
    [ -n "$holder" ] || return 0
    exec 3>&- 4<&-
    rc_i=0
    while runs "$hold/holder" && [ "$rc_i" -lt 30 ]; do
        sleep 1
        rc_i=$((rc_i + 1))
    done
    if ! rc_state=$(perl "$REPO/tools/flow/hold.pl" stop "$hold/holder" 30); then
        echo "testing: the connection that held the sync lock is not confirmed ended: $rc_state; $hold stays" >&2
        holder=
        return 1
    fi
    wait "$holder" 2>/dev/null || :
    holder=
}

# at_exit STATUS: stop what this script started and release the copy, and
# exit with STATUS, or 1 when an end is not confirmed.
at_exit()
{
    ae_rc=$1
    stop_command
    stop_feeder
    release_copy || ae_rc=1
    forget_hold || ae_rc=1
    exit "$ae_rc"
}

# forget_hold: remove the temporary directory of this script's holders once
# each run there has ended, as hold.pl's lock says; otherwise say which has
# not, keep it, and fail, since what that run held may still run.
forget_hold()
{
    [ -n "$hold" ] || return 0
    for fh_dir in "$hold/holder" "$hold/feeder" "$hold/cmd"; do
        fh_state=$(perl "$REPO/tools/flow/hold.pl" state "$fh_dir")
        case $fh_state in
            "ended "* | absent) ;;
            *)
                echo "testing: the run in $fh_dir has not confirmed its end: $fh_state; $hold stays" >&2
                return 1 ;;
        esac
    done
    rm -rf "$hold"
    hold=
}

# runs DIR: succeed while the hold.pl of the run directory DIR holds what it
# runs, or is about to.
runs()
{
    case $(perl "$REPO/tools/flow/hold.pl" state "$1") in
        running | starting) return 0 ;;
    esac
    return 1
}

# holder_lost WHEN: say that the connection that holds the sync lock ended
# WHEN, and fail.
holder_lost()
{
    echo "testing: the connection that held the sync lock ended $1" >&2
    exit 1
}

# send ARG...: run rsync with ARG... to the host, comparing by content and
# deleting what the source lacks.
send()
{
    rsync -a --no-times --checksum --delete \
        --rsync-path="$RSYNC_PATH" -e 'ssh -o BatchMode=yes -o ServerAliveInterval=15 -o ServerAliveCountMax=4' "$@" 3>&-
}

# send_git: make the copy's `.git` a repository of its own at this worktree's
# commit. A worktree's `.git` is a file that names its git directory on this
# machine, a path the host does not have. The copy gets instead the shared
# repository, without its worktrees' records or the main tree's HEAD and
# index, and then this worktree's HEAD and index. The repository goes as the
# directory a symbolic link named `.git` leads to, which rsync's -k sends as
# a directory, so that it replaces a `.git` file an older sync left there.
send_git()
{
    gitdir=$(cd "$REPO" && cd "$(git rev-parse --git-dir)" && pwd)
    common=$(cd "$REPO" && cd "$(git rev-parse --git-common-dir)" && pwd)
    mkdir "$hold/stage"
    ln -s "$common" "$hold/stage/.git"
    send -k --filter='+ /.git/' --filter='- /.git/HEAD' \
        --filter='- /.git/index' --filter='- /.git/worktrees/' --filter='- /*' \
        "$hold/stage/" "$HOST:$DEST/"
    send "$gitdir/HEAD" "$gitdir/index" "$HOST:$DEST/.git/"
}

# sync_tree WHAT: hold the copy for WHAT, `sync` or `run`, and send this tree.
sync_tree()
{
    need_rsync3
    hold_copy "$1" || exit 1
    if [ -f "$REPO/.git" ]; then
        send --filter='- /.git' --filter='P /ports/doom/doom1.wad' \
            --filter='+ /ports/doom/doom1.wad' --filter=':- .gitignore' \
            "$REPO/" "$HOST:$DEST/"
        send_git
    else
        send --filter='P /ports/doom/doom1.wad' \
            --filter='+ /ports/doom/doom1.wad' --filter=':- .gitignore' \
            "$REPO/" "$HOST:$DEST/"
    fi
    runs "$hold/holder" ||
        holder_lost "during the sync; another caller may have synced over it"
}

# run_held COMMAND: run COMMAND in the copy while the holder lives. The
# command's ssh runs in the background, so that the holder can be watched; if
# the holder ends first, the command is stopped, here and on the host.
#
# COMMAND travels on the ssh's input, as one line of base64 ahead of this
# script's own input, which the command reads after it. sshd hands the ssh
# command to its default shell as `-c "COMMAND"`, and cmd.exe reads that line
# first: it refuses a line longer than 8191 characters, a double quote in the
# command ends cmd's quoting, so that a `|` or `&` after it starts a Windows
# program, and cmd expands `%` and, in that batch file, `!`. MSYS's shell parses
# the line next, and wsl.exe, given `--`, would hand the rest of its command
# line to a shell that parses it again. So the line is the same for every
# command and has no double quote, `%` or `!`, and its `&&`, `<` and parentheses
# stay inside cmd's quotes. Bash in WSL reads the first line of its input a byte
# at a time, as it reads any input it cannot seek, so that the rest is left for
# the command; it decodes the line from a here-string into a process
# substitution, where a quoted `eval` would need double quotes, and execs the
# copy's hold.pl, which runs bash on that text in the run directory
# build/held/NAME, made new. `-e` makes wsl.exe start bash with its arguments
# as given, and MSYS_NO_PATHCONV keeps MSYS from rewriting an argument it takes
# for a path. The feeder that writes the input is stopped once the command
# ends, since it may wait on a terminal nobody types in.
run_held()
{
    exec 5<&0
    mkfifo "$hold/command"
    mkdir "$hold/feeder" "$hold/cmd"
    perl "$REPO/tools/flow/hold.pl" run -P -k 10 "$hold/feeder" \
        sh -c 'printf "%s" "$1" | base64 | tr -d "\n"; echo; exec cat' sh "$*" \
        <&5 5<&- > "$hold/command" 3>&- &
    feeder=$!
    perl "$REPO/tools/flow/hold.pl" run -P -k 10 "$hold/cmd" \
        ssh -o BatchMode=yes -o ServerAliveInterval=15 -o ServerAliveCountMax=4 "$HOST" "MSYS_NO_PATHCONV=1 wsl.exe -e bash -lc 'cd $DEST && export RACH2_QEMU_ACCEL=tcg RACH2_MIN_FREE_GIB=40 && mkdir -p build/held && perl tools/flow/hold.pl clear build/held && mkdir build/held/$HELD && read -r c && exec perl tools/flow/hold.pl run -P -k 60 build/held/$HELD bash <(base64 -d <<< \$c)'" \
        < "$hold/command" 3>&- 5<&- &
    command=$!
    exec 5<&-
    while runs "$hold/cmd"; do
        if ! runs "$hold/holder"; then
            stop_command
            holder_lost "while the command ran; it was stopped here and on the host"
        fi
        sleep 2
    done
    status=0
    wait "$command" || status=$?
    case $(perl "$REPO/tools/flow/hold.pl" state "$hold/cmd") in
        "ended "*) ;;
        *)
            # Its holder was killed, and its ssh may still run.
            stop_command
            echo "testing: the holder of the command's connection ended without its end; $hold stays" >&2
            exit 1 ;;
    esac
    command=
    stop_feeder
    return "$status"
}

# stop_command: stop the command that run_held started, if it has not ended:
# its ssh here, through its hold.pl, and its run on the host, through the
# stop of build/held/NAME there, waiting two minutes at most for that run to
# end; say so when that is not confirmed.
stop_command()
{
    [ -n "$command" ] || return 0
    perl "$REPO/tools/flow/hold.pl" stop "$hold/cmd" 30 > /dev/null || :
    wait "$command" 2>/dev/null || :
    command=
    perl "$REPO/tools/flow/bound.pl" 180 ssh -o BatchMode=yes -o ServerAliveInterval=15 -o ServerAliveCountMax=4 "$HOST" \
        "MSYS_NO_PATHCONV=1 wsl.exe -e bash -lc 'cd $DEST && perl tools/flow/hold.pl stop build/held/$HELD 120'" \
        < /dev/null > /dev/null 2>&1 ||
        echo "testing: the run build/held/$HELD in $DEST is not confirmed stopped on the host" >&2
}

# stop_feeder: stop the process that writes the command's input, if any.
stop_feeder()
{
    [ -n "$feeder" ] || return 0
    perl "$REPO/tools/flow/hold.pl" stop "$hold/feeder" 10 > /dev/null || :
    wait "$feeder" 2>/dev/null || :
    feeder=
}

case "${1:-}" in
    sync)
        sync_tree sync
        ;;
    run)
        shift
        if [ "$#" -eq 0 ]; then
            echo "testing: run needs a command" >&2
            exit 2
        fi
        sync_tree run
        run_held "$@"
        ;;
    get)
        shift
        if [ "$#" -eq 0 ]; then
            echo "testing: get needs a path" >&2
            exit 2
        fi
        need_rsync3
        for path in "$@"; do
            mkdir -p "$REPO/$(dirname "$path")"
            rsync --times --sparse --compress \
                --rsync-path="$RSYNC_PATH" -e 'ssh -o BatchMode=yes -o ServerAliveInterval=15 -o ServerAliveCountMax=4' \
                "$HOST:$DEST/$path" "$REPO/$path"
        done
        ;;
    sh)
        ssh -t "$HOST" "wsl.exe -- bash -lc 'cd $DEST && exec bash'"
        ;;
    wait)
        name=${2:-}
        seen=${3:-0}
        case "$name" in
            ''|*[!A-Za-z0-9_-]*) echo "testing: wait needs a run's name" >&2; exit 2 ;;
        esac
        end=$(( $(date +%s) + 6000 ))
        runs=$OWNER/build/runs
        while :; do
            status=0
            out=$(printf 'pgrep -f %s > /dev/null && echo RUNNING; find %s/%s -type f -mmin -15 2>/dev/null | head -1 | grep -q . && echo FRESH; cat %s/%s.out 2>/dev/null || :\n' \
                "'[ /]$name\\.sh( |\$)'" "$runs" "$name" "$runs" "$name" | "$0" sh 2>/dev/null) || status=$?
            if [ "$status" -ne 0 ]; then
                echo "testing: $name: the host did not answer, status $status; its run may still go on" >&2
                exit 1
            fi
            lines=$(printf '%s\n' "$out" | grep -c "^$name: " || true)
            new=$(printf '%s\n' "$out" | grep "^$name: " | tail -n +$((seen + 1)) || true)
            case "$out" in
                *RUNNING*) ;;
                *) printf '%s\n' "$new"; echo "testing: $name: ended, $lines lines"; exit 0 ;;
            esac
            if [ "$lines" -gt "$seen" ]; then
                printf '%s\n' "$new"; echo "testing: $name: running, $lines lines"; exit 0
            fi
            case "$out" in
                *FRESH*) ;;
                *) echo "testing: $name: stalled, nothing under $runs/$name changed for 15 minutes, $lines lines"; exit 0 ;;
            esac
            if [ "$(date +%s)" -ge "$end" ]; then
                echo "testing: $name: still running after 100 minutes, $lines lines; wait again"
                exit 0
            fi
            sleep 60
        done
        ;;
    *)
        echo "usage: testing.sh sync | run COMMAND... | get PATH... | sh | wait NAME [SEEN]" >&2
        exit 2
        ;;
esac
