#!/bin/sh
#
# tidy_test.sh: the loops remove what finished work left, and nothing else:
# `tidy` and `prune_logs` in tools/flow/lib.sh, and tools/flow/scratch.pl.
#
# `tidy` runs in a clone of a scratch origin. It must delete a branch merged
# into master and one whose commit master took by cherry-pick, and keep one
# whose commit only has a subject master has, one with a commit master lacks,
# the branches of the issue held and of the issue parked, and the branch
# checked out; and it must remove a clean helper's worktree under
# .claude/worktrees at a merged commit, with its branch, and keep one with an
# untracked file, a locked one, one with a commit master lacks, and one
# outside .claude/worktrees. `prune_logs` must remove a session's output past
# fourteen days and keep a younger one and files its patterns do not match,
# and under 1 GiB of sparse files remove the oldest until the total fits,
# never one written in the last day. `scratch.pl` must remove an old scratch
# directory and an old scratch file, and keep a directory with a file changed
# now, entries whose names mktemp does not give, a symbolic link, and, where
# /proc exists, a directory a process works in.
#
# With --mutate each fault below is put into private copies of lib.sh and
# scratch.pl and must fail exactly its expected rows; the copies are
# restored before the rows run again at the end. --texts puts each fault
# into the copies and requires only that it changes them, and runs no row.
#
# Usage: sh tests/system/tidy_test.sh [--mutate | --texts]
#
set -u

case ${1:-} in
    '') mode=check ;;
    --mutate) mode=mutate ;;
    --texts) mode=texts ;;
    *) echo 'tidy-test: usage: tidy_test.sh [--mutate | --texts]' >&2; exit 2 ;;
esac
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
busy=
trap '[ -z "$busy" ] || kill "$busy" 2> /dev/null; [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
made=$(mktemp -d "${TMPDIR:-/tmp}/tidy-test.XXXXXX") || exit 1
work=$made
real=$(cd "$made" && pwd -P) || exit 1
case $real in /?*) work=$real ;; *) exit 1 ;; esac
lib=$work/lib.sh
sweep=$work/scratch.pl

# fault NAME: put fault NAME into the private copies, or the originals for
# `none`, and fail when a fault leaves both copies unchanged.
fault()
{
    l=cat s=cat
    # shellcheck disable=SC2016 # the dollars are in the texts sed matches
    case $1 in
        none) ;;
        no-cherry) l='s/^    ! printf .%s\\n. "\$patches" | grep -q .^+.$/    true/' ;;
        no-keep) l='s/^            case \$b in "\$n"-\*) continue 2 ;; esac$/            :/' ;;
        force) l='s/git worktree remove "\$w"/git worktree remove --force --force "\$w"/' ;;
        any-worktree) l='s|^        case \$w in "\$top"/.claude/worktrees/\*) ;; \*) continue ;; esac$|        :|' ;;
        no-age) l='s/next unless \$age > 14 \* 86400 || \$total > 1 << 30;/next unless $total > 1 << 30;/' ;;
        no-young) l='/^            last if \$age < 86400;$/d' ;;
        no-size) l='s/next unless \$age > 14 \* 86400 || \$total > 1 << 30;/next unless $age > 14 * 86400;/' ;;
        no-fresh) s='/^    next if fresh(\$p);$/d' ;;
        any-name) s='/^    next unless \$e =~ /d' ;;
        links) s='s/^    next if \$st\[4\] == 0 or !(-d _ or -f _);$/    next if $st[4] == 0;/' ;;
        no-busy) s='/^    next if grep { \$_ eq \$p or index(\$_, "\$p\/") == 0 } @busy;$/d' ;;
    esac
    if [ "$l" = cat ]; then cat; else sed "$l"; fi < "$repo/tools/flow/lib.sh" > "$lib"
    if [ "$s" = cat ]; then cat; else sed "$s"; fi < "$repo/tools/flow/scratch.pl" > "$sweep"
    [ "$1" = none ] || ! cmp -s "$repo/tools/flow/lib.sh" "$lib" ||
        ! cmp -s "$repo/tools/flow/scratch.pl" "$sweep"
}

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
    printf 'build/\n' > .gitignore
    echo a > a
    git add .gitignore a && git commit -q -m 'Add a'
    git branch -M master
    git remote add origin "$work/origin.git"
    base=$(git rev-parse HEAD)
    for b in 5-held 6-parked 7-current; do git branch "$b" "$base"; done
    git switch -q -c 1-merged
    echo b > b && git add b && git commit -q -m 'Add b'
    git switch -q -c 2-rebased "$base"
    echo c > c && git add c && git commit -q -m 'Add c'
    git switch -q -c 3-subject "$base"
    echo d1 > d && git add d && git commit -q -m 'Add d'
    git switch -q -c 4-unpushed "$base"
    echo e > e && git add e && git commit -q -m 'Add e'
    git switch -q master
    git merge -q --ff-only 1-merged
    git cherry-pick -x 2-rebased > /dev/null
    echo d2 > d && git add d && git commit -q -m 'Add d'
    git push -q origin master
    git fetch -q origin
    for w in clean dirty locked unique; do
        git worktree add -q -b "worktree-$w" ".claude/worktrees/$w" "$base"
    done
    git worktree add -q -b outside "$work/outside" "$base"
    mkdir .claude/worktrees/clean/build
    : > .claude/worktrees/clean/build/out
    : > .claude/worktrees/dirty/untracked
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
    mkdir -p "$t/rach2-old.Ab3dEf/sub" "$t/rach2-busy.Bu5yBu" "$work/target"
    : > "$t/rach2-old.Ab3dEf/sub/f"
    : > "$t/tmp.Fi1eFi1eFi"
    : > "$t/rach2-ports.0"
    : > "$t/notes"
    : > "$t/log.txt"
    : > "$t/rach2-young.Yy3Yy3/now"
    ln -s "$work/target" "$t/link.Li9kLi"
    age 2 "$t/rach2-old.Ab3dEf/sub/f" "$t/rach2-old.Ab3dEf/sub" "$t/rach2-old.Ab3dEf" \
        "$t/tmp.Fi1eFi1eFi" "$t/rach2-ports.0" "$t/notes" "$t/log.txt" \
        "$t/rach2-young.Yy3Yy3" "$t/rach2-busy.Bu5yBu" "$work/target"
    touch -h -t "$(perl -MPOSIX -e 'print strftime("%Y%m%d%H%M", localtime(time - 2 * 86400))')" \
        "$t/link.Li9kLi"
}

# has REF: succeed when the clone has the branch REF.
has()
{
    git -C "$work/clone" rev-parse -q --verify "refs/heads/$1" > /dev/null
}

# rows: set up, run tidy, prune_logs and scratch.pl from the copies, print
# `tidy-test: failed: NAME` for each row that fails, and fail when any did.
rows()
{
    failed=0
    (setup) > "$work/setup.out" 2>&1 || { cat "$work/setup.out"; echo 'tidy-test: setup failed'; return 1; }
    if [ -d /proc/self/fd ]; then
        (cd "$work/tmp/rach2-busy.Bu5yBu" && exec sleep 300) &
        busy=$!
    fi
    # shellcheck source=tools/flow/lib.sh
    (cd "$work/clone" && state=$work/stateA repo=$work/clone &&
        . "$lib" && tidy) > "$work/tidy.out" 2>&1
    # shellcheck source=tools/flow/lib.sh
    (state=$work/stateA && . "$lib" && prune_logs 's-*') >> "$work/tidy.out" 2>&1
    # shellcheck source=tools/flow/lib.sh
    (state=$work/stateB && . "$lib" && prune_logs 's-*') >> "$work/tidy.out" 2>&1
    perl "$sweep" "$work/tmp" >> "$work/tidy.out" 2>&1
    [ -z "$busy" ] || { kill "$busy" 2> /dev/null; wait "$busy" 2> /dev/null; busy=; }
    wt=$work/clone/.claude/worktrees
    a=$work/stateA b=$work/stateB t=$work/tmp
    for row in branch-merged branch-rebased branch-subject branch-unpushed \
        branch-held branch-parked branch-current wt-clean wt-dirty wt-locked \
        wt-unique wt-outside prune-age prune-pattern prune-size prune-young \
        scratch-old scratch-file scratch-young scratch-name scratch-link scratch-busy; do
        case $row in
            branch-merged) ! has 1-merged ;;
            branch-rebased) ! has 2-rebased ;;
            branch-subject) has 3-subject ;;
            branch-unpushed) has 4-unpushed ;;
            branch-held) has 5-held ;;
            branch-parked) has 6-parked ;;
            branch-current) has 7-current ;;
            wt-clean) [ ! -e "$wt/clean" ] && ! has worktree-clean ;;
            wt-dirty) [ -e "$wt/dirty/untracked" ] && has worktree-dirty ;;
            wt-locked) [ -d "$wt/locked" ] && has worktree-locked ;;
            wt-unique) [ -e "$wt/unique/u" ] && has worktree-unique ;;
            wt-outside) [ -d "$work/outside" ] && has outside ;;
            prune-age) [ ! -e "$a/s-old.jsonl" ] && [ -e "$a/s-mid.jsonl" ] ;;
            prune-pattern) [ -e "$a/c-old.jsonl" ] && [ -e "$a/loop.log" ] ;;
            prune-size) [ ! -e "$b/s-big1.jsonl" ] && [ ! -e "$b/s-big2.jsonl" ] ;;
            prune-young) [ -e "$b/s-young.jsonl" ] ;;
            scratch-old) [ ! -e "$t/rach2-old.Ab3dEf" ] ;;
            scratch-file) [ ! -e "$t/tmp.Fi1eFi1eFi" ] ;;
            scratch-young) [ -e "$t/rach2-young.Yy3Yy3/now" ] ;;
            scratch-name) [ -e "$t/rach2-ports.0" ] && [ -e "$t/notes" ] && [ -e "$t/log.txt" ] ;;
            scratch-link) [ -L "$t/link.Li9kLi" ] && [ -d "$work/target" ] ;;
            scratch-busy) [ ! -d /proc/self/fd ] || [ -d "$t/rach2-busy.Bu5yBu" ] ;;
        esac || { echo "tidy-test: failed: $row"; failed=1; }
    done
    [ "$failed" -eq 0 ] || sed 's/^/tidy-test: output: /' "$work/tidy.out"
    return "$failed"
}

# expect FAULT ROWS: with FAULT in the copies, require exactly ROWS to fail.
expect()
{
    fault "$1" || { echo "tidy-test: fault $1 changes nothing" >&2; return 1; }
    rows > "$work/mutated.out" && { echo "tidy-test: fault passed: $1" >&2; return 1; }
    got=$(sed -n 's/^tidy-test: failed: //p' "$work/mutated.out" | tr '\n' ' ')
    if [ "$got" != "$2 " ]; then
        echo "tidy-test: fault $1 failed rows: $got; expected: $2" >&2
        return 1
    fi
}

faults='no-cherry no-keep force any-worktree no-age no-young no-size no-fresh any-name links no-busy'
if [ "$mode" = texts ]; then
    for f in $faults; do
        fault "$f" || { echo "tidy-test: fault $f changes nothing in lib.sh or scratch.pl" >&2; exit 1; }
    done
    exit 0
fi
fault none
rows >&2 || exit 1
if [ "$mode" = mutate ]; then
    expect no-cherry 'branch-subject branch-unpushed wt-unique' || exit 1
    expect no-keep 'branch-held branch-parked' || exit 1
    expect force 'wt-dirty wt-locked' || exit 1
    expect any-worktree wt-outside || exit 1
    expect no-age prune-age || exit 1
    expect no-young prune-young || exit 1
    expect no-size prune-size || exit 1
    expect no-fresh scratch-young || exit 1
    expect any-name scratch-name || exit 1
    expect links scratch-link || exit 1
    if [ -d /proc/self/fd ]; then
        expect no-busy scratch-busy || exit 1
    fi
    fault none
    rows >&2 || exit 1
    echo 'tidy-test: result: pass: every fault caught and restored'
fi
echo 'tidy-test: result: pass: 22 rows'
