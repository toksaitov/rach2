#!/bin/sh
#
# tidy_test.sh: the loops remove what finished work left, and nothing else:
# `tidy` and `prune_logs` in tools/flow/lib.sh, and tools/flow/scratch.pl.
#
# `tidy` runs in a clone of a scratch origin. It must delete a branch merged
# into master, one whose commit master took by cherry-pick and one whose only
# commit merges a branch master has, and keep one whose commit only has a
# subject master has, one with a commit master lacks, one whose merge commit
# adds a file, one that deletes a file master changed, one whose commit
# differs from master's only in whitespace inside a string, one whose commit
# makes in one function the change master made in another, the branches of the
# issue held and of the issue parked, and the branch checked out; and it must
# remove a clean helper's worktree under .claude/worktrees at a merged commit,
# with its branch and its build output, and keep one with an untracked file,
# one with an ignored file that is not build output, a locked one, one with a
# commit master lacks, and one outside .claude/worktrees; a branch rebased
# onto a master that changed its file elsewhere counts as merged. `stems` must
# list the rach2 stems of this tree's mktemp templates and Python prefixes,
# and no other. `prune_logs` must remove a session's output past fourteen days
# and keep a younger one and files its patterns do not match, and under 1 GiB
# of sparse files remove the oldest until the total fits, never one written in
# the last day. `scratch.pl` must remove an old scratch directory, an old
# scratch file and an old directory named as Python's tempfile names them, and
# keep a directory with a file changed now, entries whose names have no stem
# it was given, a stem that does not begin with rach2 or no suffix that mktemp
# gives, a symbolic link, and, where /proc exists, a directory a process works
# in and one that holds a running program.
#
# LIB_SH and SCRATCH_PL name the lib.sh and scratch.pl to copy, by default
# the tree's; tests/proofs/tidy.json runs the rows against copies with faults
# in them.
#
# Usage: sh tests/system/tidy_test.sh
#
set -u

[ "$#" -eq 0 ] || { echo 'tidy-test: usage: tidy_test.sh' >&2; exit 2; }
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
busy=
trap '[ -z "$busy" ] || kill $busy 2> /dev/null; [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
made=$(mktemp -d "${TMPDIR:-/tmp}/rach2-tidy-test.XXXXXX") || exit 1
work=$made
real=$(cd "$made" && pwd -P) || exit 1
case $real in /?*) work=$real ;; *) exit 1 ;; esac
lib=$work/lib.sh
sweep=$work/scratch.pl

cp "${LIB_SH:-$repo/tools/flow/lib.sh}" "$lib" || exit 1
cp "${SCRATCH_PL:-$repo/tools/flow/scratch.pl}" "$sweep" || exit 1

# age DAYS PATH...: set the times of each PATH to DAYS days ago.
age()
{
    days=$1
    shift
    perl -e '$t = time - shift() * 86400; utime $t, $t, @ARGV or exit 1' "$days" "$@"
}

# sparse PATH MIB: make PATH a file of MIB mebibytes with no data in it.
sparse()
{
    perl -e 'open F, ">", $ARGV[0] or exit 1; truncate F, $ARGV[1] * 1048576 or exit 1' "$1" "$2"
}

# setup: make the clone, its branches and worktrees, the logs and the
# scratch directory that the rows read.
setup()
{
    rm -rf "$work/origin.git" "$work/clone" "$work/outside" "$work/stateA" \
        "$work/stateB" "$work/tmp"
    git init -q --bare "$work/origin.git"
    git init -q "$work/clone"
    cd "$work/clone" || return 1
    git config user.email tidy@test
    git config user.name tidy
    git config commit.gpgsign false
    git config core.hooksPath /dev/null
    printf 'build/\n*.wad\n' > .gitignore
    echo a > a
    printf '%s\n' 1 2 3 4 5 6 7 8 9 10 > r
    echo k > k
    for f in f g; do printf '%s()\n{\n\ta\n\tb\n\tc\n\tx = 1\n\td\n\te\n\tf\n}\n' "$f"; done > h
    git add .gitignore a r h k && git commit -q -m 'Add a'
    git branch -M master
    git remote add origin "$work/origin.git"
    base=$(git rev-parse HEAD)
    for b in 5-held 6-parked 7-current; do git branch "$b" "$base"; done
    git switch -q -c 1-merged
    echo b > b && git add b && git commit -q -m 'Add b'
    git switch -q -c 2-rebased "$base"
    sed 's/^9$/nine/' r > r.new && mv r.new r && git commit -q -a -m 'Edit r'
    git switch -q -c 3-subject "$base"
    echo d1 > d && git add d && git commit -q -m 'Add d'
    git switch -q -c 4-unpushed "$base"
    echo e > e && git add e && git commit -q -m 'Add e'
    git switch -q -c 8-merge "$base"
    git merge -q --no-ff --no-commit 1-merged
    echo x > x && git add x && git commit -q -m 'Merge 1-merged'
    git switch -q -c 12-delete "$base"
    git rm -q k && git commit -q -m 'Remove k'
    git switch -q -c 11-pure "$base"
    git merge -q --no-ff -m 'Merge 1-merged' 1-merged
    git switch -q -c 9-space "$base"
    printf 'say "a b"\n' > g && git add g && git commit -q -m 'Add g'
    git switch -q -c 10-place "$base"
    sed '6s/x = 1/x = 2/' h > h.new && mv h.new h && git commit -q -a -m 'Edit h'
    git switch -q master
    git merge -q --ff-only 1-merged
    sed 's/^1$/01/' r > r.new && mv r.new r && git commit -q -a -m 'Edit r first'
    git cherry-pick -x 2-rebased > /dev/null
    echo d2 > d && git add d && git commit -q -m 'Add d'
    printf 'say "ab"\n' > g && git add g && git commit -q -m 'Add g'
    sed '16s/x = 1/x = 2/' h > h.new && mv h.new h && git commit -q -a -m 'Edit h'
    echo k2 > k && git commit -q -a -m 'Edit k'
    git push -q origin master
    git fetch -q origin
    for w in clean dirty ignored locked unique; do
        git worktree add -q -b "worktree-$w" ".claude/worktrees/$w" "$base"
    done
    git worktree add -q -b outside "$work/outside" "$base"
    mkdir .claude/worktrees/clean/build
    : > .claude/worktrees/clean/build/out
    : > .claude/worktrees/dirty/untracked
    : > .claude/worktrees/ignored/game.wad
    git worktree lock .claude/worktrees/locked
    (cd .claude/worktrees/unique && echo u > u && git add u && git commit -q -m 'Add u')
    git switch -q 7-current
    mkdir "$work/stateA" "$work/stateB" "$work/tmp" "$work/tmp/rach2-young.Yy3Yy3"
    echo 5 > "$work/stateA/issue"
    echo 6 > "$work/stateA/parked"
    for f in s-old.jsonl s-mid.jsonl c-old.jsonl loop.log; do : > "$work/stateA/$f"; done
    age 20 "$work/stateA/s-old.jsonl" "$work/stateA/c-old.jsonl" "$work/stateA/loop.log"
    age 5 "$work/stateA/s-mid.jsonl"
    sparse "$work/stateB/s-big1.jsonl" 600 && age 3 "$work/stateB/s-big1.jsonl"
    sparse "$work/stateB/s-big2.jsonl" 600 && age 2 "$work/stateB/s-big2.jsonl"
    sparse "$work/stateB/s-young.jsonl" 1200
    t=$work/tmp
    mkdir -p "$t/rach2-old.Ab3dEf/sub" "$t/rach2-busy.Bu5yBu" "$t/rach2-prog.Pr0gPr" \
        "$t/rach2-py.ab_12cde" "$work/target"
    cp "$(command -v sleep)" "$t/rach2-prog.Pr0gPr/sleep"
    : > "$t/rach2-old.Ab3dEf/sub/f"
    : > "$t/rach2-file.Fi1eFi"
    : > "$t/rach2-ports.0"
    : > "$t/notes"
    : > "$t/log.txt"
    : > "$t/notes.202610"
    : > "$t/report.Ab3dEf"
    : > "$t/rach2-old.Ab3dEfGhI"
    : > "$t/rach2-young.Yy3Yy3/now"
    ln -s "$work/target" "$t/rach2-link.Li9kLi"
    age 2 "$t/rach2-old.Ab3dEf/sub/f" "$t/rach2-old.Ab3dEf/sub" "$t/rach2-old.Ab3dEf" \
        "$t/rach2-file.Fi1eFi" "$t/rach2-ports.0" "$t/notes" "$t/log.txt" "$t/notes.202610" "$t/report.Ab3dEf" \
        "$t/rach2-old.Ab3dEfGhI" "$t/rach2-py.ab_12cde" "$t/rach2-young.Yy3Yy3" "$t/rach2-busy.Bu5yBu" \
        "$t/rach2-prog.Pr0gPr/sleep" "$t/rach2-prog.Pr0gPr" "$work/target"
    touch -h -t "$(perl -MPOSIX -e 'print strftime("%Y%m%d%H%M", localtime(time - 2 * 86400))')" \
        "$t/rach2-link.Li9kLi"
}

# has REF: succeed when the clone has the branch REF.
has()
{
    git -C "$work/clone" rev-parse -q --verify "refs/heads/$1" > /dev/null
}

# started PID LINK PATH: wait until /proc/PID/LINK names PATH, as the sweep
# reads it, for at most 30 seconds: a busy row means nothing until its
# process is in its directory or running its program.
started()
{
    want=$(cd "$(dirname "$3")" && pwd -P)/$(basename "$3")
    tries=0
    while [ "$(readlink "/proc/$1/$2" 2> /dev/null)" != "$want" ]; do
        [ "$tries" -lt 300 ] || return 1
        sleep 0.1
        tries=$((tries + 1))
    done
}

# rows: set up, run tidy, prune_logs and scratch.pl from the copies, print
# `tidy-test: failed: NAME: ...` for each row that fails, and fail when any
# did.
rows()
{
    failed=0
    (setup) > "$work/setup.out" 2>&1 || { cat "$work/setup.out"; echo 'tidy-test: setup failed'; return 1; }
    if [ -d /proc/self/fd ]; then
        (cd "$work/tmp/rach2-busy.Bu5yBu" && exec sleep 300) &
        busy=$!
        (cd / && exec "$work/tmp/rach2-prog.Pr0gPr/sleep" 300) &
        busy="$busy $!"
        # shellcheck disable=SC2086 # busy is a list of process ids
        set -- $busy
        if ! started "$1" cwd "$work/tmp/rach2-busy.Bu5yBu" ||
            ! started "$2" exe "$work/tmp/rach2-prog.Pr0gPr/sleep"; then
            echo 'tidy-test: the busy processes did not start'
            # shellcheck disable=SC2086 # busy is a list of process ids
            { kill $busy 2> /dev/null; wait $busy 2> /dev/null; busy=; }
            return 1
        fi
    fi
    {
        # shellcheck source=tools/flow/lib.sh
        (cd "$work/clone" && state=$work/stateA && . "$lib" && tidy)
        # shellcheck source=tools/flow/lib.sh
        (state=$work/stateA && . "$lib" && prune_logs 's-*')
        # shellcheck source=tools/flow/lib.sh
        (state=$work/stateB && . "$lib" && prune_logs 's-*')
        perl "$sweep" "$work/tmp" 86400 rach2-old rach2-file rach2-py rach2-young rach2-busy \
            rach2-prog rach2-ports rach2-link report
        # shellcheck source=tools/flow/lib.sh
        (cd "$repo" && state=$work/stateA && . "$lib" && stems HEAD) > "$work/stems" 2>&1
    } > "$work/tidy.out" 2>&1
    # shellcheck disable=SC2086 # busy is a list of process ids
    [ -z "$busy" ] || { kill $busy 2> /dev/null; wait $busy 2> /dev/null; busy=; }
    wt=$work/clone/.claude/worktrees
    a=$work/stateA b=$work/stateB t=$work/tmp
    for row in branch-merged branch-rebased branch-subject branch-unpushed \
        branch-held branch-parked branch-current branch-merge branch-pure branch-delete branch-space branch-place wt-clean wt-dirty wt-ignored wt-locked \
        wt-unique wt-outside prune-age prune-pattern prune-size prune-young \
        scratch-old scratch-file scratch-young scratch-name scratch-link scratch-busy \
        scratch-program scratch-stems; do
        case $row in
            branch-merged) ! has 1-merged ;;
            branch-rebased) ! has 2-rebased ;;
            branch-subject) has 3-subject ;;
            branch-unpushed) has 4-unpushed ;;
            branch-held) has 5-held ;;
            branch-parked) has 6-parked ;;
            branch-current) has 7-current ;;
            branch-merge) has 8-merge ;;
            branch-pure) ! has 11-pure ;;
            branch-delete) has 12-delete ;;
            branch-space) has 9-space ;;
            branch-place) has 10-place ;;
            wt-clean) [ ! -e "$wt/clean" ] && ! has worktree-clean ;;
            wt-dirty) [ -e "$wt/dirty/untracked" ] && has worktree-dirty ;;
            wt-ignored) [ -e "$wt/ignored/game.wad" ] && has worktree-ignored ;;
            wt-locked) [ -d "$wt/locked" ] && has worktree-locked ;;
            wt-unique) [ -e "$wt/unique/u" ] && has worktree-unique ;;
            wt-outside) [ -d "$work/outside" ] && has outside ;;
            prune-age) [ ! -e "$a/s-old.jsonl" ] && [ -e "$a/s-mid.jsonl" ] ;;
            prune-pattern) [ -e "$a/c-old.jsonl" ] && [ -e "$a/loop.log" ] ;;
            prune-size) [ ! -e "$b/s-big1.jsonl" ] && [ ! -e "$b/s-big2.jsonl" ] ;;
            prune-young) [ -e "$b/s-young.jsonl" ] ;;
            scratch-old) [ ! -e "$t/rach2-old.Ab3dEf" ] && [ ! -e "$t/rach2-py.ab_12cde" ] ;;
            scratch-file) [ ! -e "$t/rach2-file.Fi1eFi" ] ;;
            scratch-young) [ -e "$t/rach2-young.Yy3Yy3/now" ] ;;
            scratch-name) [ -e "$t/rach2-ports.0" ] && [ -e "$t/notes" ] && [ -e "$t/log.txt" ] &&
                [ -e "$t/notes.202610" ] && [ -e "$t/report.Ab3dEf" ] && [ -e "$t/rach2-old.Ab3dEfGhI" ] ;;
            scratch-link) [ -L "$t/rach2-link.Li9kLi" ] && [ -d "$work/target" ] ;;
            scratch-busy) [ ! -d /proc/self/fd ] || [ -d "$t/rach2-busy.Bu5yBu" ] ;;
            scratch-program) [ ! -d /proc/self/fd ] || [ -e "$t/rach2-prog.Pr0gPr/sleep" ] ;;
            scratch-stems) grep -qx rach2_verify "$work/stems" && grep -qx rach2-measure "$work/stems" &&
                grep -qx rach2-grow-measure "$work/stems" && ! grep -q '[^A-Za-z0-9_-]' "$work/stems" &&
                ! grep -qv '^rach2' "$work/stems" ;;
        esac || { echo "tidy-test: failed: $row: the row's condition does not hold"; failed=1; }
    done
    [ "$failed" -eq 0 ] || sed 's/^/tidy-test: output: /' "$work/tidy.out"
    return "$failed"
}

rows >&2 || exit 1
echo 'tidy-test: result: pass: 30 rows'
