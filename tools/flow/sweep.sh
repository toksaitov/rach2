#!/bin/sh
#
# sweep.sh: remove what finished work left on the testing host, on Windows
# and on this computer, as the Resources section of AGENTS.md allows.
# release.sh and stop.sh run it, and the manager whenever space runs low.
#
# Usage: sweep.sh [-a]
#
# It runs `tidy` from lib.sh in this checkout, which removes its landed
# worktrees and branches that no process and no run evidence or loop state
# keeps; prunes, as `prune_logs` in lib.sh does, the session logs of each loop
# state directory under this checkout's build/flow, which a loop itself prunes
# only while it runs and holds no task; and runs `scratch.pl` with the stems of
# master's scratch names, here on this checkout's build/tmp and on that of the
# Mac's release copy, build/machines/mac/tree, with the release runs'
# directories there that hold no KEPT, and as root on the testing host on its
# /tmp and on the build/tmp of the owner's copy and of each account's clone,
# which removes the scratch of tests that were killed before their traps ran.
# It then runs `leftovers.pl`, through `leftovers` in lib.sh, on the host and
# here, which removes failed tests' directories, loose scratch in home
# directories and copies of the repository whose work master has; and runs
# `store_expire`, which removes the entries of the host's store of step outputs
# that no copy uses. It runs `tidy` in each worker's clone too, since a loop
# tidies only while it runs. `outputs` removes what a build or a run makes
# again and what nothing needs once its run has ended: in each tree on the
# testing host whose loop holds no session, the images, test output and scratch
# unchanged for a day, but `build/tests/failed`, and the run files of
# `build/runs` unchanged for two weeks; on Windows the test collections,
# tooling job directories, case images and scratch, the old shared TMPDIR of
# the release runs among it, unchanged for a day, but what the record or KEPT
# of a release run that is live or kept names, and the release runs'
# directories in build/tmp unchanged for a day that hold no KEPT, whose parts'
# jobs are absent and whose parts have ended; each release run,
# with its files, its etude and its kept directory, of a milestone whose
# release closed; and here the entries of `build/tests` unchanged for two weeks
# and the Mac's kept run directories of a milestone whose release closed. The
# caches, the build copies of the release and of Windows, and the tooling work
# disks stay. Each removal, and each step's total, is a line on stdout. Each
# step that reaches the host stops itself after ten minutes. With -a, which
# stop.sh passes once nothing runs, what has lain unused for a day or less goes
# too, so that the checks of age of a day count for nothing; the two weeks of
# `build/runs` and `build/tests` stay.

TESTING=testing

# stale is the shell function that `outputs` runs on each machine: stale DAYS
# PATH... removes each PATH in which nothing changed for DAYS days, but a
# PATH whose last component is a line of `keep`, which names what a live or
# kept run uses, whatever its age.
# shellcheck disable=SC2016 # the dollars are the function's
stale='
stale()
{
    days=$1
    shift
    for p in "$@"; do
        [ -e "$p" ] || continue
        case $p in */tests/failed) continue ;; esac
        if [ -n "${keep:-}" ] && printf "%s\n" "$keep" | grep -qxF "${p##*/}"; then
            echo "sweep: keeps $p: a live or kept run names it"
            continue
        fi
        [ -z "$(find "$p" -mtime -"$days" -print -quit 2> /dev/null)" ] || continue
        size=$(du -sk "$p" | cut -f 1)
        rm -rf "$p" && echo "sweep: removed $p: $size KiB"
    done
}
'

# closed: print, a line each, the milestones whose release closed, as the
# records release.sh keeps of the releases that ended say.
closed()
{
    for r in "$repo"/build/flow/release/[0-9]*-*; do
        case $r in *.sha256 | *.sweep) continue ;; esac
        grep -q '^closed ' "$r" 2> /dev/null && basename "$r" | sed 's/-.*//'
    done | sort -un
}

# outputs: remove what the header says from each machine.
outputs()
{
    {
        printf '%s\n' "$stale"
        for h in $HOMES; do
            b=$h/rach2/build
            printf '%s\n' "[ -s $b/flow/claude/session ] || stale $days $b/images/* $b/x86_64/images/*" \
                "$b/i686/images/* $b/tests/* $b/x86_64/tests/* $b/i686/tests/* $b/tmp/*" |
                paste -s -d ' ' -
            echo "stale 14 $b/runs/*"
        done
    } | perl -e 'alarm shift; exec @ARGV' 600 ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" \
        'MSYS_NO_PATHCONV=1 wsl.exe -u root -- sh -s' 2>&1 | tr -d '\000\r' | sed "s/^/$TESTING: /"
    {
        printf '%s\n' "$stale"
        echo 'b=/d/rach2/build'
        echo "closed=' $(closed | tr '\n' ' ')'"
        sh "$(dirname "$0")/winjob.sh" "$(awk '$1 == "windows" && $2 == "out" { print $3; exit }' \
            "$(dirname "$0")/machines")"
        # A run in build/tmp is live while the job of one of its parts is
        # not absent, as winjob.c says, or one of its parts has a token in
        # OUT and has not ended, as winclean.sh -l lists it, and every run is
        # while Windows cannot say which. Each path that the record or KEPT
        # of a live or kept run names, such as a tooling job directory of
        # the cache, which cannot move into the run's directory, goes into
        # keep before any age rule, unless the run's milestone closed its
        # release.
        cat << 'END'
nl='
'
keep= live=
# unended ID: succeed when a part of run ID has a token in OUT and no
# stopped file, as winclean.sh -l judges it: a status file comes before the
# job's end.
unended()
{
    for u in prepare cases; do
        u=$b/release/$1.$u
        [ -e "$u.token" ] || continue
        [ -e "$u.stopped" ] || return 0
    done
    return 1
}
# running ID: succeed when the job of a part of run ID is not absent, or
# Windows cannot say.
running()
{
    for u in prepare cases; do
        [ "$("$winjob" state "Global\\rach2-$1.$u" 2> /dev/null)" = absent ] || return 0
    done
    return 1
}
[ ! -x "${winjob-}" ] || live=1
for d in $b/tmp/*; do
    [ -d "$d" ] || continue
    id=${d##*/}
    m=${id#milestone}
    case $closed in *" ${m%%-*} "*) continue ;; esac
    if [ -n "$live" ] && [ ! -e "$d/KEPT" ] && ! unended "$id" && ! running "$id"; then
        continue
    fi
    keep=$keep$nl$(sed -n 's/^\(made\|removed\|kept\) //p' "$d/record" "$d/KEPT" 2> /dev/null |
        sed 's,.*/,,')
done
END
        # shellcheck disable=SC2016 # the dollars are the remote shell's
        echo "stale $days \$b/tests/rach2-collection.* \$b/tooling/job-* \$b/images/cases/*" \
            '$b/x86_64/images/cases/* $b/release/tmp/*'
        # A run's directory stays while it holds KEPT or it is live, and all
        # of build/tmp while Windows cannot say which run is.
        # shellcheck disable=SC2016 # the dollars are the remote shell's
        printf '%s\n' 'if [ -n "$live" ]; then' \
            '    for d in $b/tmp/*; do' \
            '        [ ! -e "$d/KEPT" ] || continue' \
            '        ! unended "${d##*/}" || continue' \
            '        ! running "${d##*/}" || continue' \
            "        stale $days \"\$d\"" \
            '    done' \
            'fi'
        # A milestone whose release closed runs nothing on Windows again, so
        # its runs there, failed ones with their evidence among them, go
        # whatever their age.
        for m in $(closed); do
            echo "stale 0 \$b/release/milestone$m-*-windows-* \$b/release/etude-milestone$m-*-windows-*" \
                "\$b/tmp/milestone$m-*-windows-*"
        done
    } | perl -e 'alarm shift; exec @ARGV' 600 ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" \
        'bash -s' 2>&1 | tr -d '\000\r' | sed 's/^/windows: /'
    eval "$stale"
    stale 14 "$repo"/build/tests/* 2>&1 | sed "s/^/$(hostname -s): /"
    # The Mac's kept runs, which scratch.pl leaves, of a milestone whose
    # release closed.
    for m in $(closed); do
        stale 0 "$repo"/build/machines/mac/tree/build/tmp/milestone"$m"-*-mac-*
    done 2>&1 | sed "s/^/$(hostname -s): /"
    git worktree prune
}

# clones: run `tidy` in each worker's clone, as its account.
clones()
{
    grep -v '^#' "$(dirname "$0")/workers" | while read -r name host _; do
        [ -n "$name" ] && [ "$host" != - ] || continue
        {
            cat "$(dirname "$0")/lib.sh"
            # shellcheck disable=SC2016 # the dollar is the remote shell's
            printf '%s\n' 'log() { echo "$*"; }' 'cd rach2 && git fetch -q origin && tidy'
        } | perl -e 'alarm shift; exec @ARGV' 600 ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" \
            sh -s 2>&1 | sed "s/^/$name: /"
    done
}

main()
{
    age=86400 days=1
    if [ "${1:-}" = -a ]; then
        age=0 days=0
    fi
    repo=$(cd "$(dirname "$0")/../.." && pwd)
    cd "$repo" || exit 1
    git fetch -q origin || echo "sweep: the fetch failed; master's stems may be old"
    state=
    tidy
    for state in "$repo"/build/flow/*; do
        [ -f "$state/loop.log" ] && prune_logs 's-*'
    done
    state=
    clones
    stems=$(stems origin/master | tr '\n' ' ')
    # shellcheck disable=SC2086 # the stems are a word each
    perl "$(dirname "$0")/scratch.pl" -r -d "$repo/build/tmp" \
        -d "$repo/build/machines/mac/tree/build/tmp" "$age" $stems 2>&1 |
        sed "s/^/$(hostname -s): /"
    dirs='-d /tmp'
    for h in $HOMES; do
        dirs="$dirs -d $h/rach2/build/tmp"
    done
    perl -e 'alarm shift; exec @ARGV' 600 \
        ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" \
        "MSYS_NO_PATHCONV=1 wsl.exe -u root -- perl - $dirs $age $stems" \
        < "$(dirname "$0")/scratch.pl" 2>&1 | sed "s/^/$TESTING: /"
    # shellcheck disable=SC2119 # leftovers takes -n, not the sweep's arguments
    leftovers 2>&1 | grep -v ': leftovers: keeps '
    outputs
    store_expire 2>&1
}

# shellcheck source=tools/flow/lib.sh
. "$(dirname "$0")/lib.sh"

# log TEXT: print TEXT, with the loop state directory it concerns, if any,
# first, since the sweep's removals are lines of its output and not of a
# loop's log.
log()
{
    echo "${state:+$state: }$*"
}

main "$@"
