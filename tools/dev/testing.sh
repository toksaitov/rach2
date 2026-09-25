#!/bin/sh
#
# testing.sh: put this tree on the testing host and run a command there.
#
# Usage: testing.sh sync
#        testing.sh run COMMAND...
#        testing.sh sh
#
# `sync` replaces the tracked files of the copy on the testing host with the
# tree here. Whatever `.gitignore` names, `build/`, the extracted vendor trees
# and their stamps, the compilation database, is not sent, so the host
# builds with its own compiler, and what it built is left in place, so a run
# after a small edit rebuilds only what the edit touched. The one ignored
# file that is sent is the shareware WAD `ports/doom/fetch_wad.sh` downloads,
# because the Doom suite reads it from the image. `run`
# syncs and then runs COMMAND in the copy; the command is passed to the remote
# shell as one string, so quote it as that shell will see it. `sh` opens an
# interactive shell in the copy.
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

sync_tree()
{
    case "$(rsync --version 2>/dev/null | head -n 1)" in
        "rsync  version 3"*) ;;
        *)
            echo "testing: rsync is not rsync 3; install it (brew install rsync):" >&2
            echo "testing: openrsync deletes the host's build and vendor trees" >&2
            exit 2
            ;;
    esac
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
    sh)
        ssh -t "$HOST" "wsl.exe -- bash -lc 'cd $DEST && exec bash'"
        ;;
    *)
        echo "usage: testing.sh sync | run COMMAND... | sh" >&2
        exit 2
        ;;
esac
