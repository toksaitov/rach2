#!/bin/sh
# shellcheck disable=SC2086 # the paths are a line each
# winclean.sh: on the testing host's Windows, stop what is left of a Windows
# release run and remove its run directory, or list the runs that may still
# run.
#
# Usage: winclean.sh TREE OUT ID...
#        winclean.sh -l TREE OUT [ID...]
#
# release.sh and stop.sh pipe it to the host's MSYS2 bash with `bash -l -s
# TREE OUT ID...`, TREE being Windows' copy and OUT its output directory, as
# the machines file names them. dev-log/84-locks/84-111-lifecycle.md gives
# the states and guards that it implements, named here as there.
#
# For each run ID it first stops each of the two parts of the run that
# release.sh starts apart from its ssh session, the preparation and the
# cases, that has not ended, and keeps any from working. A part has ended
# once its bash wrote its status to OUT/ID.PART.rc, or once an earlier
# winclean.sh wrote OUT/ID.PART.stopped (G4, G6). winclean.sh touches
# OUT/ID.PART.cancel before it reads the part's launch and PID files, and
# the part's start and the part's bash each read that file after they have
# said what they are, as release.sh's windows_detach says, so that a part
# whose PID file is missing here never works (G3). OUT/ID.PART.launch names,
# once Windows has created the part's bash, that process's number and start
# time, a start time of 0 meaning none was read; OUT/ID.PART.pid names, once
# the bash runs, its Windows process and MSYS2 process group. A Windows
# process is the part's bash only while its number has the recorded start
# time (G1) or, with none, while it is the one process of that number whose
# command line is exactly the one Create was given (G2); PowerShell holds its
# handle while it compares, and kills it and its younger children through
# their handles. The MSYS2 group, which taskkill's tree misses, is killed
# only while its leader is that bash, by /proc's winpid; once the leader is
# gone, only the group's processes that carry the part's mark RACH2_PART in
# their environment are the part's (G5). /proc is read under WINCLEAN_PROC
# when that is set, which the host case uses. It reads no other OUT/ID.*.pid:
# those of the run's claims in WSL name WSL's processes, which release.sh's
# claim_stop ends. Then it stops each process of the run's etude,
# OUT/etude-ID.exe, with its tree, and waits until tasklist shows none; etude
# runs its commands, QEMU among them, in a job that Windows ends with etude.
# Only once both parts and etude are confirmed gone does it touch
# OUT/ID.PART.stopped for each part it stopped, which says that nothing of
# the part works or will (T13). While a part or etude still runs, it removes
# nothing of that run.
#
# Each session of the run has the run directory TREE/build/tmp/ID, which
# ETUDE_RUN_DIR names and which holds the run's TMPDIR and etude's scratch.
# A run that failed, or an etude that kept evidence, wrote ID/KEPT; such a
# run keeps everything, until sweep.sh removes it once its milestone's
# release closed. Otherwise winclean.sh removes the run directory whole, and
# first each path of ID/record whose last line says "made": the tooling job
# directories and partial machine disks that etude makes in TREE/build/tooling,
# which cannot move into the run directory, and which a process stopped by
# force leaves there. The run's own files, OUT/ID and OUT/ID.*, stay for the
# caller to keep or remove.
#
# -l only lists, a line `orphan ID` each, the runs whose parts may still run
# (G8): each ID of OUT with a part's script or launch file and no end, and
# each ID given that has a part with no end, as a start that was sent and has
# made no file yet leaves it.
#
# Exit 0 once nothing of any run ID is left running or can work and its
# scratch is gone or kept, or with -l once the list is whole, and 1 when a
# part or etude of one may still run, Windows cannot say, OUT cannot be read
# or the scratch cannot be removed, so that the caller keeps the run's claims
# in WSL while Windows may still use what they hold.

listing=
[ "${1-}" != -l ] || { listing=1; shift; }
[ "$#" -ge 3 ] || [ -n "$listing" ] && [ "$#" -ge 2 ] ||
    { echo 'usage: winclean.sh [-l] TREE OUT ID...' >&2; exit 2; }
tree=$1 out=$2
shift 2
cd "$out" || { echo "winclean: cannot enter $out" >&2; exit 1; }
set -f
nl='
'
proc=${WINCLEAN_PROC:-/proc}

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
# say. The process is the part when its start time, as a FILETIME, is TIME
# (G1), or, when TIME is empty, when the CIM instance of that number was
# created when the process started and its command line is exactly bash's
# with SCRIPT (G2). The kill goes through the handle that the comparison
# held, and to each child whose start is not before its parent's, through its
# own, so that no number can pass to another process in between.
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
            \$c = Get-CimInstance Win32_Process -Filter 'ProcessId = $1'
            if (-not \$c -or [math]::Abs((\$c.CreationDate - \$p.StartTime).TotalMilliseconds) -ge 1) { 'gone'; exit }
            if (\$c.CommandLine -ne 'C:/msys64/usr/bin/bash.exe -l $3') { 'gone'; exit }
        }
        function Stop-Tree(\$q) {
            \$kids = @(Get-CimInstance Win32_Process -Filter \"ParentProcessId = \$(\$q.Id)\" |
                ForEach-Object { Get-Process -Id \$_.ProcessId -ErrorAction SilentlyContinue } |
                Where-Object { try { \$null = \$_.Handle; -not \$_.HasExited -and \$_.StartTime -ge \$q.StartTime } catch { \$false } })
            try { \$q.Kill() } catch { }
            foreach (\$k in \$kids) { Stop-Tree \$k }
        }
        if ('$4' -ne 'kill') { 'runs'; exit }
        Stop-Tree \$p
        'killed'" < /dev/null 2> /dev/null | tr -d '\r'
}

# leads GROUP WINPID: succeed when the MSYS2 process GROUP, the group's
# leader, is the Windows process WINPID.
leads()
{
    [ -n "$1" ] && [ -n "$2" ] && [ "$(cat "$proc/$1/winpid" 2> /dev/null)" = "$2" ]
}

# marked GROUP MARK: print each MSYS2 process of GROUP whose environment
# holds RACH2_PART=MARK.
marked()
{
    set +f
    for d in "$proc"/[0-9]*; do
        [ "$(cat "$d/pgid" 2> /dev/null)" = "$1" ] || continue
        kill -0 "${d##*/}" 2> /dev/null || continue
        tr '\000' '\n' < "$d/environ" 2> /dev/null | grep -qxF "RACH2_PART=$2" && echo "${d##*/}"
    done
    set -f
}

# halt ID PART: mark the part PART of run ID cancelled, stop what of it runs,
# and succeed once nothing of it is left, waiting up to half a minute (T11 to
# T14). Set p and w to the bash's numbers that its files give.
halt()
{
    p='' t='' w='' g=''
    : > "$1.$2.cancel" || return 1
    # G3: the files are read once, after the mark.
    [ ! -f "$1.$2.pid" ] || read -r w g < "$1.$2.pid"
    [ ! -f "$1.$2.launch" ] || read -r p t < "$1.$2.launch"
    [ "$t" != 0 ] || t=''
    s=$out/$1.$2.sh
    # G5: the group only while its leader is the part's bash.
    wt=''
    [ "$w" != "$p" ] || wt=$t
    if leads "$g" "$w" && [ "$(windows_part "$w" "$wt" "$s")" = runs ]; then
        kill -KILL "-$g" 2> /dev/null
    fi
    i=0
    while :; do
        left=''
        if [ -n "$p" ]; then
            [ "$(windows_part "$p" "$t" "$s" kill)" = gone ] || left=1
        fi
        if [ -n "$w" ] && [ "$w" != "$p" ]; then
            [ "$(windows_part "$w" '' "$s" kill)" = gone ] || left=1
        fi
        if [ -n "$g" ]; then
            for n in $(marked "$g" "$1.$2"); do
                kill -KILL "$n" 2> /dev/null
                left=1
            done
        fi
        [ -n "$left" ] || return 0
        [ "$i" -lt 6 ] || return 1
        i=$((i + 1))
        sleep 5
    done
}

# ended ID PART: succeed when the part PART of run ID has ended (G4, G6).
ended()
{
    [ -e "$1.$2.rc" ] || [ -e "$1.$2.stopped" ]
}

# orphans ID...: print, a line `orphan ID` each, the runs that -l lists.
orphans()
{
    set +f
    for f in *.prepare.sh *.cases.sh *.prepare.launch *.cases.launch; do
        [ -f "$f" ] || continue
        b=${f%.*}
        ended "${b%.*}" "${b##*.}" || echo "${b%.*}"
    done
    set -f
    for id; do
        case $id in */* | .* | '') continue ;; esac
        for part in prepare cases; do
            ended "$id" "$part" || { echo "$id"; break; }
        done
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

if [ -n "$listing" ]; then
    orphans "$@" | sort -u | sed 's/^/orphan /'
    exit 0
fi
status=0
for id; do
    case $id in */* | .* | '') echo "winclean: $id is not a run ID" >&2; continue ;; esac
    left='' halted=''
    for part in prepare cases; do
        ! ended "$id" "$part" || continue
        if halt "$id" "$part"; then
            halted="$halted $part"
        else
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
    # G6: a part is stopped only once etude is gone too.
    for part in $halted; do
        : > "$id.$part.stopped"
    done
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
    # G16: the last removal's own result decides.
    i=0
    until rm -rf $paths 2> /dev/null; do
        if [ "$i" -ge 6 ]; then
            echo "winclean: cannot remove:" $paths >&2
            status=1
            break
        fi
        i=$((i + 1))
        sleep 5
    done
    unset IFS
done
exit "$status"
