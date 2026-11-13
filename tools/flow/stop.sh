#!/bin/sh
# shellcheck disable=SC2016,SC2009,SC2028 # other shells' scripts; ps sees all
#
# stop.sh: stop the factory on every machine and leave nothing of it behind.
#
# Usage: stop.sh [-n] [-g] [-r]
#
# It stops each loop that `workers` lists and each other loop with a state
# directory under build/flow of this clone's main working tree: it leaves
# STATE/stop, which ends the loop once its session has ended, and
# STATE/session.stop, on which the session's supervisor, the session.pl whose
# command line names STATE/session, stops the session's group, TERM and then
# KILL half a minute later, and exits once the group is gone; it then waits for
# both to end, signalling neither. A group that STATE/session names and that has
# no supervisor is not signalled, since its number may now be another's; it is a
# line `stop: left: STATE: ...`, as is a supervisor or a loop that has not ended
# in ninety seconds and a loop whose host did not answer, and each counts in the
# final count and status. It then stops what the sessions left running on the
# Mac, in WSL and on Windows: QEMU guests, test runs and the board's watches,
# but never a loop, which it only lists. In WSL procs.pl reads each from /proc
# and signals it through a pidfd, which names that process and no other; on the
# Mac, where no process holds them and there is no pidfd, each is selected and
# signalled in one pass, so that no process number waits between the two. On
# Windows it looks only at programs under D:\rach2 and at the Job Objects of
# release runs' parts, so that Hyper-V and its machines are never touched:
# winclean.sh -a first makes the current run none, so that a part's start still
# on its way starts nothing, and then stops each release run that `winclean.sh
# -l` lists from Windows' output directory, by its parts' jobs, which end
# everything the parts started, and removes the run's scratch there; then each
# other program under D:\rach2 is stopped through its own handle. Then it brings
# the board up to date, runs sweep.sh, with -a unless a release still runs, and
# prints what is still there: a line `stop: left: MACHINE: WHAT` for each
# process, each job and each claim of the admission ledger, and a line `stop:
# left: MACHINE: unknown: WHY` for each look that cannot be read and each list
# or stop on Windows that failed. It ends with `stop: everything is stopped` and
# status 0, or `stop: N left` and status 1. dev-log/84-locks/84-111-lifecycle.md
# gives the model of a release's Windows parts and WSL claims that this follows.
#
# -n changes nothing and prints only what runs now, in the same form. A running
# release is left alone and counts as left, since it costs hours to repeat:
# while one runs, stop.sh leaves Windows alone, and on the Mac the process
# group of the release's run there with every descendant of it, and in WSL
# the copy of -p's preparation. A release runs while STATE/pid or
# STATE/windows.run of build/flow/release names a process with the start the
# file records that runs tools/flow/release.sh; a command line that merely
# holds release.sh is never stopped. -r stops the release too, through the
# hold.pl that holds each part: it leaves the stop of STATE/held/release, of
# STATE/held/windows and of build/machines/mac/held, on which each hold.pl
# sends TERM to the release, to the -a Windows run's group and to the Mac
# run's group, whose traps stop its machines and end its claims, and waits
# up to ten minutes for them to end before it stops the rest; a release
# still running then stays, as without -r. `release.sh -r`
# goes on from it later. The session of a claim of WSL's admission that a
# Windows run holds, whose leader is the perl that runs LOCKS/ID.KIND.N.pl
# with `run`, LOCKS being build/flow/claims of the owner's clone in WSL, is
# never stopped here: it ends by itself once the run's ID.alive has not grown
# for ten minutes, so that WSL admits other work only once Windows has
# stopped. The two GitHub runners stay on, since
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

# halt is the script that stops the loop whose state directory is its
# argument, relative to the home directory or absolute. Nothing in it sends
# a signal. The session is stopped only through its supervisor, which
# run_session in lib.sh starts as `perl STATE/run/session.pl STATE/session
# ...`, STATE being absolute as the loop makes it, and which, once
# STATE/session.stop exists, stops the group it made while the group's
# leader, its anchor, is its unreaped child, and removes STATE/session once
# the group is gone. The anchor has the supervisor's command line. The loop
# ends by itself once STATE/stop exists and its session has ended, within
# five seconds of a wait. The wait counts ninety seconds, which covers the
# TERM, thirty turns, the KILL and the loop's end.
halt='
st=$1
case $st in "~"*) st=$HOME${st#"~"} ;; esac
[ -d "$st" ] || exit 0
st=$(cd "$st" && pwd) || exit 0
touch "$st/stop" "$st/session.stop"
supervisor="/session\.pl $st/session "
loop="$st/run/worker\.sh"
i=0
while { pgrep -f "$supervisor" || pgrep -f "$loop"; } > /dev/null 2>&1 && [ "$i" -lt 90 ]; do
    sleep 1
    i=$((i + 1))
done
group=$(cat "$st/session" 2> /dev/null)
if pgrep -f "$supervisor" > /dev/null 2>&1; then
    echo "stop: left: $st: its session.pl has not ended in ninety seconds"
elif [ -n "$group" ] && kill -0 -"$group" 2> /dev/null; then
    echo "stop: left: $st: process group $group, which $st/session names, has no session.pl; a person checks it"
elif pgrep -f "$loop" > /dev/null 2>&1; then
    echo "stop: left: $st: its loop has not ended in ninety seconds"
else
    echo "stop: the loop of $st ended"
fi
exit 0
'

# ssh_host HOST ARG...: run a command on HOST, which may not answer.
ssh_host()
{
    host=$1
    shift
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" "$@"
}

# wsl: run standard input as a script as root in WSL on the testing host, and
# print `wsl: unknown: WHY` when the host does not answer or the script fails,
# so that what cannot be read counts as left.
wsl()
{
    ws_out=$(ssh_host "$TESTING" 'MSYS_NO_PATHCONV=1 wsl.exe -u root -- sh -s' 2> /dev/null)
    ws_rc=$?
    [ -z "$ws_out" ] || printf '%s\n' "$ws_out" | tr -d '\000\r'
    unknown wsl "$ws_rc"
}

# windows: run standard input as a PowerShell script on the testing host, and
# print `windows: unknown: WHY` as wsl does.
windows()
{
    wn_out=$(ssh_host "$TESTING" 'powershell -NoProfile -NonInteractive -Command -' 2> /dev/null)
    wn_rc=$?
    [ -z "$wn_out" ] || printf '%s\n' "$wn_out" | tr -d '\000\r'
    unknown windows "$wn_rc"
}

# unknown MACHINE STATUS: print `MACHINE: unknown: WHY` for a look at MACHINE
# that ended with STATUS, unless STATUS is 0.
unknown()
{
    case $2 in
        0) ;;
        255) echo "$1: unknown: the testing host did not answer" ;;
        *) echo "$1: unknown: the look there ended with status $2" ;;
    esac
}

# machine NAME: print the value that the machines file gives Windows' NAME.
machine()
{
    awk -v n="$1" '$1 == "windows" && $2 == n { print $3; exit }' "$here/machines"
}

# loops: stop each loop, as the header says, and keep in unsure, which `left`
# counts, each loop whose stop is not confirmed: a supervisor that has not
# ended, a group without one, a host that did not answer.
loops()
{
    lp_out=$(halts)
    printf '%s\n' "$lp_out" | grep -v -e '^stop: left: ' -e '^$'
    lp_left=$(printf '%s\n' "$lp_out" | sed -n 's/^stop: left: //p')
    [ -z "$lp_left" ] || unsure="$unsure$lp_left$nl"
}

# halts: run halt for each loop, as the header says, and print its lines.
halts()
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

# mac_procs [kill]: print a line `mac: PID ARGS` for each process on the Mac
# that WHAT names, and with `kill` send it TERM as it is read, so that no
# number waits between the selection and the signal (G19), but a loop,
# which halt ends, and which no signal reaches. A command line
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
    ps -axo pid=,ppid=,pgid=,command= | WHAT=$WHAT awk -v g="$spare" -v k="${1-}" '
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
                if (!(pid[i] in s) && cmd[i] ~ ENVIRON["WHAT"] && cmd[i] !~ /grep|stop\.sh|release\.sh/ &&
                    !(k == "kill" && cmd[i] ~ /\/worker\.sh/))
                    print pid[i], cmd[i]
        }' | while read -r pid args; do
        [ "${1-}" != kill ] || kill "$pid" 2> /dev/null
        echo "mac: $pid $args"
    done
}

# wsl_procs [kill]: print a line `wsl: PID USER ARGS` for each process in WSL
# that WHAT names, and with `kill` send it TERM, but a loop, as procs.pl
# does: through a pidfd opened before the process was read, which names that
# process and no other (G19). A command line that merely holds release.sh is
# never one (G12). The session of a claim of WSL's admission never counts:
# each session whose leader is the perl that runs LOCKS/ID.KIND.N.pl with
# `run`, as release.sh's claim_start starts it (G15). Without -r, while a
# release runs, the GitHub runners' jobs and the copy of -p's preparation
# are its own and do not count.
wsl_procs()
{
    {
        echo "release='$release' live='$(releases | head -n 1)' copy='$prepare' mode='${1-}'"
        echo "what='$WHAT'"
        echo "procs() { perl - \"\$@\" << 'END_OF_PROCS'"
        cat "$here/procs.pl"
        echo 'END_OF_PROCS'
        echo '}'
        cat << 'END'
set -- -c -e "$what"
if [ "$mode" = kill ]; then
    set -- "$@" -k -v 'grep|stop\.sh|release\.sh|/worker\.sh'
else
    set -- "$@" -v 'grep|stop\.sh|release\.sh'
fi
if [ -z "$release" ]; then
    set -- "$@" -u ghrunner
    [ -z "$live" ] || set -- "$@" -D "$copy"
fi
out=$(procs "$@") || exit 1
[ -z "$out" ] || printf '%s\n' "$out" | sed 's/^/wsl: /'
END
    } | wsl
}

# windows_procs [kill]: print a line `windows: PID NAME` for each program
# under D:\rach2 on Windows and `windows: job ID.PART STATE` for each job of
# a release run's part that `winclean.sh -l` lists and whose state is not
# absent; with `kill`, stop each program under D:\rach2 through its own
# handle as it is selected (G19). The parts' jobs are winclean.sh's to stop.
# Hyper-V and its machines are never touched. A release that stays keeps
# what it runs on Windows.
windows_procs()
{
    [ -n "$release" ] || [ -z "$(releases)" ] || return 0
    k=
    [ "${1-}" != kill ] || k='; try { $_.Kill() } catch { }'
    echo "Get-Process | Where-Object { \$_.Path -like 'D:\\rach2\\*' -or \$_.Path -like 'D:/rach2/*' } |
        ForEach-Object { 'windows: {0} {1}' -f \$_.Id, \$_.ProcessName$k }" | windows
    [ "${1-}" != kill ] || return 0
    if ! wp_ids=$(orphans); then
        echo "windows: unknown: the release runs that may still run could not be listed"
        return
    fi
    [ -n "$wp_ids" ] || return 0
    # shellcheck disable=SC2086 # the ids are a word each
    wp_out=$(winclean -s $wp_ids 2> /dev/null) ||
        echo "windows: unknown: winclean.sh -s could not say whether $wp_ids run"
    printf '%s\n' "$wp_out" | tr -d '\r' |
        awk '$1 == "state" && $3 != "absent" { $1 = "windows: job"; print }'
}

# orphans: print, a line each, the release runs that `winclean.sh -l` lists,
# and fail when it cannot list them.
orphans()
{
    or_out=$(winclean -l 2> /dev/null) || return 1
    printf '%s\n' "$or_out" | tr -d '\r' | sed -n 's/^orphan //p' | sort -u
}

# running: print a line `MACHINE: PID ARGS` for each process the header names.
running()
{
    mac_procs
    wsl_procs
    windows_procs
}

# winclean ARG...: run winclean.sh on Windows with Windows' copy and output
# directory after its mode, after the lines that build its winjob program, for
# twenty minutes at most.
winclean()
{
    wc_mode=
    case $1 in -s | -l | -g | -a) wc_mode=$1; shift ;; esac
    {
        sh "$here/winjob.sh" "$(machine out)"
        cat "$here/winclean.sh"
    } | perl "$here/bound.pl" 1200 ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" \
        "C:/msys64/usr/bin/bash.exe -l -s $wc_mode $(machine tree) $(machine out) $*"
}

# stragglers: stop what `running` lists, but a release that may stay. On
# Windows, while no release runs, `winclean.sh -a` first makes the current
# run none, which a part's start reads after it has written its token, so
# that a start that a dead release left on its way starts nothing, and then
# stops each release run that may still run there, each that
# `winclean.sh -l` lists from OUT, with what it started, and removes what it
# made outside its own files (V9); a stop that fails or cannot say is kept
# in unsure, which `left` counts. Then each other program under D:\rach2 is
# stopped.
stragglers()
{
    if [ -n "$release" ] || [ -z "$(releases)" ]; then
        if ! st_out=$(winclean -a 2> /dev/null); then
            st_ids=$(printf '%s\n' "$st_out" | tr -d '\r' | sed -n 's/^left //p' | sort -u | tr '\n' ' ' |
                sed 's/ $//')
            unsure="${unsure}windows: unknown: winclean.sh -a did not confirm that every release run stopped${st_ids:+: $st_ids}$nl"
        fi
        windows_procs kill > /dev/null
    fi
    mac_procs kill > /dev/null
    wsl_procs kill > /dev/null
    sleep 10
}

# runners: stop the GitHub runners and turn them off at boot, once neither has
# a job, as tools/Readme.md says; a runner that has a job, or a stop that
# fails or cannot be read, is kept in unsure, which `left` counts.
runners()
{
    busy=$(gh api "repos/$REPO/actions/runners" --jq '[.runners[] | select(.busy)] | length') || busy=unknown
    if [ "$busy" != 0 ]; then
        unsure="${unsure}github: a runner has a job, or cannot be read: $busy$nl"
        return
    fi
    # shellcheck disable=SC2016 # the remote shell expands these
    rn_out=$(echo "o=\$(systemctl disable --now $RUNNERS 2>&1); rc=\$?"'
        printf "%s\n" "$o" | grep -v -e Removed -e "^\$"
        exit $rc' | wsl)
    [ -z "$rn_out" ] || printf '%s\n' "$rn_out"
    if printf '%s\n' "$rn_out" | grep -q '^wsl: unknown: '; then
        unsure="${unsure}github: the runners' stop in WSL failed or could not be read$nl"
    else
        echo "stop: the GitHub runners are off; \`systemctl enable --now\` starts them"
    fi
}

# left: print what `running` lists, the claims of the admission ledger and
# what stragglers could not confirm, as the header says, and the count last.
# A look that cannot be read counts as one thing left.
left()
{
    out=$({
        for p in $(releases); do
            echo "mac: $p $(ps -o command= -p "$p" 2> /dev/null)"
        done | cut -c 1-160 | sed 's/^/stop: left: /'
        running | cut -c 1-160 | sed 's/^/stop: left: /'
        # shellcheck disable=SC2016 # the remote shell expands these
        echo 'f=/var/lib/rach2-etude/ledger/claims
            if [ -e "$f" ]; then [ -r "$f" ] || exit 1; grep -v "^etude-ledger" "$f"; fi
            exit 0' | wsl |
            sed -e '/^wsl: unknown: /s/^/stop: left: /' -e '/^stop: left: /!s/^/stop: left: wsl: claim /'
        printf '%s' "$unsure" | sed 's/^/stop: left: /'
    })
    [ -z "$out" ] || printf '%s\n' "$out"
    n=$(printf '%s' "$out" | grep -c '^stop: left')
    if [ "$n" -eq 0 ]; then
        echo "stop: everything is stopped"
        return 0
    fi
    echo "stop: $n left"
    return 1
}

dry='' github='' release='' unsure=''
nl='
'
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
    # Each hold.pl sends TERM to what it holds once its stop exists.
    for h in "$top/build/flow/release/held/release" "$top/build/flow/release/held/windows" \
        "$top/build/machines/mac/held"; do
        [ ! -d "$h" ] || touch "$h/stop"
    done
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
