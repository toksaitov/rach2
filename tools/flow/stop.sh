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
# worker scripts and the board's watches, selecting and signalling each in
# one pass, so that no process number waits between the two. On Windows it
# looks only at programs under D:\rach2 and at MSYS2's bash running a release
# run's part, so that Hyper-V and its machines are never touched: winclean.sh
# first stops, with what it started, each release run that may still run
# there, found by its etude or bash or listed by `winclean.sh -l` from
# Windows' output directory and the release's records, and removes the run's
# scratch there; then each other program under D:\rach2 is stopped through its
# own handle. Then it brings the board up to date, runs sweep.sh, with -a
# unless a release still runs, and prints what is still there: a line `stop:
# left: MACHINE: WHAT` for each process and each claim of the admission
# ledger. It ends with `stop: everything is stopped` and status 0, or `stop: N
# left` and status 1. dev-log/84-locks/84-111-lifecycle.md gives the states
# and guards of a release's Windows parts and WSL claims that this follows.
#
# -n changes nothing and prints only what runs now, in the same form. A running
# release is left alone and counts as left, since it costs hours to repeat:
# while one runs, stop.sh leaves Windows alone, and on the Mac the process
# group of the release's run there with every descendant of it, and in WSL
# the copy of -p's preparation. A release runs while STATE/pid or
# STATE/windows.run of build/flow/release names a process with the start the
# file records that runs tools/flow/release.sh; a command line that merely
# holds release.sh is never stopped. -r stops the release too, with TERM to
# each such process and to the group of its Mac run while that group's leader
# is the run's release.sh -m, whose traps stop its machines and end its
# claims, and waits up to ten minutes for them to end before it stops the
# rest; a release still running then stays, as without -r. `release.sh -r`
# goes on from it later. The session of a claim of WSL's admission that a
# Windows run holds, whose leader runs OUT/ID.KIND.N.sh or whose number an
# OUT/ID.KIND.N.pid names, is never stopped here: it ends by itself ten
# minutes after the run's last touch of ID.alive, so that WSL admits other
# work only once Windows has stopped. The two GitHub runners stay on, since
# they are idle unless a release runs; -g stops them too and turns them off
# at boot, which `systemctl enable --now` undoes.
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

# releases: print each process of this clone's release.sh that STATE/pid or
# STATE/windows.run names, STATE being build/flow/release of the main working
# tree, while ps gives it the start that the file records and shows it
# running tools/flow/release.sh, with -w for STATE/windows.run, as
# release.sh's `released` tests it (G12).
releases()
{
    released "$top/build/flow/release/pid" ''
    released "$top/build/flow/release/windows.run" ' -w '
}

# released FILE WORD: print the process that FILE names, `PID START`, while
# ps gives it that start and shows it running tools/flow/release.sh with
# WORD on its command line.
released()
{
    rd_pid='' rd_start=''
    [ ! -f "$1" ] || read -r rd_pid rd_start < "$1"
    [ -n "$rd_pid" ] && [ -n "$rd_start" ] || return 0
    [ "$(lstart "$rd_pid")" = "$rd_start" ] || return 0
    case $(ps -o command= -p "$rd_pid" 2> /dev/null) in
        *tools/flow/release.sh*"$2"*) echo "$rd_pid" ;;
    esac
}

# lstart PID: print the start of process PID as ps gives it, in one line
# with single spaces.
lstart()
{
    ps -o lstart= -p "$1" 2> /dev/null | tr -s ' ' | sed 's/^ //; s/ $//'
}

# macrun: print the process group of the release's run on the Mac that
# build/machines/mac/run names, while its leader is the release.sh -m of
# that run (G12).
macrun()
{
    mr_pg='' mr_id=''
    [ ! -f "$top/build/machines/mac/run" ] || read -r mr_pg mr_id < "$top/build/machines/mac/run"
    [ -n "$mr_pg" ] && [ -n "$mr_id" ] || return 0
    case $(ps -o command= -p "$mr_pg" 2> /dev/null) in
        *tools/flow/release.sh*" -m $mr_id "*) echo "$mr_pg" ;;
    esac
}

# held: print Windows' output directory as WSL sees it.
held()
{
    o=$(machine out)
    echo "/mnt/$(echo "$o" | cut -c 1 | tr '[:upper:]' '[:lower:]')$(echo "$o" | cut -c 3-)"
}

# mac_procs [kill]: print a line `mac: PID ARGS` for each process on the Mac
# that WHAT names, and with `kill` send it TERM as it is read, so that no
# number waits between the selection and the signal (G19). A command line
# that merely holds release.sh is never one (G12). Without -r, while a
# release runs, the group of its Mac run and every descendant of that group,
# such as etude's held commands, which have groups of their own, do not
# count (V8).
mac_procs()
{
    spare=
    [ -n "$release" ] || [ -z "$(releases)" ] ||
        spare=$(cut -d ' ' -f 1 "$top/build/machines/mac/run" 2> /dev/null)
    # WHAT goes in the environment, so that awk's own command line holds
    # none of it.
    ps -axo pid=,ppid=,pgid=,command= | WHAT=$WHAT awk -v g="$spare" '
        {
            pid[NR] = $1; pp[NR] = $2; pg[NR] = $3
            c = $0
            sub(/^ *[0-9]+ +[0-9]+ +[0-9]+ /, "", c)
            cmd[NR] = c
        }
        END {
            if (g != "") {
                for (i = 1; i <= NR; i++)
                    if (pg[i] == g)
                        s[pid[i]] = 1
                do {
                    more = 0
                    for (i = 1; i <= NR; i++)
                        if (!(pid[i] in s) && (pp[i] in s)) {
                            s[pid[i]] = 1
                            more = 1
                        }
                } while (more)
            }
            for (i = 1; i <= NR; i++)
                if (!(pid[i] in s) && cmd[i] ~ ENVIRON["WHAT"] && cmd[i] !~ /grep|stop\.sh|release\.sh/)
                    print pid[i], cmd[i]
        }' | while read -r pid args; do
        [ "${1-}" != kill ] || kill "$pid" 2> /dev/null
        echo "mac: $pid $args"
    done
}

# wsl_procs [kill]: print a line `wsl: PID USER ARGS` for each process in WSL
# that WHAT names, and with `kill` signal it as it is read (G19). A command
# line that merely holds release.sh is never one (G12). The session of a
# claim of WSL's admission never counts: the sessions of the live leaders
# that run OUT/ID.KIND.N.sh and those that each OUT/ID.KIND.N.pid names, as
# words (G15). Without -r, while a release runs, the GitHub runners' jobs and
# the copy of -p's preparation are its own and do not count.
wsl_procs()
{
    {
        echo "release='$release' live='$(releases | head -n 1)' copy='$prepare' mode='${1-}' held='$(held)'"
        echo "what='$WHAT'"
        cat << 'END'
claims=" $({
    ps -eo sid=,args= | awk '$2 == "sh" && $3 ~ /\.(claim|hold)(\.[0-9]+)?\.sh$/ { print $1 }'
    cat "$held"/*.claim*.pid "$held"/*.hold*.pid 2> /dev/null
} | tr -s '\n ' '  ') "
ps -eo pid=,sid=,user=,args= | grep -E "$what" | grep -v -E 'grep|stop\.sh|release\.sh' |
    while read -r pid sid user args; do
        case $claims in *" $sid "*) continue ;; esac
        if [ -z "$release" ]; then
            [ "$user" != ghrunner ] || continue
            if [ -n "$live" ]; then
                case $(readlink "/proc/$pid/cwd" 2> /dev/null) in
                    "$copy" | "$copy"/*) continue ;;
                esac
            fi
        fi
        [ "$mode" != kill ] || kill "$pid" 2> /dev/null
        echo "wsl: $pid $user $args"
    done
END
    } | wsl
}

# windows_procs [kill]: print a line `windows: PID NAME` for each program
# under D:\rach2 on Windows and `windows: PID bash COMMAND` for each MSYS2
# bash that runs a release run's part, OUT/ID.PART.sh; with `kill`, stop each
# program under D:\rach2 through its own handle as it is selected (G19). The
# parts' bashes are winclean.sh's to stop, with what they started. Hyper-V
# and its machines are never touched. A release that stays keeps what it
# runs on Windows.
windows_procs()
{
    [ -n "$release" ] || [ -z "$(releases)" ] || return 0
    k=
    [ "${1-}" != kill ] || k='; try { $_.Kill() } catch { }'
    echo "Get-Process | Where-Object { \$_.Path -like 'D:\\rach2\\*' -or \$_.Path -like 'D:/rach2/*' } |
        ForEach-Object { 'windows: {0} {1}' -f \$_.Id, \$_.ProcessName$k }" | windows
    echo "Get-CimInstance Win32_Process -Filter \"Name = 'bash.exe'\" |
        Where-Object { \$_.CommandLine -like '* $(machine out)/*.sh' } |
        ForEach-Object { 'windows: {0} bash {1}' -f \$_.ProcessId, \$_.CommandLine }" | windows
}

# running: print a line `MACHINE: PID ARGS` for each process the header names.
running()
{
    mac_procs
    wsl_procs
    windows_procs
}

# winclean ARG...: run winclean.sh on Windows with Windows' copy and output
# directory before ARGs, for twenty minutes at most.
winclean()
{
    case $1 in
        -l) shift; set -- -l "$(machine tree)" "$(machine out)" "$@" ;;
        *) set -- "$(machine tree)" "$(machine out)" "$@" ;;
    esac
    perl -e 'alarm shift; exec @ARGV' 1200 ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" \
        "C:/msys64/usr/bin/bash.exe -l -s $*" < "$here/winclean.sh"
}

# stragglers: stop what `running` lists, but a release that may stay. On
# Windows, while no release runs, winclean.sh first stops each release run
# that may still run there, with what it started, and removes what it made
# outside its own files: each whose etude or part's bash runs, and each that
# `winclean.sh -l` lists from OUT, the record's `windows started` lines and
# -p's STATE/prepare/windows (V9); then each other program under D:\rach2 is
# stopped.
stragglers()
{
    if [ -n "$release" ] || [ -z "$(releases)" ]; then
        known=$({
            sed -n 's/^windows started //p' "$top/build/flow/release/record" 2> /dev/null
            cat "$top/build/flow/release/prepare/windows" 2> /dev/null
        } | sort -u | tr '\n' ' ')
        # shellcheck disable=SC2086 # the ids are a word each
        ids=$({
            windows_procs | sed -n -e 's/^windows: [0-9]* etude-\(milestone[^ ]*\)$/\1/p' \
                -e 's/^windows: [0-9]* bash .*\/\(milestone[^ /.]*\)\.[a-z]*\.sh$/\1/p'
            winclean -l $known 2> /dev/null | tr -d '\r' | sed -n 's/^orphan //p'
        } | sort -u | tr '\n' ' ')
        # shellcheck disable=SC2086 # the ids are a word each
        [ -z "$ids" ] || winclean $ids > /dev/null 2>&1
        windows_procs kill > /dev/null
    fi
    mac_procs kill > /dev/null
    wsl_procs kill > /dev/null
    sleep 10
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
        for p in $(releases); do
            echo "mac: $p $(ps -o command= -p "$p" 2> /dev/null)"
        done | cut -c 1-160 | sed 's/^/stop: left: /'
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
# The copy of release.sh -p's preparation in WSL, as release.sh names it.
prepare=$(sed -n 's,^PREPARE_COPY=\$OWNER,/home/testing/rach2,p' "$here/release.sh")
if [ -n "$dry" ]; then
    left
    exit
fi
loops
if [ -n "$release" ]; then
    pids=$(releases) group=$(macrun)
    # shellcheck disable=SC2086 # the ids are a word each
    [ -z "$pids" ] || kill -TERM $pids 2> /dev/null
    [ -z "$group" ] || kill -TERM -"$group" 2> /dev/null
    i=0
    while [ -n "$(releases)" ] && [ "$i" -lt 120 ]; do
        sleep 5
        i=$((i + 1))
    done
    # V10: a release still running has not run its traps; it stays, and the
    # rest is stopped as without -r.
    if [ -n "$(releases)" ]; then
        echo "stop: the release did not end in ten minutes; it stays"
        release=
    fi
fi
stragglers
[ -z "$github" ] || runners
sh "$here/board.sh" 'done' > /dev/null 2>&1 || echo "stop: board.sh done failed"
sh "$here/board.sh" archive > /dev/null 2>&1 || echo "stop: board.sh archive failed"
if [ -n "$(releases)" ]; then
    sh "$here/sweep.sh"
else
    sh "$here/sweep.sh" -a
fi
left
