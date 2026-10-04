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
# after a small edit rebuilds only what the edit touched. The one ignored
# file that is sent is the shareware WAD `ports/doom/fetch_wad.sh` downloads,
# because the Doom suite reads it from the image. `run`
# syncs and then runs COMMAND in the copy; the command is passed to the remote
# shell as one string, so quote it as that shell will see it. `get` copies
# each PATH, relative to the copy, from the host to the same path here,
# compressed on the wire, which is how a product that only the Linux host
# builds, such as the GNU system's ext2 root, reaches this machine. Its runs
# of zeros are written as holes, which the Mac's APFS keeps only for a long
# run, so an image that is sparse on the host is not sparse here. `sh` opens
# an interactive shell in the copy.
#
# `wait` watches a run started on the host as `/home/testing/NAME.sh`, which
# writes its results as lines beginning `NAME: ` to `/home/testing/NAME.out`
# and its work under `/home/testing/NAME/`, and returns as soon as there is
# something to act on: more than SEEN result lines (0 unless given), the run
# ended, nothing under its work directory changed for 15 minutes, which is a
# step stuck, or 100 minutes gone, which is before the two hours a background
# command of an agent may last. It prints the new lines and the reason, so
# that the caller reads each result when it lands and waits again with the
# new count; a watch that woke only at a run's end left results unread for
# hours behind its slower phases.
#
# The host is reached as `ssh testing`. Its ssh server answers in an MSYS
# shell on Windows; the copy lives in the WSL distribution, so the receiving
# rsync and the command both start through `wsl.exe`, and MSYS's rewriting of
# POSIX paths is switched off for the rsync it launches.
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
# under that KVM and 25 s emulated on 2026-09-18, and the 4 KiB write rows
# ran past the runner's silence limit. This is the host's setting, as the
# worker count is; the runner itself takes KVM wherever it opens.
set -e

REPO=$(cd "$(dirname "$0")/../.." && pwd)
HOST=testing
DEST=/home/testing/rach2
RSYNC_PATH='MSYS_NO_PATHCONV=1 wsl.exe -- rsync'

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

sync_tree()
{
    need_rsync3
    rsync -a --no-times --checksum --delete \
        --filter='+ /ports/doom/doom1.wad' --filter=':- .gitignore' \
        --rsync-path="$RSYNC_PATH" -e 'ssh -o BatchMode=yes' \
        "$REPO/" "$HOST:$DEST/"
}

case "${1:-}" in
    sync)
        sync_tree
        ;;
    run)
        shift
        if [ "$#" -eq 0 ]; then
            echo "testing: run needs a command" >&2
            exit 2
        fi
        sync_tree
        ssh -o BatchMode=yes "$HOST" "wsl.exe -- bash -lc 'cd $DEST && export RACH2_QEMU_ACCEL=tcg && $*'"
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
