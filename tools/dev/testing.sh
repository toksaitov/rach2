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
# The copy is `/home/testing/rach2`, or the path RACH2_TESTING_COPY names, so
# that callers on this machine that must not wait for each other, such as the
# owner's worker loops, each have a copy of their own. The check that the copy
# is free and the sync are one step, under a lock on the host beside the copy,
# `COPY.sync.lock`; two callers started at once would otherwise both find the
# copy free, and the later sync would land under the other's run. A caller
# takes the lock before it checks, keeps it until its sync ends, or for `run`
# until the command ends, and records itself in `COPY.sync.owner`. A second
# caller is refused, not made to wait, as a busy copy refuses it, and prints
# that record: the first may hold the copy for a run of hours, and the second
# learns at once that its tree did not reach the host. The lock is flock(1)'s,
# taken by a shell in WSL whose input comes from this script, so it ends when
# this script ends, however it ends, or when the connection closes, and a
# caller that is gone holds nothing. That shell writes to the connection every
# ten seconds, so that a connection whose other end is gone without having
# closed it, as when this machine loses power, fails when TCP gives up on the
# data nobody acknowledges, not after the hours the host's TCP keepalive
# takes, and the shell ends with it. A lost connection frees the lock while
# this caller still works in the copy, so the caller watches it: a sync whose
# holder has ended fails and says so, `run` starts its command only while the
# holder lives, and a command whose holder ends first is stopped here and the
# run fails and says so. The command's side on the host is not stopped with
# it.
#
# `wait` watches a run started on the host as `/home/testing/NAME.sh`, which
# writes its results as lines beginning `NAME: ` to `/home/testing/NAME.out`
# and its work under `/home/testing/NAME/`, and returns as soon as there is
# something to act on: more than SEEN result lines (0 unless given), the run
# ended, nothing under its work directory changed for 15 minutes, which is a
# step stuck, or 100 minutes gone, which is before the two hours a background
# command of an agent may last. It prints the new lines and the reason, so
# that the caller reads each result when it lands and waits again with the
# new count.
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
# the runner itself takes KVM wherever it opens.
set -e

REPO=$(cd "$(dirname "$0")/../.." && pwd)
HOST=testing
DEST=${RACH2_TESTING_COPY:-/home/testing/rach2}
case $DEST in
    /*[!A-Za-z0-9/._-]*|*/..*|/) echo "testing: not a copy's path: $DEST" >&2; exit 2 ;;
    /*) ;;
    *) echo "testing: not a copy's path: $DEST" >&2; exit 2 ;;
esac
RSYNC_PATH='MSYS_NO_PATHCONV=1 wsl.exe -- rsync'
hold=
holder=
feeder=
trap 'stop_feeder; release_copy; [ -z "$hold" ] || rm -rf "$hold"' EXIT

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
# one before. Bash reads a script from a pipe a byte at a time, and the call
# to hold is the script's last line, so the read in hold finds no text of the
# script left in its input.
hold_script()
{
    printf "who='%s'\n" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
    cat <<'EOF'
# probe COPY: print what works in COPY: a live holder of its run lock, and
# each process with its working directory in it but a person's shell.
probe()
{
    lock=$1/build/.run.lock
    if [ -d "$lock" ]; then
        pid=$(sed -n 's/^pid //p' "$lock/owner" 2>/dev/null)
        [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null &&
            echo "the run lock, held by pid $pid: $(sed -n 's/^args //p' "$lock/owner")"
    fi
    for d in /proc/[0-9]*; do
        case "$(readlink "$d/cwd" 2>/dev/null)" in
            "$1"|"$1"/*) ;;
            *) continue ;;
        esac
        command=$(tr '\0' ' ' < "$d/cmdline" 2>/dev/null)
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
    exec 8>> "$1.sync.owner" 9>> "$1.sync.lock" || exit 1
    flock 8 || exit 1
    flock -n 9 || {
        [ $? -eq 1 ] && echo "the sync lock, held by $(cat "$1.sync.owner")"
        exit 0
    }
    echo "pid $$ since $(date '+%Y-%m-%d %H:%M:%S'): $who" > "$1.sync.owner"
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
# run or a fault script holds its run lock and the holder lives, or while
# any process other than a shell has its working directory in it: a make, a
# compiler, QEMU, or a script between two runs. The owner file records WHAT,
# `sync` or `run`, and this tree's path. The holder's input is its script
# and then whatever reaches fd 3, which is nothing: closing fd 3 ends the
# input, and the holder releases the lock.
hold_copy()
{
    hold=$(mktemp -d "${TMPDIR:-/tmp}/testing.XXXXXX") || return 1
    mkfifo "$hold/in" "$hold/out" || return 1
    { hold_script "$1 from $(hostname -s):$REPO"; cat; } < "$hold/in" |
        ssh -o BatchMode=yes "$HOST" "MSYS_NO_PATHCONV=1 wsl.exe -- bash -s -- $DEST" \
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
# this script ends.
release_copy()
{
    [ -n "$holder" ] || return 0
    exec 3>&- 4<&-
    wait "$holder" 2>/dev/null || :
    holder=
}

# holder_lost WHEN: say that the connection that holds the sync lock ended
# WHEN, and fail.
holder_lost()
{
    echo "testing: the connection that held the sync lock ended $1" >&2
    exit 1
}

# sync_tree WHAT: hold the copy for WHAT, `sync` or `run`, and send this tree.
sync_tree()
{
    need_rsync3
    hold_copy "$1" || exit 1
    rsync -a --no-times --checksum --delete \
        --filter='P /ports/doom/doom1.wad' \
        --filter='+ /ports/doom/doom1.wad' --filter=':- .gitignore' \
        --rsync-path="$RSYNC_PATH" -e 'ssh -o BatchMode=yes' \
        "$REPO/" "$HOST:$DEST/" 3>&-
    kill -0 "$holder" 2>/dev/null ||
        holder_lost "during the sync; another caller may have synced over it"
}

# run_held COMMAND: run COMMAND in the copy while the holder lives. The
# command's ssh runs in the background, so that the holder can be watched; if
# the holder ends first, the ssh is stopped.
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
# the command; it decodes the line from a here-string and sources the text,
# where a quoted `eval` would need double quotes. `-e` makes wsl.exe start bash
# with its arguments as given, and MSYS_NO_PATHCONV keeps MSYS from rewriting an
# argument it takes for a path. The feeder that writes the input is stopped once
# the command ends, since it may wait on a terminal nobody types in.
run_held()
{
    exec 5<&0
    mkfifo "$hold/command"
    { printf '%s' "$*" | base64 | tr -d '\n'; echo; exec cat; } \
        <&5 5<&- > "$hold/command" 3>&- &
    feeder=$!
    ssh -o BatchMode=yes "$HOST" "MSYS_NO_PATHCONV=1 wsl.exe -e bash -lc 'cd $DEST && export RACH2_QEMU_ACCEL=tcg && read -r c && . <(base64 -d <<< \$c)'" \
        < "$hold/command" 3>&- 5<&- &
    command=$!
    exec 5<&-
    while kill -0 "$command" 2>/dev/null; do
        if ! kill -0 "$holder" 2>/dev/null; then
            kill "$command" 2>/dev/null || :
            wait "$command" 2>/dev/null || :
            holder_lost "while the command ran; it was stopped here, and its side on the host may still run"
        fi
        sleep 2
    done
    status=0
    wait "$command" || status=$?
    stop_feeder
    return "$status"
}

# stop_feeder: stop the process that writes the command's input, if any.
stop_feeder()
{
    [ -n "$feeder" ] || return 0
    kill "$feeder" 2>/dev/null || :
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
                --rsync-path="$RSYNC_PATH" -e 'ssh -o BatchMode=yes' \
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
        while :; do
            out=$(printf 'pgrep -f /home/testing/%s.sh > /dev/null || pgrep -f "sh %s.sh" > /dev/null && echo RUNNING; find /home/testing/%s -type f -mmin -15 2>/dev/null | head -1 | grep -q . && echo FRESH; cat /home/testing/%s.out 2>/dev/null\n' \
                "$name" "$name" "$name" "$name" | "$0" sh 2>/dev/null) || out=
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
                *) echo "testing: $name: stalled, nothing under /home/testing/$name changed for 15 minutes, $lines lines"; exit 0 ;;
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
