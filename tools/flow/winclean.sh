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
# the preparation and the cases, that has not ended, and keeps any from
# starting. A part has ended once its bash wrote its status to
# OUT/ID.PART.rc, or, for a part started before release.sh made that file,
# once its output, OUT/ID.PART.out, has a status line. winclean.sh touches
# OUT/ID.PART.cancel before it reads whether the part was launched, and the
# part's start and the part's bash each read that file after they have said
# what they are, as release.sh's windows_detach says, so that a part either
# shows itself here or does no work. OUT/ID.PART.launch exists while a part's
# start runs and names, once Windows has created the part's bash, that
# process's number and start time; OUT/ID.PART.pid names, once the bash runs,
# its Windows process and MSYS2 process group. winclean.sh kills that process
# group once, since taskkill's tree misses what MSYS2's exec starts, and the
# bash with its tree while a process of its number and start time runs, or,
# for a bash whose start time no file gives, of its number and a command line
# that runs OUT/ID.PART.sh; PowerShell holds the process while it compares
# and kills, so that Windows cannot give the number to another in between. It
# waits until neither the bash nor the group is left, and then touches
# OUT/ID.PART.stopped, which says that nothing of the part runs or will. A
# start that has not said within half a minute what it created counts as one
# that may still run. It reads no other OUT/ID.*.pid: those of the run's
# claims in WSL name WSL's processes, which release.sh's claim_stop ends.
# Then it stops each process of the run's etude, OUT/etude-ID.exe, with its
# tree, which a stopped ssh leaves running, and waits until tasklist shows
# none; etude runs its commands, QEMU among them, in a job that Windows ends
# with etude. While a part or etude still runs, it removes nothing of that
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
# Exit 0 once nothing of any run ID is left running or can start and its
# scratch is gone or kept, and 1 when a part or etude of one may still run,
# Windows cannot say, or the scratch cannot be removed, so that the caller
# keeps the run's claims in WSL while Windows may still use what they hold.

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

# windows_part PID TIME SCRIPT [kill]: print `gone` when no Windows process
# of number PID that is the part runs, `runs` while one does, and with `kill`
# kill it and its tree and print `killed`; print nothing when Windows cannot
# say. The process is the part when its start time, as a FILETIME, is TIME,
# or, when TIME is empty, when its command line names SCRIPT. PowerShell
# holds the process's handle while it compares and kills, so that its number
# cannot pass to another process in between.
windows_part()
{
    MSYS_NO_PATHCONV=1 powershell.exe -NoProfile -NonInteractive -Command "
        \$p = Get-Process -Id $1 -ErrorAction SilentlyContinue
        if (-not \$p) { 'gone'; exit }
        try { \$null = \$p.Handle } catch { 'gone'; exit }
        if (\$p.HasExited) { 'gone'; exit }
        if ('$2' -ne '') {
            if (\$p.StartTime.ToFileTimeUtc() -ne '$2') { 'gone'; exit }
        } else {
            \$c = (Get-CimInstance Win32_Process -Filter 'ProcessId = $1').CommandLine
            if (-not \$c -or -not \$c.Contains('$3')) { 'gone'; exit }
        }
        if ('$4' -ne 'kill') { 'runs'; exit }
        taskkill.exe /F /T /PID $1 | Out-Null
        'killed'" < /dev/null 2> /dev/null | tr -d '\r'
}

# launched ID PART: wait for the start of the part PART of run ID to say what
# it created, and set p and t to the Windows process number and start time
# that OUT/ID.PART.launch gives, both empty when nothing was created, and w
# and g to the Windows process number and MSYS2 process group that the
# bash's own OUT/ID.PART.pid gives, both empty while it has written none.
# Fail when the start has not said within half a minute what it created, and
# the bash has not named itself either.
launched()
{
    i=0
    while :; do
        p='' t='' w='' g=''
        [ ! -f "$1.$2.pid" ] || read -r w g < "$1.$2.pid"
        [ ! -f "$1.$2.launch" ] || read -r p t < "$1.$2.launch"
        { [ -f "$1.$2.launch" ] && [ -z "$p" ]; } || return 0
        if [ "$i" -ge 6 ]; then
            [ -n "$w" ]
            return
        fi
        i=$((i + 1))
        sleep 5
    done
}

# halt ID PART: stop the part PART of run ID and keep it from starting, and
# succeed once nothing of it is left, waiting up to half a minute.
halt()
{
    p='' w=''
    : > "$1.$2.cancel" || return 1
    launched "$1" "$2" || return 1
    killed=''
    i=0
    while :; do
        [ -n "$g" ] || [ ! -f "$1.$2.pid" ] || read -r w g < "$1.$2.pid"
        if [ -n "$g" ] && [ -z "$killed" ]; then
            kill -KILL "-$g" 2> /dev/null
            killed=1
        fi
        left=''
        if [ -n "$p" ]; then
            [ "$(windows_part "$p" "$t" "$1.$2.sh" kill)" = gone ] || left=1
        fi
        if [ -n "$w" ] && [ "$w" != "$p" ]; then
            [ "$(windows_part "$w" '' "$1.$2.sh" kill)" = gone ] || left=1
        fi
        [ -z "$g" ] || ! kill -0 "-$g" 2> /dev/null || left=1
        if [ -z "$left" ]; then
            : > "$1.$2.stopped"
            return 0
        fi
        [ "$i" -lt 6 ] || return 1
        i=$((i + 1))
        sleep 5
    done
}

# ended ID PART: succeed when the part PART of run ID has ended, as the
# header says.
ended()
{
    [ -e "$1.$2.rc" ] && return 0
    [ ! -e "$1.$2.launch" ] && grep -aq '^status ' "$1.$2.out" 2> /dev/null
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
        ! ended "$id" "$part" || continue
        if ! halt "$id" "$part"; then
            echo "winclean: the $part of $id, bash ${p:-${w:-unknown}}, may still run" >&2
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
    [ "$i" -lt 6 ] || { echo "winclean: cannot remove:" $paths >&2; status=1; }
    unset IFS
done
exit "$status"
