#!/bin/sh
# shellcheck disable=SC2086 # the paths are a line each
# winclean.sh: on the testing host's Windows, stop what is left of a Windows
# release run and remove its run directory.
#
# Usage: winclean.sh TREE OUT ID...
#
# release.sh and stop.sh pipe it to the host's MSYS2 bash with `bash -l -s
# TREE OUT ID...`, TREE being Windows' copy and OUT its output directory, as
# the machines file names them. For each run ID it stops each process of the
# run's etude, OUT/etude-ID.exe, with its tree, which a stopped ssh leaves
# running, and waits until tasklist shows none; while one still runs, it
# removes nothing of that run. Before that it stops each of the two parts of
# the run that release.sh starts apart from its ssh session, the preparation
# and the cases, whose output, OUT/ID.PART.out, has no status line yet: it
# kills the MSYS2 process group that OUT/ID.PART.pid names after the bash's
# Windows process, since taskkill's tree misses what MSYS2's exec starts, and
# that process with its tree while it runs, and waits until neither is left.
# It reads no other OUT/ID.*.pid: those of the run's claims in WSL name WSL's
# processes, which release.sh's claim_stop ends.
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
# Exit 0 once nothing of any run ID is left running, and 1 when a part or
# etude of one still runs or tasklist cannot say, so that the caller keeps
# the run's claims in WSL while Windows may still use what they hold.

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

# windows_bash PID: print `yes` while Windows shows a bash.exe of number
# PID, `no` when it shows none, and nothing when tasklist cannot say.
windows_bash()
{
    list=$(MSYS_NO_PATHCONV=1 tasklist /NH /FO CSV /FI "PID eq $1" /FI "IMAGENAME eq bash.exe" 2> /dev/null) ||
        return 0
    case $list in
    *'"bash.exe"'*) echo yes ;;
    *) echo no ;;
    esac
}

# halt PID GROUP: stop the part whose bash is Windows process PID and whose
# MSYS2 process group is GROUP, and succeed once neither is left, waiting up
# to half a minute. taskkill goes to PID only while it is a bash.exe, since
# Windows may have given the number to another program since.
halt()
{
    i=0
    while :; do
        [ -z "$2" ] || kill -KILL "-$2" 2> /dev/null
        case $(windows_bash "$1") in
        yes) MSYS_NO_PATHCONV=1 taskkill /F /T /PID "$1" > /dev/null 2>&1 ;;
        no) { [ -n "$2" ] && kill -0 "-$2" 2> /dev/null; } || return 0 ;;
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
        [ -f "$id.$part.pid" ] || continue
        ! grep -aq '^status ' "$id.$part.out" 2> /dev/null || continue
        p='' g=''
        read -r p g < "$id.$part.pid"
        if [ -z "$p" ] || ! halt "$p" "$g"; then
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
