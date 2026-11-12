#!/bin/sh
# shellcheck disable=SC2086 # the paths are a line each
# winclean.sh: on the testing host's Windows, stop what is left of a Windows
# release run and remove its run directory.
#
# Usage: winclean.sh TREE OUT ID...
#
# release.sh and stop.sh pipe it to the host's MSYS2 bash with `bash -l -s
# TREE OUT ID...`, TREE being Windows' copy and OUT its output directory, as
# the machines file names them. For each run ID it first stops each of the
# two parts of the run that release.sh starts apart from its ssh session,
# the preparation and the cases, whose output, OUT/ID.PART.out, has no status
# line yet, and keeps any from starting: it touches OUT/ID.PART.cancel before
# it reads whether the part was launched, and the part's start and the part
# itself each read that file after they have said what they are, as
# release.sh's windows_detach says, so that a part either shows itself here
# or does no work. OUT/ID.PART.launch exists while a part's start runs and
# names, once Windows has created the part's bash, that process's number and
# start time; OUT/ID.PART.pid names, once the bash runs, its Windows process
# and MSYS2 process group. winclean.sh kills that process group, since
# taskkill's tree misses what MSYS2's exec starts, and the bash with its tree
# while a process of its number and start time runs, holding the process
# while it compares and kills so that Windows cannot give the number to
# another; it waits until neither is left. A start that has not said within
# half a minute what it created, or a part whose bash it cannot name, counts
# as one that may still run. It reads no other OUT/ID.*.pid: those of the
# run's claims in WSL name WSL's processes, which release.sh's claim_stop
# ends. Then it stops each process of the run's etude, OUT/etude-ID.exe, with
# its tree, which a stopped ssh leaves running, and waits until tasklist
# shows none; while a part or etude still runs, it removes nothing of that
# run.
#
# Each session of the run has the run directory TREE/build/tmp/ID, which
# ETUDE_RUN_DIR names and which holds the run's TMPDIR and etude's scratch.
# A run that failed, or an etude that kept evidence, wrote ID/KEPT; such a
# run keeps everything, until sweep.sh removes it once its milestone's
# release closed. Otherwise winclean.sh removes the run directory whole, and
# first each path of ID/record whose last line says "made": the tooling job
# directories and partial machine disks that etude makes in TREE/build/tooling,
# which cannot move into the run directory, and which a process stopped by
# force, as taskkill /F and Stop-Process -Force stop one, leaves there. The
# run's own files, OUT/ID and OUT/ID.*, stay for the caller to keep or
# remove.
#
# Exit 0 once nothing of any run ID is left running or can start, and 1 when
# a part or etude of one may still run or Windows cannot say, so that the
# caller keeps the run's claims in WSL while Windows may still use what they
# hold.

[ "$#" -ge 3 ] || { echo 'usage: winclean.sh TREE OUT ID...' >&2; exit 2; }
tree=$1 out=$2
shift 2
cd "$out" || exit 0
set -f
nl='
'

# stopped ID: succeed once tasklist shows no etude-ID.exe, waiting up to half
# a minute; fail while one runs or when tasklist cannot say.
stopped()
{
    i=0
    while :; do
        # CSV, since the table cuts a long image name.
        list=$(MSYS_NO_PATHCONV=1 tasklist /NH /FO CSV /FI "IMAGENAME eq etude-$1.exe" 2> /dev/null) ||
            return 1
        case $list in
        *"\"etude-$1.exe\""*) ;;
        *) return 0 ;;
        esac
        [ "$i" -lt 6 ] || return 1
        i=$((i + 1))
        sleep 5
    done
}

# windows_part PID TIME [kill]: print `gone` when no Windows process of
# number PID and start time TIME runs, `runs` while one does, and with `kill`
# kill it and its tree and print `killed`; print `unknown` when a process of
# number PID runs and TIME is empty, and nothing when Windows cannot say.
# TIME is the start time as a FILETIME, as release.sh's windows_detach
# records it. PowerShell holds the process's handle while it compares and
# kills, so that its number cannot pass to another process in between.
windows_part()
{
    MSYS_NO_PATHCONV=1 powershell.exe -NoProfile -NonInteractive -Command "
        \$p = Get-Process -Id $1 -ErrorAction SilentlyContinue
        if (-not \$p) { 'gone'; exit }
        try { \$null = \$p.Handle } catch { 'gone'; exit }
        if (\$p.HasExited) { 'gone'; exit }
        if ('$2' -eq '') { 'unknown'; exit }
        if (\$p.StartTime.ToFileTimeUtc() -ne '$2') { 'gone'; exit }
        if ('$3' -ne 'kill') { 'runs'; exit }
        taskkill.exe /F /T /PID $1 | Out-Null
        'killed'" < /dev/null 2> /dev/null | tr -d '\r'
}

# launched ID PART: wait for the start of the part PART of run ID to say what
# it created, and set p and t to the Windows process number and start time of
# the part's bash, both empty when nothing was created, and g to the part's
# MSYS2 process group, or empty while its bash has not named it. When the
# part's bash runs but its start never recorded what it created, as a start
# whose session was stopped leaves it, p is the number that the bash's own
# PID file gives and t is empty. Fail when the start has not said within half
# a minute what it created, or the bash is not the one it created.
launched()
{
    i=0
    while :; do
        p='' t='' w='' g=''
        [ ! -f "$1.$2.pid" ] || read -r w g < "$1.$2.pid"
        if [ -f "$1.$2.launch" ]; then
            read -r p t < "$1.$2.launch"
            if [ -n "$p" ]; then
                [ -z "$w" ] || [ "$w" = "$p" ] || { p='' t=''; return 1; }
                return 0
            fi
            if [ "$i" -ge 6 ]; then
                [ -n "$w" ] || return 1
                p=$w
                return 0
            fi
        else
            p=$w
            return 0
        fi
        i=$((i + 1))
        sleep 5
    done
}

# halt ID PART: stop the part PART of run ID and keep it from starting, and
# succeed once nothing of it is left, waiting up to half a minute.
halt()
{
    p=''
    : > "$1.$2.cancel" || return 1
    launched "$1" "$2" || return 1
    [ -n "$p" ] || return 0
    i=0
    while :; do
        [ ! -f "$1.$2.pid" ] || [ -n "$g" ] || read -r w g < "$1.$2.pid"
        [ -z "$g" ] || kill -KILL "-$g" 2> /dev/null
        case $(windows_part "$p" "$t" kill) in
        gone) { [ -n "$g" ] && kill -0 "-$g" 2> /dev/null; } || return 0 ;;
        esac
        [ "$i" -lt 6 ] || return 1
        i=$((i + 1))
        sleep 5
    done
}

# abandoned RECORD: print each path of RECORD whose last line says "made", in
# the order the record first names them.
abandoned()
{
    awk '{
        path = substr($0, length($1) + 2)
        if (!(path in last))
            order[n++] = path
        last[path] = $1
    }
    END {
        for (i = 0; i < n; i++)
            if (last[order[i]] == "made")
                print order[i]
    }' "$1"
}

status=0
for id; do
    case $id in */* | .* | '') echo "winclean: $id is not a run ID" >&2; continue ;; esac
    left=
    for part in prepare cases; do
        ! grep -aq '^status ' "$id.$part.out" 2> /dev/null || continue
        if ! halt "$id" "$part"; then
            echo "winclean: the $part of $id, bash ${p:-unknown}, may still run" >&2
            left=1
        fi
    done
    MSYS_NO_PATHCONV=1 taskkill /F /T /IM "etude-$id.exe" > /dev/null 2>&1
    if ! stopped "$id"; then
        echo "winclean: etude-$id.exe still runs" >&2
        left=1
    fi
    if [ -n "$left" ]; then
        echo "winclean: $id still runs; its scratch stays" >&2
        status=1
        continue
    fi
    run=$tree/build/tmp/$id
    [ -d "$run" ] || continue
    if [ -e "$run/KEPT" ]; then
        echo "winclean: $run is kept: $(head -n 1 "$run/KEPT")" >&2
        continue
    fi
    paths=
    if [ -f "$run/record" ]; then
        IFS=$nl
        for path in $(abandoned "$run/record"); do
            case $path in
            */../* | */..) ;;
            "$tree"/build/tooling/job-* | "$tree"/build/tooling/tooling-*.part)
                paths=$paths$nl$path
                continue
                ;;
            esac
            echo "winclean: $run/record names $path, outside the tooling cache; it stays" >&2
        done
        unset IFS
    fi
    paths=$paths$nl$run
    IFS=$nl
    # A QEMU that taskkill ended may hold its files for a few seconds more.
    i=0
    until rm -rf $paths 2> /dev/null || [ "$i" -ge 6 ]; do
        i=$((i + 1))
        sleep 5
    done
    [ "$i" -lt 6 ] || echo "winclean: cannot remove:" $paths >&2
    unset IFS
done
exit "$status"
