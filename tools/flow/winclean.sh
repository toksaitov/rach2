#!/bin/sh
# shellcheck disable=SC2086 # the paths are a line each
# winclean.sh: on the testing host's Windows, stop what is left of a Windows
# release run and remove its run directory, say whether a run's parts run,
# list the runs that may still run, let one run start alone, or let none.
#
# Usage: winclean.sh TREE OUT ID...
#        winclean.sh -s TREE OUT ID...
#        winclean.sh -l TREE OUT
#        winclean.sh -g TREE OUT ID
#        winclean.sh -a TREE OUT
#
# release.sh, stop.sh and sweep.sh pipe it to the host's MSYS2 bash with
# `bash -l -s ARG...`, TREE being Windows' copy and OUT its output directory,
# as the machines file names them, after the lines that winjob.sh prints,
# which set winjob to the program that winjob.c builds.
# dev-log/84-locks/84-111-lifecycle.md gives the model that it implements.
#
# Each part PART of a run ID, `prepare` or `cases`, runs in the Job Object
# Global\rach2-ID.PART, which `winjob run` makes, and whose processes end
# with it. Its start writes its nonce to OUT/ID.PART.token, failing when that
# file exists, before it creates the helper, and the helper starts the part
# only once it holds the job and the token still holds its nonce. A stop
# replaces the token with a file that holds `stop`, by rename, and then
# terminates the job, so that a start in flight either holds a job that the
# stop ends or reads a token not its own; a start that has created the token
# and not yet written its nonce writes into the file the rename unlinked.
# The part is stopped once `winjob stop` says the job is absent, and then
# winclean.sh touches OUT/ID.PART.stopped. etude runs inside the part that
# started it, so its end comes with the part's. OUT/ID.PART.rc, which its
# bash writes before it exits, gives the part's result and not its end: the
# helper ends the job after it. A part has ended only once a stop wrote
# OUT/ID.PART.stopped, which a stop of a part whose job is already absent
# writes at once.
#
# Each session of the run has the run directory TREE/build/tmp/ID, which
# ETUDE_RUN_DIR names and which holds the run's TMPDIR and etude's scratch.
# A run that failed, or an etude that kept evidence, wrote ID/KEPT; such a
# run keeps everything, until sweep.sh removes it once its milestone's
# release closed. Otherwise, once both parts are stopped, winclean.sh removes
# the run directory whole, and first each path of ID/record whose last line
# says "made": the tooling job directories and partial machine disks that
# etude makes in TREE/build/tooling, which cannot move into the run
# directory, and which a process stopped by force leaves there. The run's
# own files, OUT/ID and OUT/ID.*, stay for the caller to keep or remove.
#
# -s prints, for each part of each run ID, a line `state ID.PART STATE`,
# STATE being what `winjob state` says: `absent`, `active N` or `unknown`.
#
# -l lists, a line `orphan ID` each, the runs of OUT that may still run:
# each with a part whose token exists and that has not ended, a part with a
# status file among them until a stop has confirmed its job absent.
#
# -g writes ID to OUT/windows.current, which a part's start reads after it
# has written its token and before it creates the helper, starting nothing
# for any other run, and then stops each run that -l lists but ID, as the
# first form does; so a start that the gate cannot list reads the new ID and
# starts nothing. It prints a line `left ID` for each run not confirmed
# stopped.
#
# -a does as -g for no run: it writes `none` to OUT/windows.current, so that
# a start that has not yet read it starts nothing, and then stops each run
# that -l lists, printing `left ID` for each not confirmed stopped. stop.sh
# uses it, so that a start still on its way when the release died, before
# its token, starts nothing after the stop.
#
# Exit 0 once nothing of any run ID is left running or can start and its
# scratch is gone or kept, with -s or -l once the answer is whole, and with
# -g or -a once every other run is stopped; exit 1 when a part may still run,
# Windows cannot say, OUT cannot be read or written, the helper is missing or
# the scratch cannot be removed, so that the caller keeps the run's claims in
# WSL while Windows may still use what they hold.

mode=
case ${1-} in -s | -l | -g | -a) mode=$1; shift ;; esac
case $mode in
    -l | -a) [ "$#" -eq 2 ] ;;
    -g) [ "$#" -eq 3 ] ;;
    *) [ "$#" -ge 3 ] ;;
esac || { echo 'usage: winclean.sh [-s | -l | -g | -a] TREE OUT [ID...]' >&2; exit 2; }
tree=$1 out=$2
shift 2
cd "$out" || { echo "winclean: cannot enter $out" >&2; exit 1; }
set -f
nl='
'
if [ "$mode" != -l ] && [ ! -x "${winjob-}" ]; then
    echo "winclean: no winjob program${winjob:+ at $winjob}" >&2
    exit 1
fi

# job ID PART: print the name of the Job Object of part PART of run ID.
job()
{
    printf 'Global\\rach2-%s.%s' "$1" "$2"
}

# halt ID PART: replace the token of part PART of run ID with `stop`,
# terminate its job, and succeed once the job is absent, with the part's
# stopped file. A start may hold the token open, so the token is never
# truncated in place; a rename that a reader holding the file refuses is
# tried again.
halt()
{
    v=
    i=0
    until { echo stop > "$1.$2.token.$$" && mv -f "$1.$2.token.$$" "$1.$2.token"; } 2> /dev/null; do
        if [ "$i" -ge 5 ]; then
            rm -f "$1.$2.token.$$"
            v='no stop token'
            return 1
        fi
        i=$((i + 1))
        sleep 1
    done
    v=$("$winjob" stop "$(job "$1" "$2")" 2> /dev/null)
    [ "$v" = absent ] || return 1
    : > "$1.$2.stopped"
}

# ended ID PART: succeed when a stop has confirmed the part PART of run ID
# ended.
ended()
{
    [ -e "$1.$2.stopped" ]
}

# orphans: print, a line each, the runs that -l lists.
orphans()
{
    set +f
    for f in *.prepare.token *.cases.token; do
        [ -f "$f" ] || continue
        b=${f%.*}
        ended "${b%.*}" "${b##*.}" || echo "${b%.*}"
    done
    set -f
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

# clean ID: stop both parts of run ID and remove its scratch, as the header
# says; fail when a part may still run or the scratch stays.
clean()
{
    left=''
    for part in prepare cases; do
        halt "$1" "$part" && continue
        echo "winclean: the $part of $1 may still run: winjob says ${v:-nothing}" >&2
        left=1
    done
    if [ -n "$left" ]; then
        echo "winclean: $1 still runs; its scratch stays" >&2
        return 1
    fi
    run=$tree/build/tmp/$1
    [ -d "$run" ] || return 0
    if [ -e "$run/KEPT" ]; then
        echo "winclean: $run is kept: $(head -n 1 "$run/KEPT")" >&2
        return 0
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
    # A QEMU that the job's end stopped may hold its files for a few seconds
    # more; the last removal's own result decides.
    i=0
    until rm -rf $paths 2> /dev/null; do
        if [ "$i" -ge 6 ]; then
            echo "winclean: cannot remove:" $paths >&2
            unset IFS
            return 1
        fi
        i=$((i + 1))
        sleep 5
    done
    unset IFS
}

# valid ID: succeed when ID can name a run's files in OUT.
valid()
{
    case $1 in
        */* | .* | '' | *[!A-Za-z0-9._-]*) echo "winclean: $1 is not a run ID" >&2; return 1 ;;
    esac
}

status=0
case $mode in
    -l)
        orphans | sort -u | sed 's/^/orphan /'
        ;;
    -s)
        for id; do
            valid "$id" || { status=1; continue; }
            for part in prepare cases; do
                v=$("$winjob" state "$(job "$id" "$part")" 2> /dev/null)
                echo "state $id.$part ${v:-unknown}"
                [ "$v" = absent ] || [ "${v#active }" != "$v" ] || status=1
            done
        done
        ;;
    -g | -a)
        own=none
        if [ "$mode" = -g ]; then
            valid "$1" || exit 1
            own=$1
        fi
        { echo "$own" > windows.current.$$ && mv -f windows.current.$$ windows.current; } ||
            { echo "winclean: cannot write $out/windows.current" >&2; exit 1; }
        for id in $(orphans | sort -u); do
            [ "$mode" = -a ] || [ "$id" != "$own" ] || continue
            clean "$id" || { echo "left $id"; status=1; }
        done
        ;;
    *)
        for id; do
            valid "$id" || continue
            clean "$id" || status=1
        done
        ;;
esac
exit "$status"
