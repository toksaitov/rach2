#!/bin/sh
#
# leftovers_test.sh: tools/flow/leftovers.pl removes what finished work left
# in home directories, and nothing else.
#
# A scratch home has the clone `rach2` of a scratch origin, copies beside it,
# worktrees of the clone, failed tests' directories and loose files, all two
# days old unless a row says otherwise. The sweep must remove a copy at master's
# commit with only build output ignored in it, with its sync lock and owner
# files; a copy whose commit master took by cherry-pick after the copy was made,
# which only the clone's objects show; a clean worktree of the clone, with its
# entry in the clone; old failed tests' directories, in the clone and in a copy;
# and an old loose file and directory. It must keep a copy with an uncommitted
# change and one with an untracked file, one with a commit master lacks, one
# with an ignored file that is not build output, one an open issue cites, one
# with a failed test's directory that an open issue cites, one whose tracked
# file was touched now and one named to keep, and change nothing in a copy it
# keeps; a locked worktree; a failed test's directory that an open issue cites,
# one whose run it cites, one changed now and one a process works in; and a
# loose entry that is hidden, begins with rach2, is named to keep, holds a WAD,
# a key, a tarball or a repository, is changed now, is cited at the end of a
# sentence, is a symbolic link or has a process working in it. Without the
# issues' text it must fail and remove nothing, and with -n it must remove
# nothing and say what it would remove.
#
# LEFTOVERS_PL names the leftovers.pl to copy, by default the tree's.
#
# Usage: sh tests/system/leftovers_test.sh
#
set -u

[ "$#" -eq 0 ] || { echo 'leftovers-test: usage: leftovers_test.sh' >&2; exit 2; }
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
busy=
trap '[ -z "$busy" ] || kill $busy 2> /dev/null; [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
made=$(mktemp -d "${TMPDIR:-/tmp}/rach2-leftovers-test.XXXXXX") || exit 1
work=$made
real=$(cd "$made" && pwd -P) || exit 1
case $real in /?*) work=$real ;; *) exit 1 ;; esac
sweep=$work/leftovers.pl
cp "${LEFTOVERS_PL:-$repo/tools/flow/leftovers.pl}" "$sweep" || exit 1
# Spotlight indexes no directory whose name ends in .noindex; its mdworker
# would otherwise open the new copies' files, and the sweep keeps a copy that a
# process uses.
h=$work/home.noindex
f=$h/rach2/build/tests/failed
run=tests.i686.20261020T000000Z

# age PATH...: set the times of each PATH and everything under it, symbolic
# links themselves included, to two days ago.
age()
{
    perl -MFile::Find -e '
        my $t = time - 2 * 86400;
        finddepth({ no_chdir => 1, wanted => sub {
            if (-l) { system "touch", "-h", "-d", "\@$t", $_ } else { utime $t, $t, $_ }
        } }, @ARGV)' "$@"
}

# clone NAME [REF]: clone the origin as the copy NAME of the home, at REF,
# with objects of its own: a hard link to an object of another repository
# would show git's use of that repository as a change in this one.
clone()
{
    git clone -q --no-hardlinks "$work/origin.git" "$h/$1" && git -C "$h/$1" checkout -q --detach "${2:-origin/master}"
}

# setup: make the origin, the home and everything in it that the rows read.
setup()
{
    rm -rf "$work/origin.git" "$work/seed" "$h"
    mkdir -p "$h"
    git init -q --bare -b master "$work/origin.git"
    git init -q "$work/seed"
    cd "$work/seed" || return 1
    git config user.email leftovers@test
    git config user.name leftovers
    git config commit.gpgsign false
    git config core.hooksPath /dev/null
    printf 'build/\n*.wad\n.claude/worktrees/\n' > .gitignore
    echo a > a
    git add .gitignore a && git commit -q -m 'Add a'
    git branch -M master
    git remote add origin "$work/origin.git"
    git switch -q -c 1-picked
    echo x > x && git add x && git commit -q -m 'Add x'
    git push -q origin master 1-picked
    clone rach2-stale origin/1-picked
    git switch -q master
    echo m > m && git add m && git commit -q -m 'Add m'
    git cherry-pick 1-picked > /dev/null
    git push -q origin master
    git clone -q --no-hardlinks "$work/origin.git" "$h/rach2"
    for c in landed dirty untracked unique ignored cited evidence young named; do
        clone "rach2-$c" || return 1
        git -C "$h/rach2-$c" config user.email leftovers@test
        git -C "$h/rach2-$c" config user.name leftovers
    done
    mkdir -p "$h/rach2-landed/build" "$h/rach2-dirty/build/tests/failed/$run.600.machine.g.r0" \
        "$h/rach2-evidence/build/tests/failed/$run.700.machine.h.r0"
    : > "$h/rach2-landed/build/out"
    : > "$h/rach2-landed.sync.lock"
    : > "$h/rach2-landed.sync.owner"
    echo changed > "$h/rach2-dirty/a"
    : > "$h/rach2-untracked/new"
    (cd "$h/rach2-unique" && echo u > u && git add u && git commit -q -m 'Add u')
    : > "$h/rach2-ignored/game.wad"
    git -C "$h/rach2" worktree add -q --detach "$h/rach2/.claude/worktrees/clean"
    git -C "$h/rach2" worktree add -q --detach "$h/rach2/.claude/worktrees/locked"
    git -C "$h/rach2" worktree lock "$h/rach2/.claude/worktrees/locked"
    for d in 100.machine.a 30.machine.d 200.machine.b 300.machine.c 400.machine.e 500.machine.f; do
        mkdir -p "$f/$run.$d.r0" && : > "$f/$run.$d.r0/log"
    done
    mkdir -p "$h/trial/sub" "$h/flow" "$h/keys" "$h/src/.git" "$h/busy"
    for p in c160.log trial/sub/f flow/log .hidden rach2-notes doom1.wad keys/id_ed25519 \
        pkg.tar.gz src/.git/HEAD young.log cited.out; do
        : > "$h/$p"
    done
    ln -s "$h/trial" "$h/link"
    age "$h"
    touch "$h/rach2-young/a"
    : > "$f/$run.400.machine.e.r0/now"
    : > "$h/young.log"
}

# sweep [OPTION...]: run the copy of leftovers.pl on the home as the manager's
# loop does, with the issues' text after `__END__`.
sweep()
{
    { cat "$sweep"; echo __END__; cat "$work/text"; } |
        perl - "$@" -l -m "$(git -C "$h/rach2" rev-parse origin/master)" \
            -k "$h/rach2-named" -k "$h/flow" 86400 "$h"
}

# started PID PATH: wait until /proc/PID/cwd names PATH, for at most 30
# seconds: a busy row means nothing until its process is in its directory.
started()
{
    tries=0
    while [ "$(readlink "/proc/$1/cwd" 2> /dev/null)" != "$2" ]; do
        [ "$tries" -lt 300 ] || return 1
        sleep 0.1
        tries=$((tries + 1))
    done
}

# check ROW...: print `leftovers-test: failed: ROW: ...` for each ROW whose
# condition does not hold, and fail when one does not.
check()
{
    bad=0
    for row in "$@"; do
        case $row in
            no-text) [ -d "$h/rach2-landed" ] && [ -e "$h/c160.log" ] && [ -d "$f/$run.100.machine.a.r0" ] ;;
            dry) [ -d "$h/rach2-landed" ] && [ -e "$h/c160.log" ] &&
                grep -qx "leftovers: would remove copy $h/rach2-landed: [0-9]* KiB" "$work/out" ;;
            copy-landed) [ ! -e "$h/rach2-landed" ] && [ ! -e "$h/rach2-landed.sync.lock" ] &&
                [ ! -e "$h/rach2-landed.sync.owner" ] ;;
            copy-stale) [ ! -e "$h/rach2-stale" ] ;;
            copy-dirty) [ -e "$h/rach2-dirty/a" ] && [ -e "$h/rach2-untracked/new" ] ;;
            copy-unique) [ -e "$h/rach2-unique/u" ] ;;
            copy-ignored) [ -e "$h/rach2-ignored/game.wad" ] ;;
            copy-cited) [ -d "$h/rach2-cited" ] ;;
            copy-evidence) [ -d "$h/rach2-evidence/build/tests/failed/$run.700.machine.h.r0" ] ;;
            copy-untouched) [ -z "$(find "$h/rach2-unique" "$h/rach2-ignored" "$h/rach2-dirty/.git" \
                -newer "$work/stamp")" ] ;;
            copy-young) [ -d "$h/rach2-young" ] ;;
            copy-named) [ -d "$h/rach2-named" ] ;;
            wt-clean) [ ! -e "$h/rach2/.claude/worktrees/clean" ] &&
                ! git -C "$h/rach2" worktree list | grep -q /clean ;;
            wt-locked) [ -d "$h/rach2/.claude/worktrees/locked" ] ;;
            failed-old) [ ! -e "$f/$run.100.machine.a.r0" ] && [ ! -e "$f/$run.30.machine.d.r0" ] ;;
            failed-copy) [ ! -e "$h/rach2-dirty/build/tests/failed/$run.600.machine.g.r0" ] ;;
            failed-cited) [ -d "$f/$run.200.machine.b.r0" ] ;;
            failed-run) [ -d "$f/$run.300.machine.c.r0" ] ;;
            failed-young) [ -d "$f/$run.400.machine.e.r0" ] ;;
            failed-busy) [ ! -d /proc/self/fd ] || [ -d "$f/$run.500.machine.f.r0" ] ;;
            loose-old) [ ! -e "$h/c160.log" ] && [ ! -e "$h/trial" ] ;;
            loose-hidden) [ -e "$h/.hidden" ] ;;
            loose-rach2) [ -e "$h/rach2-notes" ] ;;
            loose-named) [ -e "$h/flow/log" ] ;;
            loose-guarded) [ -e "$h/doom1.wad" ] && [ -e "$h/keys/id_ed25519" ] && [ -e "$h/pkg.tar.gz" ] &&
                [ -e "$h/src/.git/HEAD" ] ;;
            loose-young) [ -e "$h/young.log" ] ;;
            loose-cited) [ -e "$h/cited.out" ] ;;
            loose-link) [ -L "$h/link" ] ;;
            loose-busy) [ ! -d /proc/self/fd ] || [ -d "$h/busy" ] ;;
        esac || { echo "leftovers-test: failed: $row: the row's condition does not hold"; bad=1; }
    done
    [ "$bad" -eq 0 ] || sed 's/^/leftovers-test: output: /' "$work/out"
    return "$bad"
}

# rows: set up and sweep three times, without the issues' text, with -n and
# for real, and check the rows of each; fail when any row failed.
rows()
{
    failed=0
    cat > "$work/text" <<EOF
The kept workspace is ~toksaitov-d/rach2/build/tests/failed/$run.200.machine.b.r0 on the host.
The run $run.300 failed twice, and the copy /home/testing/rach2-cited has the fix.
Its evidence is $run.700.machine.h.r0 in a copy.
Its output is in cited.out.
EOF
    (setup) > "$work/out" 2>&1 || { cat "$work/out"; echo 'leftovers-test: setup failed'; return 1; }
    cat "$sweep" | perl - -l 86400 "$h" > "$work/out" 2>&1 && echo 'leftovers-test: the sweep ran without text' >> "$work/out"
    check no-text || failed=1
    (setup) > "$work/out" 2>&1 || { cat "$work/out"; echo 'leftovers-test: setup failed'; return 1; }
    sweep -n > "$work/out" 2>&1
    check dry || failed=1
    (setup) > "$work/out" 2>&1 || { cat "$work/out"; echo 'leftovers-test: setup failed'; return 1; }
    if [ -d /proc/self/fd ]; then
        (cd "$f/$run.500.machine.f.r0" && exec sleep 86400) &
        busy=$!
        (cd "$h/busy" && exec sleep 86400) &
        busy="$busy $!"
        # shellcheck disable=SC2086 # busy is a list of process ids
        set -- $busy
        if ! started "$1" "$f/$run.500.machine.f.r0" || ! started "$2" "$h/busy"; then
            echo 'leftovers-test: the busy processes did not start'
            # shellcheck disable=SC2086 # busy is a list of process ids
            { kill $busy 2> /dev/null; wait $busy 2> /dev/null; busy=; }
            return 1
        fi
    fi
    touch "$work/stamp"
    sweep > "$work/out" 2>&1
    # shellcheck disable=SC2086 # busy is a list of process ids
    [ -z "$busy" ] || { kill $busy 2> /dev/null; wait $busy 2> /dev/null; busy=; }
    check copy-landed copy-stale copy-dirty copy-unique copy-ignored copy-cited copy-evidence \
        copy-untouched copy-young copy-named wt-clean wt-locked failed-old failed-copy failed-cited failed-run \
        failed-young failed-busy loose-old loose-hidden loose-rach2 loose-named loose-guarded \
        loose-young loose-cited loose-link loose-busy || failed=1
    return "$failed"
}

rows >&2 || exit 1
echo 'leftovers-test: result: pass: 29 rows'
