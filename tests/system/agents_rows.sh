#!/bin/bash

#
# agents_rows.sh: run one row of `tests/run agents` on the machine.
#
# Usage: agents_rows.sh NAME [VERSION]
#
# The agents suite puts this script and the rows of tests/system/agents/ in
# /tmp/agents on the machine it boots, the GNU root with the agents layer,
# and runs it there as the test account under /bin/bash, once for each row,
# each run bounded on the host. It reads the row's file, NAME.sh, and runs
# its function `row` in an empty directory of the row's own, WORK, with
# VERSION, the version the package's recipe pins, in VERSION, and the path
# of the fake API server's certificate authority in CA. A row has a
# program do its job as a person would ask for it and compares what it
# printed or wrote with what it must be, through the functions below; a row
# that runs the tree's tools, such as the worker loop, makes a git repository
# of the tree's tests and tools with `tree`, and a row
# that drives a program's screen runs it in tmux, on the server and in the
# session PANE names, waits for it with `pane` and answers its questions
# with `keys`, `press` and `choose`. A row that inspects a program while it
# waits for the fake server's answer has that answer held, with `held` and
# `release`. The script prints
# `AGENTROW OK NAME` when every comparison holds, and otherwise
# `AGENTROW FAIL NAME: ` with the first that did not.
#

set -u
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export LC_ALL=C

ROW=${1:-}
# shellcheck disable=SC2034 # the rows read it
VERSION=${2:-}
WORK=/tmp/agents/work/$ROW
# shellcheck disable=SC2034 # the rows read it
CA=/tmp/agents/ca.pem
PANE=
SELECTED=
FAILED=
OUT=

# fail WHAT: record WHAT unless an earlier comparison already failed.
fail() {
    [ -n "$FAILED" ] || FAILED=$1
}

# same WHAT GOT WANT: require GOT to be WANT.
same() {
    [ "$2" = "$3" ] || fail "$1: got [$2], want [$3]"
}

# has WHAT GOT WANT: require GOT to contain WANT.
has() {
    case "$2" in
        *"$3"*) ;;
        *) fail "$1: [$(printf '%s' "$2" | tail -n 3 | tr '\n' ' ')] has no [$3]" ;;
    esac
}

# output WHAT COMMAND...: keep what COMMAND prints on both streams in OUT,
# and require it to exit with status 0.
output() {
    local what=$1
    shift
    OUT=$("$@" 2>&1) || fail "$what: status $?: $(printf '%s' "$OUT" | tail -n 3 | tr '\n' ' ')"
}

# status WHAT WANT COMMAND...: keep what COMMAND prints on both streams in
# OUT, and require it to exit with status WANT.
status() {
    local what=$1 want=$2 got
    shift 2
    OUT=$("$@" 2>&1)
    got=$?
    [ "$got" = "$want" ] ||
        fail "$what: status $got, want $want: $(printf '%s' "$OUT" | tail -n 3 | tr '\n' ' ')"
}

# tree: make WORK/tree a git repository with one commit of the tree's tests
# and tools, from the tar the suite put in /tmp/agents, or fail and return 1.
tree() {
    if ! { mkdir "$WORK/tree" && tar -xf /tmp/agents/tree.tar -C "$WORK/tree" &&
        git -C "$WORK/tree" init -q && git -C "$WORK/tree" add -A &&
        git -C "$WORK/tree" -c user.name=rach2 -c user.email=test@rach2.invalid commit -qm tree; }; then
        fail "the tree's tests and tools in a git repository"
        return 1
    fi
}

# pane WHAT TEXT SECONDS: wait until the window of the tmux server PANE, a
# session of the same name, shows TEXT, looking once a second for at most
# SECONDS.
pane() {
    local n=0 screen
    while :; do
        screen=$(tmux -u -L "$PANE" capture-pane -p -t "$PANE" 2>&1)
        case "$screen" in
            *"$2"*) return 0 ;;
        esac
        if [ "$n" -ge "$3" ]; then
            fail "$1: in $3 s the screen showed no [$2], but [$(printf '%s' "$screen" |
                sed '/^ *$/d' | tail -n 4 | tr '\n' ' ')]"
            return 1
        fi
        n=$((n + 1))
        sleep 1
    done
}

# keys KEY...: press the keys KEY in the window of the tmux server PANE.
keys() {
    tmux -u -L "$PANE" send-keys -t "$PANE" "$@"
}

# program NAME: print the process ID of NAME, the program that the shell of
# the window of the tmux server PANE started. A child of the program has its
# name until the child calls execve, so a search by the name alone can find
# two processes.
program() {
    pgrep -P "$(tmux -L "$PANE" display-message -p -t "$PANE" '#{pane_pid}')" -x "$1"
}

# press WHAT KEY TEXT: press KEY until the window of PANE shows TEXT on two
# reads in a row, looking once a second for at most 300 s. KEY is pressed on
# a screen without TEXT that is the same on two reads in a row and that the
# program has drawn since KEY was last pressed. A program's question can
# refuse keys for a moment after it opens and then be drawn again, or take a
# key and draw its first answer again; either way the program settles on a
# new screen without TEXT, and is given KEY again. A key whose effect a slow
# program has yet to draw leaves the screen as it was, and is not pressed
# twice.
press() {
    local n=0 screen last='' pressed='' drawn=1 shown=''
    while :; do
        screen=$(tmux -u -L "$PANE" capture-pane -p -t "$PANE")
        [ "$screen" = "$pressed" ] || drawn=1
        case "$screen" in
            *"$3"*)
                [ -z "$shown" ] || return 0
                shown=1
                ;;
            *)
                shown=
                if [ "$screen" = "$last" ] && [ -n "$drawn" ]; then
                    keys "$2"
                    pressed=$screen
                    drawn=
                fi
                ;;
        esac
        last=$screen
        if [ "$n" -ge 300 ]; then
            pane "$1" "$3" 0
            return 1
        fi
        n=$((n + 1))
        sleep 1
    done
}

# choose WHAT KEY ANSWER: press KEY until the window of PANE has marked
# ANSWER selected, the program's mark SELECTED before it, as `press` does.
choose() {
    press "$1" "$2" "$SELECTED $3"
}

# exited WHAT SECONDS COMMAND...: wait until COMMAND, which prints the
# processes it finds, prints none, looking once a second; SECONDS only ends
# the wait for a process that does not exit. Fail WHAT with those it still
# prints then.
exited() {
    local what=$1 bound=$2 n=0 procs
    shift 2
    while procs=$("$@") && [ -n "$procs" ] && [ "$n" -lt "$bound" ]; do
        n=$((n + 1))
        sleep 1
    done
    same "$what" "$(printf '%s' "$procs" | tr '\n' ' ')" ""
}

# held WHAT PID: wait until the fake server holds its answer to the request
# of its script's held step, which the program PID sends, or until the
# program ends. Return 0 while the server holds it, the program then waiting
# for that answer until `release`. Otherwise fail WHAT and return 1: the
# program ended first, or the server could not be asked, and the program is
# stopped.
held() {
    local waiter ended status
    curl -fsS --cacert "$CA" -o /dev/null https://fakeapi.test/fakeapi/held &
    waiter=$!
    wait -n -p ended "$2" "$waiter"
    status=$?
    if [ "$ended" = "$2" ]; then
        kill "$waiter" 2> /dev/null
        wait "$waiter" 2> /dev/null
        fail "$1 ended before the fake server held its last answer"
        return 1
    fi
    if [ "$status" != 0 ]; then
        kill "$2" 2> /dev/null
        fail "$1: the fake server's hold: curl status $status"
        return 1
    fi
}

# release PID: have the fake server send the answer it holds, which the
# program PID waits for; stop the program when the server cannot be asked.
release() {
    curl -fsS --cacert "$CA" -o /dev/null -X POST https://fakeapi.test/fakeapi/release ||
        { fail "the fake server's release: curl status $?"; kill "$1" 2> /dev/null; }
}

# sample_memory: print the kernel's count of free frames, nr_free_pages of
# /proc/vmstat, once a second until killed.
sample_memory() {
    while :; do
        awk '$1 == "nr_free_pages" {print $2}' /proc/vmstat
        sleep 1
    done
}

# unmodified WHAT PID PROGRAM FILE...: require the process PID to run
# PROGRAM with nothing preloaded: the process runs PROGRAM, its environment
# has no variable of the dynamic linker's, LD_*, the root has no
# /etc/ld.so.preload, and every file the process maps, with every link in
# its path resolved, is PROGRAM, matches one of the FILEs, each a path or a
# pattern of the shell, or is the linker's cache, /etc/ld.so.cache, which
# glibc's linker keeps mapped after a dlopen. The host compares the
# program's digest with the one its vendor published.
unmodified() {
    local what=$1 pid=$2 program=$3 exe maps linker file resolved allowed
    shift 3
    exe=$(readlink "/proc/$pid/exe")
    maps=$(awk '$6 ~ /^\// { print $6 }' "/proc/$pid/maps" | sort -u)
    linker=$(tr '\0' '\n' < "/proc/$pid/environ" | grep '^LD_')
    same "$what: the program" "$exe" "$program"
    [ -n "$maps" ] || fail "$what: /proc/$pid/maps names no file"
    for file in $maps; do
        resolved=$(readlink -f "$file")
        for allowed in "$program" /etc/ld.so.cache "$@" ""; do
            # shellcheck disable=SC2254 # a FILE may be a pattern
            case $resolved in
                $allowed) break ;;
            esac
        done
        [ -n "$allowed" ] || fail "$what: the process maps $file"
    done
    same "$what: the linker's variables" "$linker" ""
    [ ! -e /etc/ld.so.preload ] || fail "$what: the root has /etc/ld.so.preload"
}

if [ -z "$ROW" ] || [ ! -f "/tmp/agents/$ROW.sh" ]; then
    echo "AGENTROW FAIL $ROW: no such row"
    exit 2
fi
# shellcheck disable=SC1090 # the row is chosen at run time
. "/tmp/agents/$ROW.sh"
if ! declare -F row > /dev/null; then
    echo "AGENTROW FAIL $ROW: $ROW.sh defines no function row"
    exit 2
fi
# Stop a row's children before removing its work on success or a signal.
cleanup() {
    if declare -F row_cleanup > /dev/null; then
        row_cleanup
    fi
    cd / || return
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK" || exit 2
row
cd / || exit 2
rm -rf "$WORK"
if [ -n "$FAILED" ]; then
    echo "AGENTROW FAIL $ROW: $FAILED"
    exit 1
fi
echo "AGENTROW OK $ROW"
