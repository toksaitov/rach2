#!/bin/sh
# shellcheck disable=SC2086 # the paths are a line each
# winclean.sh: on the testing host's Windows, stop what is left of a Windows
# release run and remove the scratch it made outside its own files.
#
# Usage: winclean.sh TREE OUT ID...
#
# release.sh and stop.sh pipe it to the host's MSYS2 bash with `bash -l -s
# TREE OUT ID...`, TREE being Windows' copy and OUT its output directory, as
# the machines file names them. For each run ID it stops each process of the
# run's etude, OUT/etude-ID.exe, with its tree, which a stopped ssh leaves
# running, and waits until tasklist shows none; while one still runs, it
# removes nothing of that run.
#
# The run's etude and the etude programs under it note each scratch path they
# make, keep as evidence or remove in the run's scratch record,
# OUT/ID.scratch, which ETUDE_SCRATCH_RECORD names: the collection workspaces
# under TREE/build/tests and the tooling jobs, provisioning directories and
# partial machine disks under TREE/build/tooling. etude removes its scratch
# itself when it ends or a signal stops it, but keeps what its output names
# as evidence; a process stopped by force, as taskkill /F and Stop-Process
# -Force stop one, leaves its scratch. winclean.sh removes each path under
# TREE whose last line in the record says "made", so that what another run
# made in the same copy and what this run kept as evidence stay. It removes
# the run's TMPDIR, OUT/ID.tmp, too, unless the record keeps a path in it.
# The run's own files, OUT/ID and OUT/ID.*, stay for the caller to keep or
# remove.

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

for id; do
    MSYS_NO_PATHCONV=1 taskkill /F /T /IM "etude-$id.exe" > /dev/null 2>&1
    if ! stopped "$id"; then
        echo "winclean: etude-$id.exe still runs; its scratch stays" >&2
        continue
    fi
    paths=
    keep_tmp=
    if [ -f "$id.scratch" ]; then
        IFS=$nl
        for path in $(abandoned "$id.scratch"); do
            case $path in
            */../* | */..) ;;
            "$tree"/build/tests/rach2-collection.* | "$tree"/build/tooling/job-* | \
                "$tree"/build/tooling/prepare-* | "$tree"/build/tooling/tooling-*.part)
                paths=$paths$nl$path
                continue
                ;;
            esac
            echo "winclean: $id.scratch names $path, outside the run's scratch; it stays" >&2
        done
        unset IFS
        awk -v p="kept $out/$id.tmp/" 'index($0, p) == 1 { f = 1 } END { exit !f }' "$id.scratch" &&
            keep_tmp=1
    fi
    [ -n "$keep_tmp" ] || paths=$id.tmp$paths
    [ -n "$paths" ] || continue
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
