#!/usr/bin/env sh

#
# agents_accept.sh: the acceptance of the coding agents of the agents layer
# (vendor/gnu/agents/Readme.md) on an installed machine, run from the
# owner's computer over SSH once the owner has signed in to each program on
# it, and rehearsed in QEMU against the fake API server.
#
# Usage: agents_accept.sh lenovo USER [ADDRESS]
#        agents_accept.sh qemu KERNEL IMAGE DISK PORT APIPORT
#
# For each agent of RACH2_ACCEPT_AGENTS, `claude codex agy muse` unless it
# names fewer, the script makes a disposable repository on the machine,
# ~/rach2-accept/AGENT, from tests/system/fakeapi/repo: notes.txt asks for a
# change to greeting.txt, and check.sh is its test. It then works as a person
# does. Its own tmux, on this host, runs `ssh -tt` to the machine, and that
# attaches to a tmux session there, in which the account's login shell
# starts the agent. The script types into its own tmux, so that every key
# goes through SSH's terminal, and reads the agent's screen from the
# machine's tmux. It answers the questions of a first start and of a new
# directory, asks the agent to read notes.txt, do what it says, check the
# change and commit it, and requires a second commit whose greeting.txt
# check.sh passes, with nothing left uncommitted. It then asks for `sleep 600`
# and cancels the turn with Escape once the command runs, which must leave
# the agent at its prompt and no such command running; resizes its window,
# which the agent's terminal must follow; kills its SSH client, after which
# the session must lose its client and the agent go on, and attaches again;
# and ends the agent and reopens its conversation, which must show the
# cancelled request. While the agent runs, its process must run the
# program of the agents layer, /opt/AGENT/bin/AGENT.
#
# Before the sessions it requires each program's version to be the one its
# recipe pins, and keeps the program's SHA-256 and that of each credential
# file. It then reboots the machine as root and requires every credential
# file unchanged, every version and program unchanged, `gh auth status` to
# pass, and each agent to answer one request without its screen, which only
# a program still signed in can do. Last it sends its log to the machine,
# which counts there the strings of the credential files the log has; none
# may be in it. Only that count comes back. It removes ~/rach2-accept on the
# machine at the end, and refuses to start when that directory exists
# without the mark the script leaves in it.
#
# Each text the script reads from the machine, a screen, an answer or the
# output of a step, passes first through a filter there that replaces each
# string of a credential file with `[credential]`, so that the log has none
# on any path to the script's exit. Before the sessions the script requires
# the filter to remove every string of the credential files themselves.
#
# `lenovo` accepts the installed Lenovo as USER, the account the owner signed
# in with, at ADDRESS, RACH2_MACHINE_ADDRESS or 192.168.0.106, by the key
# RACH2_ACCEPT_KEY names or ssh's own. It reboots the machine as root by the
# development key, krffs/rach2_demo_ed25519, and only after the machine has
# reported the Lenovo's station address, RACH2_MACHINE_MAC or
# 28:d2:44:7e:e0:af. The credential files are the sign-ins the manual pages
# name: ~/.claude/.credentials.json, ~/.codex/auth.json,
# ~/.config/muse/auth.json and ~/.config/gh/hosts.yml; Antigravity CLI keeps
# its own in a file it does not document, so its answer after the reboot is
# its check. The log is build/images/agents-accept-lenovo.log.
#
# `qemu` is the rehearsal, which `tests/run agentsaccept` runs: it boots
# KERNEL with `rach2.root=ext2` on DISK, a copy of IMAGE, the GNU root with
# every package of the agents layer, as the agents suite does, with SSH on
# the host's PORT and no route out but the fake API server on APIPORT. The
# account is the test account, by the test key. `guest.sh setup` gives it
# dummy keys and settings that point each agent at the server, and the
# server plays tests/system/accept/agents/AGENT.json: its `session` while
# the agent's steps run, and its `after` for its request after the reboot,
# each of which must pass, every step played and no request it does not
# have. The credential files are the dummy keys and the token `setup` gives
# gh. The log is DISK.log, and the console's DISK.console.
#
# It prints a line for each step, `agents-accept: PASS` and exits 0, or
# `agents-accept: FAIL (N)` and exits 1; a usage error exits 2.
#

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
# shellcheck source=tests/system/deadline.sh
. "$REPO/tests/system/deadline.sh"

MODE=${1:-}
case $MODE in
    lenovo)
        [ $# -ge 2 ] || { echo "usage: agents_accept.sh lenovo USER [ADDRESS]" >&2; exit 2; }
        USERNAME=$2
        HOST=${3:-${RACH2_MACHINE_ADDRESS:-192.168.0.106}}
        PORT=22
        KEY=${RACH2_ACCEPT_KEY:-}
        mkdir -p "$REPO/build/images"
        LOG=$REPO/build/images/agents-accept-lenovo.log
        ;;
    qemu)
        [ $# -eq 6 ] ||
            { echo "usage: agents_accept.sh qemu KERNEL IMAGE DISK PORT APIPORT" >&2; exit 2; }
        KERNEL=$2
        IMAGE=$3
        DISK=$4
        PORT=$5
        APIPORT=$6
        USERNAME="test"
        HOST=127.0.0.1
        KEY=$REPO/krffs/rach2_test_ed25519
        LOG=$DISK.log
        ;;
    *)
        echo "usage: agents_accept.sh lenovo USER [ADDRESS]" >&2
        echo "       agents_accept.sh qemu KERNEL IMAGE DISK PORT APIPORT" >&2
        exit 2
        ;;
esac
AGENTS=${RACH2_ACCEPT_AGENTS:-claude codex agy muse}
for agent in $AGENTS; do
    case $agent in
        claude | codex | agy | muse) ;;
        *) echo "agents_accept.sh: no agent named $agent" >&2; exit 2 ;;
    esac
done
ROOT_KEY=$REPO/krffs/rach2_demo_ed25519
MAC=${RACH2_MACHINE_MAC:-28:d2:44:7e:e0:af}
PACKAGES=$REPO/vendor/gnu/agents/packages
FAKEAPI=$REPO/tests/system/fakeapi/fakeapi.py
SCRIPTS=$REPO/tests/system/accept/agents
CANCEL="Run the shell command sleep 600 in the foreground and wait for it to finish."
WORK_PROMPT="Read notes.txt and do what it says. Check the change with check.sh, then commit it."

work=""
HOME_M=""
QPID=""
SPID=""
SERVER_STATUS=""
FAILS=0

# The host's own tmux and the master SSH connection that the commands share;
# both live in the scratch directory, whose path stays short enough for a
# socket's name.
h() {
    tmux -S "$work/tmux" "$@"
}

# m COMMAND: run COMMAND on the machine as the account, over the master
# connection, which the first call opens, with no input.
m() {
    mi "$@" < /dev/null
}

# mi COMMAND: run COMMAND as m does, with this script's input as its own.
mi() {
    ssh -p "$PORT" ${KEY:+-i "$KEY"} -o BatchMode=yes -o StrictHostKeyChecking=no \
        -o "UserKnownHostsFile=$work/known" -o "ConnectTimeout=$(scaled 30)" \
        -o ServerAliveInterval=15 -o ServerAliveCountMax=4 -o LogLevel=ERROR \
        -o ControlMaster=auto -o "ControlPath=$work/ssh" -o ControlPersist=yes \
        "$USERNAME@$HOST" "$@"
}

# root COMMAND: run COMMAND on the machine as root, by the development key.
root() {
    ssh -n -p "$PORT" -i "$ROOT_KEY" -o BatchMode=yes -o StrictHostKeyChecking=no \
        -o "UserKnownHostsFile=$work/known" -o "ConnectTimeout=$(scaled 30)" \
        -o ServerAliveInterval=15 -o ServerAliveCountMax=4 -o LogLevel=ERROR \
        "root@$HOST" "$@"
}

# say TEXT: print TEXT as a line of the log.
say() {
    echo "agents-accept: $*" | tee -a "$LOG"
}

# bad TEXT: count a failure and say what it was.
bad() {
    FAILS=$((FAILS + 1))
    say "FAIL: $*"
}

# record WHAT TEXT: put TEXT in the log alone, under the heading WHAT.
record() {
    { echo "agents-accept: $1:"; printf '%s\n' "$2" | sed '/^ *$/d; s/^/    /'; } >> "$LOG"
}

# stop_server: stop the fake server, which writes its verdict as it ends, and
# keep its status in SERVER_STATUS.
stop_server() {
    spid=$SPID
    SPID=""
    if [ -n "$spid" ]; then
        kill -TERM "$spid" 2> /dev/null || true
        wait "$spid" 2> /dev/null
        SERVER_STATUS=$?
    fi
}

# Stop the machine and the server, end the agents' sessions on the machine,
# close the host's tmux and the master connection, and remove the disk and
# the scratch directory.
# shellcheck disable=SC2329 # the traps call it
cleanup() {
    stop_server
    if [ -n "$work" ]; then
        [ -z "$HOME_M" ] || m 'tmux -L rach2-accept kill-server' 2> /dev/null
        h kill-server 2> /dev/null
        ssh -o "ControlPath=$work/ssh" -O exit "$USERNAME@$HOST" 2> /dev/null
    fi
    if [ -n "$QPID" ]; then
        kill -9 "$QPID" 2> /dev/null
        wait "$QPID" 2> /dev/null
        QPID=""
    fi
    [ "$MODE" != qemu ] || rm -f "$DISK"
    [ -z "$work" ] || rm -rf "$work"
}
trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

# give_up REASON: say why the acceptance cannot go on, and exit 1.
give_up() {
    bad "$1"
    say "FAIL ($FAILS)"
    exit 1
}

: > "$LOG"
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-accept.XXXXXX") || exit 1

# The agent's own words and keys: its prompt, by a text the screen shows only
# while the agent waits for a request; the text it shows only while it works,
# and any it shows while it starts, when it may yet ask a question;
# the mark before the selected answer of a question; how it ends; what more it
# needs after a cancelled turn; and the menu it shows when nobody is signed in.
agent_set() {
    LOADING=""
    case $1 in
        claude)
            READY="bypass permissions on"
            BUSY="esc to interrupt"
            MARK=$(printf '\342\235\257')
            QUIT=/exit
            AFTER_CANCEL=""
            SIGNIN="Select login method"
            ;;
        codex)
            READY="? for shortcuts"
            BUSY="esc to interrupt"
            LOADING="loading"
            MARK=$(printf '\342\200\272')
            QUIT=/quit
            AFTER_CANCEL=/stop
            SIGNIN="Sign in with ChatGPT"
            ;;
        agy)
            READY="? for shortcuts"
            BUSY="esc to cancel"
            MARK=">"
            QUIT=/exit
            AFTER_CANCEL=""
            SIGNIN="Google OAuth"
            ;;
        muse)
            READY="YOLO"
            BUSY="esc to interrupt"
            MARK=$(printf '\342\235\257')
            QUIT=/exit
            AFTER_CANCEL=""
            SIGNIN="Log in with browser"
            ;;
    esac
}

# version_line PROGRAM VERSION: print the first line PROGRAM's --version
# prints when it is VERSION, as each program words it; gh's goes on with the
# date of its release.
version_line() {
    case $1 in
        claude) echo "$2 (Claude Code)" ;;
        codex) echo "codex-cli $2" ;;
        agy) echo "$2" ;;
        muse) echo "Muse Code ${2%%-*} ($2)" ;;
        gh) echo "gh version $2 (*" ;;
    esac
}

# screen [all]: print the agent's screen in the machine's tmux, or with `all`
# its whole history.
screen() {
    guest screen "$agent" "$@" 2> /dev/null
}

# keys KEY...: type KEY in the host's window of the agent, into SSH's terminal.
keys() {
    h send-keys -t "accept:$agent" "$@"
}

# shows TEXT: succeed when the last screen read, SCREEN, has TEXT.
shows() {
    case $SCREEN in
        *"$1"*) return 0 ;;
    esac
    return 1
}

# wait_for WHAT TEXT SECONDS: read the screen every two seconds until it
# shows TEXT, for at most SECONDS; otherwise fail WHAT.
wait_for() {
    end=$(($(date +%s) + $3))
    while :; do
        SCREEN=$(screen)
        shows "$2" && return 0
        if [ "$(date +%s)" -ge "$end" ]; then
            record "$agent: the screen" "$SCREEN"
            bad "$agent: $1: in $3 s the screen showed no [$2]"
            return 1
        fi
        # interval: the screen is read every two seconds
        sleep 2
    done
}

# choose ANSWER KEY: press KEY until the question has ANSWER selected for
# six seconds, then Enter. A question can refuse keys for a moment after it
# opens and then be drawn again with its first answer selected, so KEY is
# pressed again while ANSWER is not selected, at most every six seconds. A
# question answered a moment before can still be on the screen; once ANSWER
# has gone from it, there is nothing to choose.
choose() {
    n=0
    held=0
    while [ "$n" -lt 30 ]; do
        SCREEN=$(screen)
        shows "$1" || return 0
        if shows "$MARK $1"; then
            held=$((held + 1))
            if [ "$held" -ge 3 ]; then
                keys Enter
                return 0
            fi
        else
            held=0
            [ $((n % 3)) != 0 ] || keys "$2"
        fi
        n=$((n + 1))
        # interval: the screen is read every two seconds
        sleep 2
    done
    record "$agent: the screen" "$SCREEN"
    bad "$agent: the answer [$1] was not selected"
    return 1
}

# press KEY TEXT: press KEY until the screen has shown TEXT for six seconds.
press() {
    n=0
    held=0
    while [ "$n" -lt 30 ]; do
        SCREEN=$(screen)
        if shows "$2"; then
            held=$((held + 1))
            [ "$held" -lt 3 ] || return 0
        else
            held=0
            [ $((n % 3)) != 0 ] || keys "$1"
        fi
        n=$((n + 1))
        # interval: the screen is read every two seconds
        sleep 2
    done
    record "$agent: the screen" "$SCREEN"
    bad "$agent: [$2] did not follow $1"
    return 1
}

# leave TEXT: press Enter until the screen no longer shows TEXT.
leave() {
    n=0
    while [ "$n" -lt 30 ]; do
        SCREEN=$(screen)
        shows "$1" || return 0
        [ $((n % 5)) != 0 ] || keys Enter
        n=$((n + 1))
        # interval: the screen is read every two seconds
        sleep 2
    done
    record "$agent: the screen" "$SCREEN"
    bad "$agent: Enter did not take the screen past [$1]"
    return 1
}

# question: answer the question of a first start or of a new directory that
# SCREEN shows, as a person accepting the agent would; fail when it shows
# none. Antigravity CLI offers its consent to Google's use of the person's
# interactions given, which the script withholds.
question() {
    case $agent.$SCREEN in
        claude.*"Choose the text style"*) leave "Choose the text style" ;;
        claude.*"Do you want to use this API key?"*) choose Yes Up ;;
        claude.*"Press Enter to continue"*) leave "Press Enter to continue" ;;
        claude.*"Yes, I trust this folder"*) choose "Yes, I trust this folder" Down ;;
        claude.*"Yes, I accept"*) choose "Yes, I accept" Down ;;
        codex.*"Trust and continue"*) choose "1. Trust and continue" Up ;;
        codex.*"Use existing model"*) choose "2. Use existing model" Down ;;
        agy.*"Choose your color scheme"*) leave "Choose your color scheme" ;;
        agy.*"Terms of Service & Data Use"*)
            press Enter "[ ] Yes, I agree" && press Down ">  Previous" &&
                press Right ">  Done" && leave "Terms of Service & Data Use"
            ;;
        agy.*"Yes, I trust this folder"*) leave "Yes, I trust this folder" ;;
        muse.*"Do you trust this workspace?"*) leave "Do you trust this workspace?" ;;
        *) return 1 ;;
    esac
}

# idle WHAT SECONDS: answer the agent's questions until it waits for a
# request, its prompt on three screens read in a row with no sign of work or
# of its start, for at most SECONDS; otherwise fail WHAT. A sign-in menu
# fails at once.
idle() {
    end=$(($(date +%s) + $2))
    calm=0
    while :; do
        SCREEN=$(screen)
        if shows "$SIGNIN"; then
            record "$agent: the screen" "$SCREEN"
            bad "$agent: $1: the agent is not signed in"
            return 1
        fi
        if question; then
            calm=0
            end=$(($(date +%s) + $2))
            continue
        fi
        if shows "$READY" && ! shows "$BUSY" && { [ -z "$LOADING" ] || ! shows "$LOADING"; }; then
            calm=$((calm + 1))
            [ "$calm" -lt 3 ] || return 0
        else
            calm=0
        fi
        if [ "$(date +%s)" -ge "$end" ]; then
            record "$agent: the screen" "$SCREEN"
            bad "$agent: $1: no prompt in $2 s"
            return 1
        fi
        # interval: the screen is read every two seconds
        sleep 2
    done
}

# send WHAT TEXT TAIL CHECK: type TEXT in the agent's window and, once the
# screen shows TAIL, press Enter, and again every 20 s until the function
# CHECK succeeds, for at most 120 s; otherwise fail WHAT. A program that is
# still starting can take the text and lose the Enter.
send() {
    keys -l "$2" && wait_for "$1" "$3" 120 || return 1
    end=$(($(date +%s) + $(scaled 120)))
    while :; do
        keys Enter
        n=0
        while [ "$n" -lt 10 ]; do
            # interval: the screen is read every two seconds
            sleep 2
            SCREEN=$(screen)
            "$4" && return 0
            n=$((n + 1))
        done
        if [ "$(date +%s)" -ge "$end" ]; then
            record "$agent: the screen" "$SCREEN"
            bad "$agent: $1: Enter did not start it in $(scaled 120) s"
            return 1
        fi
    done
}

# The checks of send: the work began, the command runs, the command is gone,
# and the agent has ended, its process gone from the session.
# shellcheck disable=SC2329 # send calls it
working() {
    shows "$BUSY" || [ "$(guest work "$agent" | head -n 1)" != "commits 1" ]
}
# shellcheck disable=SC2329 # send calls it
sleeping() {
    shows "$BUSY" || [ -n "$(guest sleeps "$agent")" ]
}
# shellcheck disable=SC2329 # send calls it
stopped() {
    [ -z "$(guest sleeps "$agent")" ]
}
# shellcheck disable=SC2329 # send calls it
ended() {
    [ -z "$(guest pid "$agent")" ]
}

# guest COMMAND...: run guest.sh COMMAND on the machine as the account.
guest() {
    m "bash ~/rach2-accept/guest.sh $*"
}

# until_guest WHAT SECONDS WANT COMMAND...: run guest.sh COMMAND every two
# seconds until it prints WANT, for at most SECONDS; otherwise fail WHAT.
until_guest() {
    what=$1
    end=$(($(date +%s) + $2))
    want=$3
    shift 3
    while :; do
        OUT=$(guest "$@")
        [ "$OUT" != "$want" ] || return 0
        if [ "$(date +%s)" -ge "$end" ]; then
            bad "$agent: $what: got [$OUT], want [$want]"
            return 1
        fi
        # interval: the machine is asked every two seconds
        sleep 2
    done
}

# attach: start the host's window of the agent, whose SSH client attaches to
# the agent's session on the machine, made the first time.
attach() {
    # shellcheck disable=SC2016 # the quotes are the client's, around the path
    client="exec ssh -tt -p $PORT ${KEY:+-i '$KEY'} -o BatchMode=yes -o StrictHostKeyChecking=no"
    client="$client -o 'UserKnownHostsFile=$work/known' -o ControlPath=none"
    client="$client -o ServerAliveInterval=15 -o ServerAliveCountMax=4 -o LogLevel=ERROR"
    client="$client $USERNAME@$HOST tmux -u -L rach2-accept new-session -A -s rach2-accept-$agent"
    if h list-windows -F '#{window_name}' | grep -qx "$agent"; then
        h respawn-pane -k -t "accept:$agent" "$client"
    else
        h new-window -d -t accept -n "$agent" "$client"
    fi
}

# step_session: the agent's work in its screen, from the repository to the
# reopened conversation.
step_session() {
    guest prepare "$agent" > /dev/null || { bad "$agent: the repository"; return 1; }
    attach
    until_guest "the client attached" "$(scaled 60)" "120x39 120x39 1" pane "$agent" || return 1
    keys -l "bash ~/rach2-accept/guest.sh run $agent start" && keys Enter
    idle "the first prompt" "$(scaled 600)" || return 1
    say "$agent: the screen waits for a request: OK"
    OUT=$(guest exe "$agent")
    [ "$OUT" = "/opt/$agent/bin/$agent" ] ||
        bad "$agent: the process runs [$OUT], not /opt/$agent/bin/$agent"

    send "the request" "$WORK_PROMPT" "then commit it." working || return 1
    end=$(($(date +%s) + $(scaled 900)))
    while :; do
        OUT=$(guest work "$agent")
        case $OUT in
            "commits 2"*) idle "the end of the turn" "$(scaled 300)" && break ;;
        esac
        if [ "$(date +%s)" -ge "$end" ]; then
            record "$agent: the screen" "$(screen)"
            bad "$agent: no commit in $(scaled 900) s: $(printf '%s' "$OUT" | tr '\n' ' ')"
            return 1
        fi
        # interval: the repository is read every five seconds
        sleep 5
    done
    OUT=$(guest work "$agent")
    record "$agent: the repository" "$OUT"
    for want in "commits 2" "greeting hello, rach2" "check check: ok" "status "; do
        printf '%s\n' "$OUT" | grep -qxF "$want" || bad "$agent: the repository has no [$want]"
    done
    say "$agent: read, changed, tested and committed: $(printf '%s\n' "$OUT" | sed -n 's/^subject //p')"

    send "the request" "$CANCEL" "to finish." sleeping || return 1
    until_guest_some "the command" "$(scaled 300)" sleeps "$agent" || return 1
    keys Escape
    idle "the cancelled turn" "$(scaled 120)" || return 1
    if [ -n "$AFTER_CANCEL" ]; then
        send "$AFTER_CANCEL" "$AFTER_CANCEL" "$AFTER_CANCEL" stopped || return 1
        idle "$AFTER_CANCEL" "$(scaled 120)" || return 1
    fi
    until_guest "the command after the cancel" "$(scaled 60)" "" sleeps "$agent" || return 1
    [ -n "$(guest pid "$agent")" ] || bad "$agent: the agent ended with its cancelled turn"
    say "$agent: cancelled: OK"

    h resize-window -t "accept:$agent" -x 100 -y 30
    until_guest "the resized terminal" "$(scaled 60)" "100x29 100x29 1" pane "$agent" &&
        idle "the resized screen" "$(scaled 60)" && say "$agent: resized to 100x30: OK"
    h resize-window -t "accept:$agent" -x 120 -y 40
    until_guest "the terminal's size again" "$(scaled 60)" "120x39 120x39 1" pane "$agent" ||
        return 1

    pid=$(h display-message -p -t "accept:$agent" '#{pane_pid}')
    kill -9 "$pid"
    until_guest "the session without its client" "$(scaled 60)" "120x39 120x39 0" pane "$agent" ||
        return 1
    [ -n "$(guest pid "$agent")" ] || bad "$agent: the agent ended with its SSH connection"
    attach
    until_guest "the client attached again" "$(scaled 60)" "120x39 120x39 1" pane "$agent" &&
        idle "the screen after the new connection" "$(scaled 60)" &&
        say "$agent: reconnected: OK"

    quit || return 1
    keys -l "bash ~/rach2-accept/guest.sh run $agent resume" && keys Enter
    idle "the reopened conversation" "$(scaled 300)" || return 1
    OUT=$(screen all | awk '/rach2-accept: ended with status/ { text = "" } { text = text $0 "\n" }
        END { printf "%s", text }')
    case $OUT in
        *"sleep 600"*) say "$agent: reopened the conversation: OK" ;;
        *)
            record "$agent: the screen" "$OUT"
            bad "$agent: the reopened screen has no [sleep 600] of the cancelled request"
            ;;
    esac
    quit
}

# quit: end the agent with its command, and require its end to come with
# status 0.
quit() {
    send "$QUIT" "$QUIT" "$QUIT" ended || return 1
    ends=$(screen all | grep 'rach2-accept: ended with status')
    [ "$(printf '%s\n' "$ends" | tail -n 1)" = "rach2-accept: ended with status 0" ] ||
        bad "$agent: $QUIT ended it with [$(printf '%s\n' "$ends" | tail -n 1)]"
}

# until_guest_some WHAT SECONDS COMMAND...: run guest.sh COMMAND every two
# seconds until it prints something, for at most SECONDS; otherwise fail WHAT.
until_guest_some() {
    what=$1
    end=$(($(date +%s) + $2))
    shift 2
    while :; do
        OUT=$(guest "$@")
        [ -z "$OUT" ] || return 0
        if [ "$(date +%s)" -ge "$end" ]; then
            record "$agent: the screen" "$(screen)"
            bad "$agent: $what did not start in $2 s"
            return 1
        fi
        # interval: the machine is asked every two seconds
        sleep 2
    done
}

# serve AGENT PART: play PART of the agent's script for the fake server,
# with its repository at the account's ~/rach2-accept/AGENT, in the QEMU
# mode; nothing on the Lenovo.
serve() {
    [ "$MODE" = qemu ] || return 0
    python3 -B -c 'import json, sys
script = json.load(open(sys.argv[1]))[sys.argv[2]]
text = json.dumps(script).replace("@REPO@", sys.argv[3])
open(sys.argv[4], "w").write(text)' "$SCRIPTS/$1.json" "$2" "$HOME_M/rach2-accept/$1" \
        "$work/$1.$2.json" || give_up "the server's script $1.$2"
    python3 -B "$FAKEAPI" serve "$work/$1.$2.json" "$DISK.$1.$2.record" "$work/ca" "$APIPORT" \
        > /dev/null 2> "$DISK.$1.$2.server" &
    SPID=$!
    n=0
    until grep -q "fakeapi: listening on" "$DISK.$1.$2.server"; do
        n=$((n + 1))
        [ "$n" -lt 30 ] || give_up "the server did not listen for $1: $(cat "$DISK.$1.$2.server")"
        # interval: the server's log is read once a second
        sleep 1
    done
}

# unserve AGENT PART: stop the fake server and require its verdict to pass.
unserve() {
    [ "$MODE" = qemu ] || return 0
    stop_server
    if [ "$SERVER_STATUS" = 0 ]; then
        say "$1: the fake server played the $2 script: OK"
    else
        bad "$1: the fake server's verdict on the $2 script: $(tail -n 1 "$DISK.$1.$2.server")"
    fi
}

# boot: start the QEMU mode's machine, with the fake server's names in its
# /etc/hosts, its authority in the account's ~/rach2-accept/ca.pem, and the
# account's settings for the fake server.
boot() {
    RUNUTIL=$REPO/build/tests/system/runutil
    MKMBR=$REPO/build/tools/image/mkmbr
    hosts=$(python3 -B -c 'import json, sys
print(" ".join(sorted({n for f in sys.argv[1:] for n in json.load(open(f))["hosts"]})))' \
        "$SCRIPTS"/*.json) || give_up "the fake server's names"
    # shellcheck disable=SC2086 # the names are one word each
    python3 -B "$FAKEAPI" ca "$work/ca" $hosts > "$work/ca.log" 2>&1 ||
        give_up "the certificate authority: $(cat "$work/ca.log")"
    size=$(($(wc -c < "$IMAGE") / 1048576))
    rm -f "$DISK"
    if ! dd if=/dev/zero of="$DISK" bs=1048576 count=0 seek=$((size + 2)) 2> /dev/null ||
        ! dd if="$IMAGE" of="$DISK" bs=1048576 seek=1 conv=notrunc,sparse 2> /dev/null ||
        ! "$MKMBR" --lba "$DISK" "2048:$((size * 2048)):83" > /dev/null; then
        give_up "the disk"
    fi
    "${RACH2_QEMU:-qemu-system-x86_64}" -cpu max -m 4096 -display none -kernel "$KERNEL" \
        -append "rach2.root=ext2" \
        -device ich9-ahci,id=ahci0 \
        -drive "if=none,id=drv0,file=$DISK,format=raw" -device ide-hd,drive=drv0,bus=ahci0.0 \
        -netdev "user,id=n0,restrict=on,hostfwd=tcp::$PORT-:22,guestfwd=tcp:10.0.2.100:443-cmd:python3 -B $FAKEAPI relay $APIPORT" \
        -device rtl8139,netdev=n0 \
        -serial stdio < /dev/null > "$DISK.console" 2>&1 &
    QPID=$!
    "$RUNUTIL" until "$DISK.console" "init: sshd: started as pid" "$(scaled 400)" ||
        give_up "init did not start the SSH server; the console: $DISK.console"
    answer "$(scaled 900)" || give_up "no SSH answer from the machine"
    lines=""
    for name in $hosts; do
        lines="${lines}10.0.2.100 $name\\n"
    done
    root "printf '$lines' >> /etc/hosts" || give_up "the fake server's names in /etc/hosts"
    say "the machine boots, with the fake server's names: OK"
}

# answer SECONDS: wait until the account logs in over SSH, for at most
# SECONDS, through a new master connection.
answer() {
    ssh -o "ControlPath=$work/ssh" -O exit "$USERNAME@$HOST" 2> /dev/null
    end=$(($(date +%s) + $1))
    until m true 2> /dev/null; do
        [ "$(date +%s)" -lt "$end" ] || return 1
        # interval: a try of SSH every two seconds until the server answers
        sleep 2
    done
}

say "accepting the agents $AGENTS on $USERNAME@$HOST, in the mode $MODE"
if [ "$MODE" = qemu ]; then
    [ "${RACH2_ARCH:-x86_64}" = x86_64 ] || give_up "the agents layer is x86-64 only"
    boot
else
    answer 30 || give_up "no SSH answer from $USERNAME@$HOST"
fi
# shellcheck disable=SC2016 # the machine's shell expands it
HOME_M=$(m 'printf %s "$HOME"') || give_up "the account's home"
OUT=$(m 'if [ -d ~/rach2-accept ] && [ ! -f ~/rach2-accept/.rach2-accept ]; then echo foreign; fi')
[ -z "$OUT" ] || give_up "$HOME_M/rach2-accept is not this script's: move it away first"
m 'tmux -L rach2-accept kill-server' 2> /dev/null
if ! mi 'rm -rf ~/rach2-accept && mkdir -p ~/rach2-accept/fixture &&
    touch ~/rach2-accept/.rach2-accept && cat > ~/rach2-accept/guest.sh' \
    < "$REPO/tests/system/accept/agents_guest.sh"; then
    give_up "the script's files on the machine"
fi
for file in "$REPO"/tests/system/fakeapi/repo/*; do
    mi "cat > ~/rach2-accept/fixture/${file##*/}" < "$file" ||
        give_up "the script's files on the machine"
done
if [ "$MODE" = qemu ]; then
    if ! mi 'cat > ~/rach2-accept/ca.pem' < "$work/ca/ca.pem" || ! guest setup; then
        give_up "the settings for the fake server"
    fi
fi

# The versions and programs, and the credential files, before the reboot.
for program in $AGENTS gh; do
    version=$(sed -n 's/^VERSION=//p' "$PACKAGES/$program.sh")
    want=$(version_line "$program" "$version")
    OUT=$(guest version "$program")
    printf '%s\n' "$OUT" > "$work/version.$program"
    # shellcheck disable=SC2254 # gh's line is a pattern
    case $(printf '%s\n' "$OUT" | head -n 1) in
        $want) say "$program: the version is $version: OK" ;;
        *) bad "$program: the version is [$(printf '%s\n' "$OUT" | head -n 1)], not $version" ;;
    esac
    case $(printf '%s\n' "$OUT" | sed -n 2p) in
        *" /opt/$program/bin/$program") ;;
        *) bad "$program: the path finds [$(printf '%s\n' "$OUT" | sed -n 2p)]" ;;
    esac
done
guest credentials "$MODE" > "$work/credentials.before" || give_up "the credential files"
while read -r file digest; do
    [ "$digest" != missing ] || bad "no credential file $file"
done < "$work/credentials.before"
OUT=$(guest filtered "$MODE")
case $OUT in
    "0 of "[1-9]*) say "the machine's filter removes every credential string: OK" ;;
    *) give_up "the machine's filter leaves [$OUT] of the credential files' strings" ;;
esac

h new-session -d -s accept -x 120 -y 40 -n script
h set-option -g remain-on-exit on
h set-option -g default-terminal tmux-256color
for agent in $AGENTS; do
    agent_set "$agent"
    say "$agent: the session"
    serve "$agent" session
    step_session
    h kill-window -t "accept:$agent" 2> /dev/null
    m "tmux -L rach2-accept kill-session -t rach2-accept-$agent" 2> /dev/null
    unserve "$agent" session
done

# The reboot, as root; on the Lenovo only after the machine has said it is
# the Lenovo.
if [ "$MODE" = lenovo ]; then
    OUT=$(root 'cat /sys/class/net/eth0/address') || give_up "root's login by the development key"
    [ "$OUT" = "$MAC" ] || give_up "the machine's station address is $OUT, not the Lenovo's $MAC"
fi
up=$(m 'cut -d . -f 1 /proc/uptime')
root 'setsid sh -c "sleep 2; reboot" > /dev/null 2>&1 < /dev/null &' ||
    give_up "the reboot was not started"
end=$(($(date +%s) + $(scaled 900)))
while :; do
    # interval: the machine is asked every ten seconds whether it is up again
    sleep 10
    if answer "$(scaled 60)"; then
        now=$(m 'cut -d . -f 1 /proc/uptime' 2> /dev/null)
        [ -z "$now" ] || [ "$now" -ge "$up" ] || break
    fi
    [ "$(date +%s)" -lt "$end" ] || give_up "the machine did not come back from its reboot"
done
say "the machine rebooted: OK"

guest credentials "$MODE" > "$work/credentials.after" || give_up "the credential files after the reboot"
if cmp -s "$work/credentials.before" "$work/credentials.after"; then
    say "the credential files are as they were before the reboot: OK"
else
    bad "the credential files changed in the reboot: $(cut -d ' ' -f 1 "$work/credentials.after" |
        tr '\n' ' ')"
fi
for program in $AGENTS gh; do
    OUT=$(guest version "$program")
    if [ "$OUT" = "$(cat "$work/version.$program")" ]; then
        say "$program: the version and the program are as they were: OK"
    else
        bad "$program: changed: [$(printf '%s' "$OUT" | tr '\n' ' ')]"
    fi
    agent=$program
    serve "$program" after
    OUT=$(guest run "$program" once 2>&1)
    record "$program: its answer after the reboot" "$OUT"
    case $program.$OUT in
        gh.*"rach2-accept: ended with status 0") say "gh: signed in after the reboot: OK" ;;
        gh.*) bad "gh: gh auth status failed after the reboot" ;;
        *"rach2-accept: ended with status 0")
            if printf '%s\n' "$OUT" | grep -qi "signed in"; then
                say "$program: answered a request after the reboot: OK"
            else
                bad "$program: its answer after the reboot does not say it is signed in"
            fi
            ;;
        *) bad "$program: no answer after the reboot: $(printf '%s\n' "$OUT" | tail -n 1)" ;;
    esac
    unserve "$program" after
done

OUT=$(mi "bash ~/rach2-accept/guest.sh secrets $MODE" < "$LOG")
case $OUT in
    "0 of "*) say "the log has none of the credential files' strings: OK" ;;
    *) bad "the log has $OUT of the credential files' strings" ;;
esac
m 'rm -rf ~/rach2-accept' || bad "$HOME_M/rach2-accept was not removed"
if [ "$FAILS" -eq 0 ]; then
    say "PASS"
    exit 0
fi
say "FAIL ($FAILS)"
exit 1
