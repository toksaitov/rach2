#!/bin/sh
# shellcheck disable=SC2016,SC2009,SC2028 # other shells' scripts; ps sees all
#
# stop.sh: stop the factory on every machine and leave nothing of it behind.
#
# Usage: stop.sh [-n] [-g] [-r]
#
# It stops each loop that `workers` lists and each other loop with a state
# directory under build/flow of this clone's main working tree: it leaves
# STATE/stop, sends TERM to the process group in STATE/session, then KILL half
# a minute later, and ends the loop itself. It then stops what the sessions
# left running on the Mac, in WSL and on Windows: QEMU guests, test runs, the
# worker scripts and the board's watches. On Windows it looks only at programs
# under D:\rach2, so that Hyper-V and its machines are never touched, and for
# each release run whose etude it stopped it removes the run's scratch there,
# as winclean.sh says. Then it brings the board up to date, runs sweep.sh, with
# -a unless a release still runs, and prints what is still there: a line
# `stop: left: MACHINE: WHAT` for each process and each claim of the admission
# ledger. It ends with `stop: everything is stopped` and status 0, or `stop: N
# left` and status 1.
#
# -n changes nothing and prints only what runs now, in the same form. A running
# release is left alone and counts as left, since it costs hours to repeat;
# -r stops it too, with TERM to release.sh, whose traps stop its machines, and
# `release.sh -r` goes on from it later. The two GitHub runners stay on, since
# they are idle unless a release runs; -g stops them too and turns them off at
# boot, which `systemctl enable --now` undoes.
#
# STATE/stop stays, so that no loop starts by accident: remove it to start the
# loop again, as tools/Readme.md says. The manager's own /loop and Monitors live
# in its session, so the manager cancels those itself.

REPO=toksaitov/rach2
TESTING=testing
RUNNERS='actions.runner.toksaitov-rach2.rach2-ci-1.service actions.runner.toksaitov-rach2.rach2-ci-2.service'
# What a session or a run leaves running, as an extended regular expression
# on a process's command line.
WHAT='qemu-system|/worker\.sh|board\.sh watch|tests/run|/etude |/rach2-etude'

# halt is the script that stops the loop whose state directory is its argument.
halt='
st=$1
case $st in "~"*) st=$HOME${st#"~"} ;; esac
[ -d "$st" ] || exit 0
touch "$st/stop"
group=$(cat "$st/session" 2> /dev/null)
if [ -n "$group" ] && kill -TERM -"$group" 2> /dev/null; then
    i=0
    while kill -0 -"$group" 2> /dev/null && [ "$i" -lt 30 ]; do
        sleep 1
        i=$((i + 1))
    done
    kill -KILL -"$group" 2> /dev/null
    echo "stop: session group $group of $st ended"
fi
pkill -f "$st/run/worker.sh" 2> /dev/null
exit 0
'

# ssh_host HOST ARG...: run a command on HOST, which may not answer.
ssh_host()
{
    host=$1
    shift
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" "$@"
}

# wsl: run standard input as a script as root in WSL on the testing host.
wsl()
{
    ssh_host "$TESTING" 'MSYS_NO_PATHCONV=1 wsl.exe -u root -- sh -s' 2> /dev/null | tr -d '\000\r'
}

# windows: run standard input as a PowerShell script on the testing host.
windows()
{
    ssh_host "$TESTING" 'powershell -NoProfile -NonInteractive -Command -' 2> /dev/null |
        tr -d '\000\r'
}

# machine NAME: print the value that the machines file gives Windows' NAME.
machine()
{
    awk -v n="$1" '$1 == "windows" && $2 == n { print $3; exit }' "$here/machines"
}

# loops: stop each loop, as the header says.
loops()
{
    grep -v '^#' "$here/workers" | while read -r name host st; do
        [ -n "$name" ] || continue
        if [ "$host" = - ]; then
            case $st in /*|"~"*) ;; *) st=$top/$st ;; esac
            printf '%s' "$halt" | sh -s "$st"
        else
            printf '%s' "$halt" | ssh_host "$host" sh -s "$st" ||
                echo "stop: left: $name: its host did not answer"
        fi
    done
    for st in "$top"/build/flow/*; do
        [ -f "$st/loop.log" ] || continue
        printf '%s' "$halt" | sh -s "$st"
    done
}

# running: print a line `MACHINE: PID ARGS` for each process the header names.
# With -r the release's own processes count, and with them the GitHub runners'
# jobs, whose QEMU belongs to the release.
running()
{
    ps -axo pid=,command= | grep -E "$WHAT|release\.sh" | grep -v -E 'grep|stop\.sh' |
        sed 's/^ */mac: /'
    {
        echo "ps -eo pid=,user=,args= | grep -E '$WHAT|release\\.sh' | grep -v -E 'grep|stop\\.sh' |"
        echo "    { [ -n \"$release\" ] && cat || grep -v ' ghrunner '; } | sed 's/^ */wsl: /'"
    } | wsl
    echo "Get-Process | Where-Object { \$_.Path -like 'D:\\rach2\\*' -or \$_.Path -like 'D:/rach2/*' } |
        ForEach-Object { 'windows: {0} {1}' -f \$_.Id, \$_.ProcessName }" | windows
}

# stragglers: stop what `running` lists, but a release that may stay.
stragglers()
{
    list=$(running | cut -c 1-200)
    [ -z "$release" ] && list=$(printf '%s\n' "$list" | grep -v 'release\.sh')
    pids=$(printf '%s\n' "$list" | sed -n 's/^mac: \([0-9]*\) .*/\1/p')
    # shellcheck disable=SC2086 # the ids are a word each
    [ -z "$pids" ] || kill $pids 2> /dev/null
    pids=$(printf '%s\n' "$list" | sed -n 's/^wsl: \([0-9]*\) .*/\1/p' | tr '\n' ' ')
    [ -z "$pids" ] || echo "kill $pids 2> /dev/null" | wsl
    pids=$(printf '%s\n' "$list" | sed -n 's/^windows: \([0-9]*\) .*/\1/p' | tr '\n' ',' | sed 's/,$//')
    [ -z "$pids" ] || echo "Stop-Process -Id $pids -Force" | windows
    # A release run's etude is OUT/etude-ID.exe; remove what each stopped run
    # made on Windows outside its own files.
    ids=$(printf '%s\n' "$list" | sed -n 's/^windows: [0-9]* etude-\(milestone[^ ]*\)$/\1/p' |
        sort -u | tr '\n' ' ')
    [ -z "$ids" ] ||
        ssh_host "$TESTING" "C:/msys64/usr/bin/bash.exe -l -s $(machine tree) $(machine out) $ids" \
            < "$here/winclean.sh" > /dev/null 2>&1
    [ -z "$list" ] || sleep 10
}

# runners: stop the GitHub runners and turn them off at boot, once neither has
# a job, as tools/Readme.md says.
runners()
{
    busy=$(gh api "repos/$REPO/actions/runners" --jq '[.runners[] | select(.busy)] | length') || busy=unknown
    if [ "$busy" != 0 ]; then
        echo "stop: left: github: a runner has a job, or cannot be read: $busy"
        return
    fi
    echo "systemctl disable --now $RUNNERS 2>&1 | grep -v Removed" | wsl
    echo "stop: the GitHub runners are off; \`systemctl enable --now\` starts them"
}

# left: print what `running` lists and the claims of the admission ledger, as
# the header says, and the count last.
left()
{
    out=$({
        running | cut -c 1-160 | sed 's/^/stop: left: /'
        echo 'grep -v "^etude-ledger" /var/lib/rach2-etude/ledger/claims 2> /dev/null' | wsl |
            sed 's/^/stop: left: wsl: claim /'
    })
    [ -z "$out" ] || printf '%s\n' "$out"
    n=$(printf '%s' "$out" | grep -c '^stop: left')
    if [ "$n" -eq 0 ]; then
        echo "stop: everything is stopped"
    else
        echo "stop: $n left"
    fi
    return "$n"
}

dry='' github='' release=''
while getopts ngr o; do
    case $o in
        n) dry=1 ;;
        g) github=1 ;;
        r) release=1 ;;
        *) sed -n 's/^# Usage: /usage: /p' "$0" >&2; exit 2 ;;
    esac
done
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=tools/flow/lib.sh
. "$here/lib.sh"
top=$(primary "$here")
if [ -n "$dry" ]; then
    left
    exit
fi
loops
[ -z "$release" ] || pkill -TERM -f 'release\.sh' 2> /dev/null
stragglers
[ -z "$github" ] || runners
sh "$here/board.sh" 'done' > /dev/null 2>&1 || echo "stop: board.sh done failed"
sh "$here/board.sh" archive > /dev/null 2>&1 || echo "stop: board.sh archive failed"
if running | grep -q 'release\.sh'; then
    sh "$here/sweep.sh"
else
    sh "$here/sweep.sh" -a
fi
left
