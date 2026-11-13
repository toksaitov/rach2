#!/bin/sh
# shellcheck disable=SC2016,SC2028 # other shells' scripts
#
# stop.sh: stop the factory on every machine and leave nothing of it behind.
#
# Usage: stop.sh [-n] [-g] [-r]
#
# It stops each loop that `workers` lists and each other loop with a state
# directory under build/flow of this clone's main working tree: it leaves
# STATE/stop, which ends the loop once its session has ended, and
# STATE/session.stop, on which the session's supervisor, session.pl, stops the
# session's group, TERM and then KILL half a minute later, and exits once the
# group is gone; it then waits for both to end, signalling neither, by the
# locks that each holds for its life, STATE/loop.live and STATE/session.live.
# A STATE/session whose supervisor is gone names a group that is not
# signalled, since its number may now be another's; unless the group is
# proved gone, as session.pl proves it, it is a line `stop: left: STATE:
# ...`, as is a supervisor or a loop that has not ended in ninety
# seconds and a loop whose host did not answer, and each counts in the final
# count and status. Every ssh is bounded by bound.pl. It then stops what the
# sessions left running in WSL and on Windows, QEMU guests, test runs and the
# board's watches, but never a loop, which it only lists, and lists what they
# left on the Mac. In WSL procs.pl reads each from /proc and signals it
# through a pidfd, which names that process and no other. On the Mac there is
# no pidfd and no process can be held but by its parent or its group, so a
# process that no hold.pl holds is only listed, `stop: left: mac: PID ARGS`,
# and counts as left, for a person to stop by its number. On Windows it looks
# only at programs under D:\rach2 and at the Job Objects of release runs'
# parts, so that Hyper-V and its machines are never touched: winclean.sh -a
# first makes the current run none, so that a part's start still on its way
# starts nothing, and then stops each release run that `winclean.sh -l` lists
# from Windows' output directory, by its parts' jobs, which end everything
# the parts started, and removes the run's scratch there; then each other
# program under D:\rach2 is stopped through its own handle. Then it brings the
# board up to date, runs sweep.sh, with -a only once no run of the release is
# left, a lost one among them, and every loop's stop is confirmed, and prints
# what is still there: a line `stop: left: MACHINE: WHAT` for each process, each
# run of the release that has not ended, each job and each claim of the
# admission ledger, and a line `stop: left: MACHINE: unknown: WHY` for each look
# that cannot be read and each list or stop on Windows that failed. It ends with
# `stop: everything is stopped` and status 0, or `stop: N left` and status 1.
# dev-log/84-locks/84-111-lifecycle.md gives the model of a release's runs,
# Windows parts and WSL claims that this follows.
#
# -n changes nothing and prints only what runs now, in the same form. A running
# release is left alone and counts as left, since it costs hours to repeat:
# while one runs, stop.sh leaves Windows alone, and in WSL the copy of -p's
# preparation. A release runs while the hold.pl of one of its runs holds the
# lock of its run directory, as `hold.pl state` reads it: the release itself,
# whose directory build/flow/release/run names, the Windows run of -a, which
# build/flow/release/windows.run names, and the Mac's run or -p's preparation
# there, which build/machines/mac/run names; each such run is a line `stop:
# left: mac: KIND DIR STATE`. A command line that merely holds release.sh is
# never stopped. -r stops the release too, through the hold.pl that holds
# each run: it creates the stop of each run's directory, and again on each
# turn for a run that the release started meanwhile, on which each hold.pl
# sends TERM to the release, to the -a Windows run's group and to the Mac
# run's group, whose traps stop its machines and end its claims, and waits up
# to ten minutes for each hold.pl to end before it stops the rest; a release
# still running then stays, as without -r. `release.sh -r` goes on from it
# later. The session of a claim of WSL's admission that a Windows run holds,
# whose leader is the perl that runs LOCKS/ID.KIND.N.pl with `run`, LOCKS
# being build/flow/claims of the owner's clone in WSL, is never stopped here:
# it ends by itself once the run's ID.alive has not grown for ten minutes, so
# that WSL admits other work only once Windows has stopped. The two GitHub
# runners stay on, since they are idle unless a release runs; -g stops them
# too and turns them off at boot, which `systemctl enable --now` undoes.
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
# a signal, and nothing in it reads a process's number. The loop holds
# STATE/loop.live for its whole life, and the supervisor of its session,
# session.pl, holds STATE/session.live for its own; halt probes the two locks.
# Once STATE/session.stop exists the supervisor stops the group it made while
# the group's leader, its anchor, is its unreaped child, and removes
# STATE/session once the group is gone. The loop ends by itself once
# STATE/stop exists and its session has ended, within five seconds of a wait.
# The wait counts ninety seconds, which covers the TERM, thirty turns, the
# KILL and the loop's end. A STATE/session whose supervisor is gone names a
# group that may still run: halt removes it when it proves the group gone as
# session.pl does, by a boot that is not this one or a ps that shows no
# process in the group, and otherwise a person checks it.
halt='
st=$1
case $st in "~"*) st=$HOME${st#"~"} ;; esac
[ -d "$st" ] || exit 0
st=$(cd "$st" && pwd) || exit 0
touch "$st/stop" "$st/session.stop"
# held FILE: succeed while a process holds the lock FILE; a lock that cannot
# be probed counts as held.
held() {
    perl -MFcntl=:flock -e '"'"'open(L, "<", shift) or exit($!{ENOENT} ? 1 : 0);
        exit(flock(L, LOCK_SH | LOCK_NB) ? 1 : 0)'"'"' "$1"
}
# gone RECORD: succeed when the group that RECORD, `GROUP BOOT`, names is
# proved gone, as session.pl proves it.
gone() {
    perl -e '"'"'
        open(R, "<", $ARGV[0]) or exit 1;
        my ($g, $b) = split(" ", <R> // "");
        my $n = "";
        if (open(B, "<", "/proc/sys/kernel/random/boot_id")) {
            $n = <B>;
        } elsif ($^O eq "darwin") {
            $n = `/usr/sbin/sysctl -n kern.bootsessionuuid 2> /dev/null`;
        }
        chomp $n;
        exit 0 if defined $b && $b ne "-" && $n ne "" && $b ne $n;
        defined $g && $g =~ /^\d+$/ or exit 1;
        my @ps = `ps -A -o pgid=`;
        exit 1 if $? != 0;
        exit((grep { /^\s*(\d+)\s*$/ && $1 == $g } @ps) ? 1 : 0)'"'"' "$1"
}
i=0
while { held "$st/session.live" || held "$st/loop.live"; } && [ "$i" -lt 90 ]; do
    sleep 1
    i=$((i + 1))
done
if held "$st/session.live"; then
    echo "stop: left: $st: its session.pl has not ended in ninety seconds"
elif [ -e "$st/session" ] && ! { gone "$st/session" && rm -f "$st/session"; }; then
    echo "stop: left: $st: $st/session names process group $(cut -d " " -f 1 "$st/session"), whose session.pl was killed, and the group is not proved gone; a person checks it"
elif held "$st/loop.live"; then
    echo "stop: left: $st: its loop has not ended in ninety seconds"
else
    echo "stop: the loop of $st ended"
fi
exit 0
'

# ssh_host HOST ARG...: run a command on HOST, which may not answer, for five
# minutes at most, as bound.pl bounds it.
ssh_host()
{
    host=$1
    shift
    perl "$here/bound.pl" 300 ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" "$@"
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

# windows: run standard input as a script for the testing host's MSYS2 bash,
# print each line it prints after `windows: `, and print `windows: unknown:
# WHY` as wsl does.
windows()
{
    wn_out=$(ssh_host "$TESTING" 'C:/msys64/usr/bin/bash.exe -l -s' 2> /dev/null)
    wn_rc=$?
    [ -z "$wn_out" ] || printf '%s\n' "$wn_out" | tr -d '\000\r' | sed 's/^/windows: /'
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

# releases: print a line `KIND DIR STATE` for each run of this clone's
# release.sh whose hold.pl has not ended, as `hold.pl state` reads it from
# the run's directory DIR: KIND `release` for the release, which
# build/flow/release/run of the main working tree names, `windows` for the
# Windows run of -a, which build/flow/release/windows.run names, and `mac`
# for the Mac's run or -p's preparation there, which build/machines/mac/run
# names (G12). STATE is running, starting, lost or unknown.
releases()
{
    for rl in "release $top/build/flow/release/run" "windows $top/build/flow/release/windows.run" \
        "mac $top/build/machines/mac/run"; do
        rl_kind=${rl%% *} rl_file=${rl#* }
        rl_dir=$(cut -d ' ' -f 1 "$rl_file" 2> /dev/null)
        [ -n "$rl_dir" ] || continue
        rl_state=$(perl "$here/hold.pl" state "$rl_dir")
        case $rl_state in
            "ended "* | absent) ;;
            *) echo "$rl_kind $rl_dir $rl_state" ;;
        esac
    done
}

# live_release: succeed while a run that `releases` lists runs or is about
# to, so that it stays without -r.
live_release()
{
    releases | grep -q -e ' running$' -e ' starting$'
}

# mac_procs: print a line `mac: PID ARGS` for each process on the Mac that
# WHAT names, as one ps lists them, and `mac: unknown: WHY` when ps cannot
# be read. Nothing here signals them: on the Mac no process can be held by
# anything but its parent or the group it is in, so each process that no
# hold.pl holds is only listed, and counts as left, for a person to stop by
# its number (G19). A command line that merely holds release.sh is never one
# (G12).
mac_procs()
{
    if ! mp_out=$(ps -axo pid=,command=); then
        echo "mac: unknown: ps could not be read"
        return
    fi
    # WHAT goes in the environment, so that awk's own command line holds
    # none of it.
    printf '%s\n' "$mp_out" | WHAT=$WHAT awk '
        {
            c = $0
            sub(/^ *[0-9]+ /, "", c)
            if (c ~ ENVIRON["WHAT"] && c !~ /grep|stop\.sh|release\.sh/)
                print "mac: " $1, c
        }'
}

# wsl_procs [kill]: print a line `wsl: PID START USER ARGS` for each process
# in WSL that WHAT names, and with `kill` send it TERM, but a loop, as
# procs.pl does: through a pidfd opened before the process was read, which
# names that process and no other, and a scan that cannot read the whole of
# /proc fails, which counts as a look that cannot be read (G19). A command
# line that merely holds release.sh is never one (G12). The session of a
# claim of WSL's admission never counts: each session whose leader is the
# perl that runs LOCKS/ID.KIND.N.pl with `run`, as release.sh's claim_start
# starts it (G15). Without -r, while a release runs, the GitHub runners' jobs
# and the copy of -p's preparation are its own and do not count.
wsl_procs()
{
    {
        echo "release='$release' live='$(live_release && echo 1)' copy='$prepare' mode='${1-}'"
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
# absent; with `kill`, stop each program under D:\rach2 through the one
# handle through which winjob's `strays` read its path, so that no other
# process is ever stopped (G19). The parts' jobs are winclean.sh's to stop.
# Hyper-V and its machines are never touched. A release that stays keeps
# what it runs on Windows.
windows_procs()
{
    [ -n "$release" ] || ! live_release || return 0
    {
        sh "$here/winjob.sh" "$(machine out)"
        # shellcheck disable=SC2016 # the remote shell expands these
        echo '[ -x "$winjob" ] || exit 3'
        # shellcheck disable=SC2016 # the remote shell expands these
        echo '"$winjob" strays D:/rach2 '"$([ "${1-}" != kill ] || echo stop)"
    } | windows
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
    if [ -n "$release" ] || ! live_release; then
        if ! st_out=$(winclean -a 2> /dev/null); then
            st_ids=$(printf '%s\n' "$st_out" | tr -d '\r' | sed -n 's/^left //p' | sort -u | tr '\n' ' ' |
                sed 's/ $//')
            unsure="${unsure}windows: unknown: winclean.sh -a did not confirm that every release run stopped${st_ids:+: $st_ids}$nl"
        fi
        windows_procs kill > /dev/null
    fi
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
        releases | while read -r kind dir st; do
            echo "stop: left: mac: $kind $dir: $st"
        done
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
    # Each hold.pl sends TERM to what it holds once its stop exists. The
    # records are read again on each turn, so that a run that the release
    # started before its own stop gets one too.
    i=0
    while live_release && [ "$i" -lt 120 ]; do
        releases | while read -r kind dir st; do
            case $st in running | starting) perl "$here/hold.pl" stop "$dir" 0 > /dev/null ;; esac
        done
        sleep 5
        i=$((i + 1))
    done
    # V10: a release still running has not run its traps; it stays, and the
    # rest is stopped as without -r.
    if live_release; then
        echo "stop: the release did not end in ten minutes; it stays"
        release=
    fi
fi
stragglers
[ -z "$github" ] || runners
sh "$here/board.sh" 'done' > /dev/null 2>&1 || echo "stop: board.sh done failed"
sh "$here/board.sh" archive > /dev/null 2>&1 || echo "stop: board.sh archive failed"
# -a only once no run of the release is left, lost ones among them, and
# every loop's stop is confirmed.
if [ -z "$(releases)" ] && [ -z "$unsure" ]; then
    sh "$here/sweep.sh" -a
else
    sh "$here/sweep.sh"
fi
left
