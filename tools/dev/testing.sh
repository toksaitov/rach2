#!/bin/sh
#
# testing.sh: put this tree on the testing host and run a command there.
#
# Usage: testing.sh sync
#        testing.sh run COMMAND...
#        testing.sh sh
#
# `sync` replaces the copy of this tree on the testing host with the tree
# here, `build/` and the host-specific compilation database excluded. `run`
# syncs and then runs COMMAND in the copy; the command is passed to the remote
# shell as one string, so quote it as that shell will see it. `sh` opens an
# interactive shell in the copy.
#
# The host is reached as `ssh testing`. Its ssh server answers in an MSYS
# shell on Windows; the copy lives in the WSL distribution, so the receiving
# rsync and the command both start through `wsl.exe`, and MSYS's rewriting of
# POSIX paths is switched off for the rsync it launches.
set -e

REPO=$(cd "$(dirname "$0")/../.." && pwd)
HOST=testing
DEST=/home/testing/rach2
RSYNC_PATH='MSYS_NO_PATHCONV=1 wsl.exe -- rsync'

sync_tree()
{
    rsync -a --delete --exclude /build/ --exclude /compile_commands.json \
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
        ssh -o BatchMode=yes "$HOST" "wsl.exe -- bash -lc 'cd $DEST && $*'"
        ;;
    sh)
        ssh -t "$HOST" "wsl.exe -- bash -lc 'cd $DEST && exec bash'"
        ;;
    *)
        echo "usage: testing.sh sync | run COMMAND... | sh" >&2
        exit 2
        ;;
esac
