#!/usr/bin/env sh

#
# agents_accept.sh: the acceptance of the coding agents of the agents layer
# (vendor/gnu/agents/Readme.md) on an installed machine, run from the
# owner's computer over SSH once the owner has signed in to each program on
# it, and rehearsed in QEMU against the fake API server.
#
# Usage: agents_accept.sh lenovo USER [ADDRESS]
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
# recipe pins, and keeps the program's SHA-256 and that of each selected
# agent's credential file and gh's. It reboots as root and requires each
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
# ~/.gemini/antigravity-cli/antigravity-oauth-token, ~/.config/muse/auth.json
# and ~/.config/gh/hosts.yml. The log is build/images/agents-accept-lenovo.log.
#
# Etude calls the internal `etude ACTION AGENT` mode on private GNU roots.
# Its scenario boots the machine and starts and stops each API script; these
# actions run the same SSH, session and credential assertions as on Lenovo.
# Waits end on session state, with no completion deadline in this script.
# Etude supervises each action with its existing watchdog and case bounds.
#
# It prints a line for each step, `agents-accept: PASS` and exits 0, or
# `agents-accept: FAIL (N)` and exits 1; a usage error exits 2.
#

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"

MODE=${1:-}
case $MODE in
    lenovo)
        [ $# -ge 2 ] || { echo "usage: agents_accept.sh lenovo USER [ADDRESS]" >&2; exit 2; }
        USERNAME=$2
        HOST=${3:-${RACH2_MACHINE_ADDRESS:-192.168.0.106}}
        PORT=22
        KEY=${RACH2_ACCEPT_KEY:-}
        AGENTS=${RACH2_ACCEPT_AGENTS:-claude codex agy muse}
        ROOT_KEY=$REPO/krffs/rach2_demo_ed25519
        mkdir -p "$REPO/build/images"
        LOG=$REPO/build/images/agents-accept-lenovo.log
        ;;
    etude)
        [ $# -eq 3 ] || exit 2
        GUEST_MODE=qemu
        ACTION=$2
        AGENTS=$3
        USERNAME="test"
        HOST=127.0.0.1
        PORT=${AGENT_PORT:?}
        KEY=${AGENT_TEST_KEY:?}
        ROOT_KEY=${AGENT_ROOT_KEY:?}
        LOG=${AGENT_WORK:?}/accept.log
        ;;
    *)
        echo "usage: agents_accept.sh lenovo USER [ADDRESS]" >&2
        exit 2
        ;;
esac
for agent in $AGENTS; do
    case $agent in
        claude | codex | agy | muse) ;;
        *) echo "agents_accept.sh: no agent named $agent" >&2; exit 2 ;;
    esac
done
MAC=${RACH2_MACHINE_MAC:-28:d2:44:7e:e0:af}
PACKAGES=$REPO/vendor/gnu/agents/packages
FAKEAPI=$REPO/tests/system/fakeapi/fakeapi.py
SCRIPTS=$REPO/tests/system/accept/agents
CANCEL="Run the shell command sleep 600 in the foreground and wait for it to finish."
WORK_PROMPT="Read notes.txt and do what it says. Check the change with check.sh, then commit it."

work=""
TMUX_PID=""
SOCKET_DIR=""
HOME_M=""
FAILS=0

# Use a relative socket name within the private directory of an etude case.
h() {
    if [ -n "$SOCKET_DIR" ]; then
        (cd "$SOCKET_DIR" && tmux -S tmux "$@")
    else
        tmux -S "$work/tmux" "$@"
    fi
}

# m COMMAND: run COMMAND as the account, over SSH, with no input.
m() {
    mi "$@" < /dev/null
}

# mi COMMAND: run COMMAND as m does, with this script's input as its own.
mi() {
    if [ "$MODE" = etude ]; then
        ssh_mode_options='-F /dev/null -o IdentitiesOnly=yes'
        ssh_control=none
    else
        ssh_mode_options='-o ControlMaster=auto -o ControlPersist=yes'
        ssh_control=$work/ssh
    fi
    # shellcheck disable=SC2086 # fixed SSH options, without paths
    ssh -p "$PORT" ${KEY:+-i "$KEY"} -o BatchMode=yes -o StrictHostKeyChecking=no \
        -o "UserKnownHostsFile=$work/known" \
        -o LogLevel=ERROR \
        $ssh_mode_options -o "ControlPath=$ssh_control" "$USERNAME@$HOST" "$@"
}

# root COMMAND: run COMMAND on the machine as root, by the development key.
root() {
    ssh_root_options=""
    [ "$MODE" != etude ] || ssh_root_options='-F /dev/null -o IdentitiesOnly=yes'
    # shellcheck disable=SC2086 # fixed SSH options, without paths
    ssh -n -p "$PORT" -i "$ROOT_KEY" -o BatchMode=yes -o StrictHostKeyChecking=no \
        -o "UserKnownHostsFile=$work/known" \
        -o LogLevel=ERROR \
        $ssh_root_options "root@$HOST" "$@"
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

# End host and guest terminal sessions and remove scratch on every exit.
# shellcheck disable=SC2329 # the traps call it
cleanup() {
    [ -z "$work" ] || h kill-server 2> /dev/null
    [ -z "$TMUX_PID" ] || wait "$TMUX_PID" 2>/dev/null
    [ -z "$SOCKET_DIR" ] || rm -rf "$SOCKET_DIR"
    if [ -n "$work" ]; then
        if [ "$MODE" = lenovo ] && [ -n "$HOME_M" ]; then
            m 'tmux -L rach2-accept kill-server' 2> /dev/null
        fi
        ssh -o "ControlPath=$work/ssh" -O exit "$USERNAME@$HOST" 2> /dev/null
        [ "$MODE" = etude ] || rm -rf "$work"
        work=""
    fi
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

if [ "$MODE" = etude ]; then
    work=${AGENT_WORK:?}
    mkdir -p "$work"
else
    : > "$LOG"
    work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-accept.XXXXXX") || exit 1
fi

# The agent's own words and keys: its prompt, by a text the screen shows only
# while the agent waits for a request; the text it shows only while it works,
# and any it shows while it starts, when it may yet ask a question;
# the mark before the selected answer of a question; how it ends; what more it
# needs after a cancelled turn; and the menu it shows when nobody is signed in.
# A fresh etude Codex home must select its model before its prompt is ready.
agent_set() {
    LOADING=""
    MODEL_SELECTED=yes
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
            [ "$MODE" != etude ] || MODEL_SELECTED=""
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

# settle KEY TEXT [QUESTION]: press KEY until the screen shows TEXT on two
# reads in a row, reading it every two seconds; return 0 then, or 2 when
# the screen no longer shows QUESTION. KEY is pressed on a screen without
# TEXT that is the same on two reads in a row and that the agent has drawn
# since KEY was last pressed. A question can refuse keys for a moment after
# it opens and then be drawn again, or take a key and draw its first answer
# again; either way the agent settles on a new screen without TEXT, and is
# given KEY again.
# A key whose effect a slow agent has yet to draw leaves the screen as it
# was, and is not pressed twice.
settle() {
    last=""
    pressed=""
    drawn=1
    shown=""
    while :; do
        SCREEN=$(screen)
        [ "$SCREEN" = "$pressed" ] || drawn=1
        shows "${3:-}" || return 2
        if shows "$2"; then
            [ -z "$shown" ] || return 0
            shown=1
        else
            shown=""
            if [ "$SCREEN" = "$last" ] && [ -n "$drawn" ]; then
                keys "$1"
                pressed=$SCREEN
                drawn=""
            fi
        fi
        last=$SCREEN
        # interval: the screen is read every two seconds
        sleep 2
    done
}

# choose ANSWER KEY: press KEY until the question has ANSWER selected, as
# `settle` does, then Enter. A question answered a moment before can still
# be on the screen; once ANSWER has gone from it, there is nothing to choose.
choose() {
    if settle "$2" "$MARK $1" "$1"; then
        keys Enter
    fi
    return 0
}

# press KEY TEXT: press KEY until the screen shows TEXT, as `settle` does.
press() {
    settle "$1" "$2" || { bad "$agent: [$2] did not follow $1"; return 1; }
}

# leave TEXT: press Enter until the screen no longer shows TEXT, reading it
# every two seconds. Enter is pressed on a screen that is the same on two
# reads in a row and that the agent has drawn since Enter was last pressed,
# as `settle` presses its key.
leave() {
    last=""
    pressed=""
    drawn=1
    while :; do
        SCREEN=$(screen)
        shows "$1" || return 0
        [ "$SCREEN" = "$pressed" ] || drawn=1
        if [ "$SCREEN" = "$last" ] && [ -n "$drawn" ]; then
            keys Enter
            pressed=$SCREEN
            drawn=""
        fi
        last=$SCREEN
        # interval: the screen is read every two seconds
        sleep 2
    done
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
        codex.*"Use existing model"*)
            choose "2. Use existing model" Down && MODEL_SELECTED=yes
            ;;
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

# idle WHAT: answer the agent's questions until it waits for a request,
# its prompt on two screens read in a row with no sign of work or of its
# start. A sign-in menu fails WHAT at once.
idle() {
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
            continue
        fi
        if [ -n "$MODEL_SELECTED" ] && shows "$READY" && ! shows "$BUSY" &&
            { [ -z "$LOADING" ] || ! shows "$LOADING"; }; then
            calm=$((calm + 1))
            [ "$calm" -lt 2 ] || return 0
        else
            calm=0
        fi
        # interval: the screen is read every two seconds
        sleep 2
    done
}

# send WHAT TEXT TAIL CHECK: type TEXT in the agent's window and, while the
# screen shows TAIL, press Enter, and again every 20 s until the function
# CHECK succeeds. WHAT describes the request in the log. A program that is
# still starting can lose an Enter, or ask a question after it has drawn its
# prompt. Such a question is answered when it shows, and TEXT is typed again
# when the screen no longer has it once the question has gone.
send() {
    say "$agent: sending $1"
    keys -l "$2" || return 1
    asked=""
    while :; do
        SCREEN=$(screen)
        if question; then
            asked=1
        elif shows "$3"; then
            keys Enter
            n=0
            while [ "$n" -lt 10 ]; do
                # interval: the screen is read every two seconds
                sleep 2
                SCREEN=$(screen)
                "$4" && return 0
                if question; then
                    asked=1
                    break
                fi
                n=$((n + 1))
            done
        elif [ -n "$asked" ]; then
            keys -l "$2"
            asked=""
        fi
        # interval: the screen is read every two seconds
        sleep 2
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

# until_guest WHAT WANT COMMAND...: wait for guest.sh COMMAND to print
# WANT, describing the wait as WHAT in the log.
until_guest() {
    what=$1
    want=$2
    shift 2
    say "$agent: waiting: $what"
    while :; do
        OUT=$(guest "$@")
        [ "$OUT" != "$want" ] || return 0
        # interval: the machine is asked every two seconds
        sleep 2
    done
}

# attach: start the host's window of the agent, whose SSH client attaches to
# the agent's session on the machine, made the first time.
attach() {
    # shellcheck disable=SC2016 # the quotes are the client's, around the path
    client="exec ssh -tt -p $PORT ${KEY:+-i '$KEY'} -o BatchMode=yes -o StrictHostKeyChecking=no"
    [ "$MODE" != etude ] || client="$client -F /dev/null -o IdentitiesOnly=yes"
    client="$client -o 'UserKnownHostsFile=$work/known' -o ControlPath=none"
    client="$client -o LogLevel=ERROR"
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
    until_guest "the client attached" "120x39 120x39 1" pane "$agent" || return 1
    keys -l "bash ~/rach2-accept/guest.sh run $agent start" && keys Enter
    idle "the first prompt" || return 1
    record "$agent: the first prompt" "$SCREEN"
    say "$agent: the screen waits for a request: OK"
    OUT=$(guest exe "$agent")
    [ "$OUT" = "/opt/$agent/bin/$agent" ] ||
        bad "$agent: the process runs [$OUT], not /opt/$agent/bin/$agent"

    send "the request" "$WORK_PROMPT" "then commit it." working || return 1
    while :; do
        OUT=$(guest work "$agent")
        case $OUT in
            "commits 2"*) idle "the end of the turn" && break ;;
        esac
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
    until_guest_some "the command" sleeps "$agent" || return 1
    keys Escape
    idle "the cancelled turn" || return 1
    if [ -n "$AFTER_CANCEL" ]; then
        send "$AFTER_CANCEL" "$AFTER_CANCEL" "$AFTER_CANCEL" stopped || return 1
        idle "$AFTER_CANCEL" || return 1
    fi
    until_guest "the command after the cancel" "" sleeps "$agent" || return 1
    [ -n "$(guest pid "$agent")" ] || bad "$agent: the agent ended with its cancelled turn"
    say "$agent: cancelled: OK"

    h resize-window -t "accept:$agent" -x 100 -y 30
    until_guest "the resized terminal" "100x29 100x29 1" pane "$agent" &&
        idle "the resized screen" && say "$agent: resized to 100x30: OK"
    h resize-window -t "accept:$agent" -x 120 -y 40
    until_guest "the terminal's size again" "120x39 120x39 1" pane "$agent" ||
        return 1

    pid=$(h display-message -p -t "accept:$agent" '#{pane_pid}')
    kill -9 "$pid"
    until_guest "the session without its client" "120x39 120x39 0" pane "$agent" ||
        return 1
    [ -n "$(guest pid "$agent")" ] || bad "$agent: the agent ended with its SSH connection"
    attach
    until_guest "the client attached again" "120x39 120x39 1" pane "$agent" &&
        idle "the screen after the new connection" &&
        say "$agent: reconnected: OK"

    quit || return 1
    keys -l "bash ~/rach2-accept/guest.sh run $agent resume" && keys Enter
    idle "the reopened conversation" || return 1
    while :; do
        SCREEN=$(screen)
        if question; then
            continue
        fi
        OUT=$(screen all | awk '/rach2-accept: ended with status/ { text = "" } { text = text $0 "\n" }
            END { printf "%s", text }')
        case $OUT in
            *"sleep 600"*) say "$agent: reopened the conversation: OK"; break ;;
        esac
        # interval: the reopened conversation is read every two seconds
        sleep 2
    done
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

# until_guest_some WHAT COMMAND...: wait for guest.sh COMMAND to print
# something, describing the wait as WHAT in the log.
until_guest_some() {
    what=$1
    shift
    say "$agent: waiting: $what"
    while :; do
        OUT=$(guest "$@")
        [ -z "$OUT" ] || return 0
        # interval: the machine is asked every two seconds
        sleep 2
    done
}

# answer: wait until the account logs in over a new master SSH connection.
answer() {
    ssh -o "ControlPath=$work/ssh" -O exit "$USERNAME@$HOST" 2> /dev/null
    until m true 2> /dev/null; do
        # interval: a try of SSH every two seconds until the server answers
        sleep 2
    done
}

# Stage the shared guest driver, versions and credentials for either mode.
acceptance_setup() {
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
if [ "$MODE" = etude ]; then
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
# shellcheck disable=SC2086 # AGENTS is the validated list of program names
guest credentials "${GUEST_MODE:-$MODE}" $AGENTS > "$work/credentials.before" || give_up "the credential files"
while read -r file digest; do
    [ "$digest" != missing ] || bad "no credential file $file"
done < "$work/credentials.before"
# shellcheck disable=SC2086 # AGENTS is the validated list of program names
OUT=$(guest filtered "${GUEST_MODE:-$MODE}" $AGENTS)
case $OUT in
    "0 of "[1-9]*) say "the machine's filter removes every credential string: OK" ;;
    *) give_up "the machine's filter leaves [$OUT] of the credential files' strings" ;;
esac
}

if [ "$MODE" = etude ]; then
    HOME_M=/home/test
    case $ACTION in
        prepare)
            hosts=$(python3 -B -c 'import json,sys; print(" ".join(sorted({n for f in sys.argv[1:] for n in json.load(open(f))["hosts"]})))' "$SCRIPTS/$AGENTS.json" "$SCRIPTS/gh.json") || exit 1
            # shellcheck disable=SC2086 # DNS names from the scripts
            python3 -B "$FAKEAPI" ca "$work/ca" $hosts || exit 1
            for part in session after; do
                python3 -B -c 'import json,sys; s=json.load(open(sys.argv[1]))[sys.argv[2]]; open(sys.argv[4],"w").write(json.dumps(s).replace("@REPO@",sys.argv[3]))' "$SCRIPTS/$AGENTS.json" "$part" "$HOME_M/rach2-accept/$AGENTS" "$work/$part.json" || exit 1
            done
            python3 -B -c 'import json,sys; json.dump(json.load(open(sys.argv[1]))["after"],open(sys.argv[2],"w"))' "$SCRIPTS/gh.json" "$work/gh.json" || exit 1
            ;;
        setup)
            hosts=$(python3 -B -c 'import json,sys; print(" ".join(sorted({n for f in sys.argv[1:] for n in json.load(open(f))["hosts"]})))' "$SCRIPTS/$AGENTS.json" "$SCRIPTS/gh.json") || exit 1
            lines=""
            for name in $hosts; do lines="${lines}10.0.2.100 $name\\n"; done
            root "printf '$lines' >> /etc/hosts" || give_up "the fake server's names"
            acceptance_setup
            ;;
        session)
            SOCKET_DIR=$(mktemp -d "${TMPDIR:-/tmp}/rach2-accept.XXXXXX") || exit 1
            (cd "$SOCKET_DIR" && exec tmux -f /dev/null -D -S tmux) > "$work/tmux.log" 2>&1 &
            TMUX_PID=$!
            # interval: wait for the foreground server's socket
            until [ -S "$SOCKET_DIR/tmux" ]; do kill -0 "$TMUX_PID" || exit 1; sleep 0.1; done
            h new-session -d -s accept -x 120 -y 40 -n script
            h set-option -g remain-on-exit on
            h set-option -g default-terminal tmux-256color
            agent=$AGENTS
    agent_set "$agent"
    say "$agent: the session"
    step_session
    h kill-window -t "accept:$agent" 2> /dev/null
    m "tmux -L rach2-accept kill-session -t rach2-accept-$agent" 2> /dev/null
            h kill-server 2>/dev/null
            wait "$TMUX_PID" || :
            TMUX_PID=""
            ;;
        credentials)
# shellcheck disable=SC2086 # AGENTS is the validated list of program names
guest credentials "${GUEST_MODE:-$MODE}" $AGENTS > "$work/credentials.after" || give_up "the credential files after the reboot"
if cmp -s "$work/credentials.before" "$work/credentials.after"; then
    say "the credential files are as they were before the reboot: OK"
else
    bad "the credential files changed in the reboot: $(cut -d ' ' -f 1 "$work/credentials.after" |
        tr '\n' ' ')"
fi
            ;;
        after|gh)
            if [ "$ACTION" = gh ]; then program=gh; else program=$AGENTS; fi
    OUT=$(guest version "$program")
    if [ "$OUT" = "$(cat "$work/version.$program")" ]; then
        say "$program: the version and the program are as they were: OK"
    else
        bad "$program: changed: [$(printf '%s' "$OUT" | tr '\n' ' ')]"
    fi
    agent=$program
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
            ;;
        finish)

OUT=$(mi "bash ~/rach2-accept/guest.sh secrets ${GUEST_MODE:-$MODE} $AGENTS" < "$LOG")
case $OUT in
    "0 of "*) say "the log has none of the credential files' strings: OK" ;;
    *) bad "the log has $OUT of the credential files' strings" ;;
esac
m 'rm -rf ~/rach2-accept' || bad "$HOME_M/rach2-accept was not removed"
            ;;
        *) exit 2 ;;
    esac
    [ "$FAILS" -eq 0 ] || exit 1
    echo "agents-accept: $ACTION: PASS"
    [ "$ACTION" != finish ] || echo "agents-accept: PASS"
    exit 0
fi

say "accepting the agents $AGENTS on $USERNAME@$HOST, in the mode $MODE"
answer
acceptance_setup

h new-session -d -s accept -x 120 -y 40 -n script
h set-option -g remain-on-exit on
h set-option -g default-terminal tmux-256color
for agent in $AGENTS; do
    agent_set "$agent"
    say "$agent: the session"
    step_session
    h kill-window -t "accept:$agent" 2> /dev/null
    m "tmux -L rach2-accept kill-session -t rach2-accept-$agent" 2> /dev/null
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
while :; do
    # interval: the machine is asked every ten seconds whether it is up again
    sleep 10
    answer
    now=$(m 'cut -d . -f 1 /proc/uptime' 2> /dev/null)
    [ -z "$now" ] || [ "$now" -ge "$up" ] || break
done
say "the machine rebooted: OK"

# shellcheck disable=SC2086 # AGENTS is the validated list of program names
guest credentials "${GUEST_MODE:-$MODE}" $AGENTS > "$work/credentials.after" || give_up "the credential files after the reboot"
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
done

OUT=$(mi "bash ~/rach2-accept/guest.sh secrets ${GUEST_MODE:-$MODE} $AGENTS" < "$LOG")
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
