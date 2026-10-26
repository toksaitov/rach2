#!/bin/sh
# style_hooks_test.sh: check the git hooks tools/git-hooks/pre-commit and
# pre-push, which tools/verify/hookcheck.sh runs, in a scratch repository
# whose core.hooksPath names this tree's hooks. A commit of a misformatted C
# file must commit it formatted and leave it staged as committed; a commit of
# a Markdown file with an em-dash, and one of a script with a comment line past
# 80 columns, must be refused with a line that names the file and the line; a
# clean commit must pass; and the pre-push hook must refuse a push to master
# of a commit with faults, made without the hooks, change no file, and pass a
# push of clean commits. RACH2_STYLE_GATE is strict, so a missing clang-format
# fails the test instead of skipping the formatting.
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=
trap 'rm -rf ${WORK:+"$WORK"}' 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rach2-style-hooks-test.XXXXXX")
# Report $* and exit 1.
fail() { echo "style-hooks-test: $*" >&2; exit 1; }
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY GIT_COMMON_DIR
GIT_AUTHOR_NAME=test GIT_AUTHOR_EMAIL=test@example.invalid
GIT_COMMITTER_NAME=test GIT_COMMITTER_EMAIL=test@example.invalid
RACH2_STYLE_GATE=strict
export GIT_AUTHOR_NAME GIT_AUTHOR_EMAIL GIT_COMMITTER_NAME GIT_COMMITTER_EMAIL RACH2_STYLE_GATE

# The scratch repository has what the checkers read from the tree they check:
# CLAUDE.md, which marks the root for prosecheck, the .clang-format, and the
# scripts textcheck.sh runs from that tree.
TREE=$WORK/repo
mkdir -p "$TREE/tools/verify"
cd "$TREE"
git init -q
git symbolic-ref HEAD refs/heads/master
git config core.hooksPath "$REPO/tools/git-hooks"
cp "$REPO/.clang-format" .clang-format
cp "$REPO/tools/verify/scripts.sh" "$REPO/tools/verify/pyprose.py" tools/verify/
printf '# Fixture\n\nThe root of a scratch tree.\n' >CLAUDE.md
git add .
git commit -q -m 'Add the fixture tree' || fail "the commit of the clean fixture tree was refused"
base=$(git rev-parse HEAD)

# A misformatted C file is committed formatted, and the index and the file
# match the commit.
printf '/* a.c: a fixture. */\nint  f( void ){return 0;}\n' >tools/a.c
cp tools/a.c "$WORK/a.c"
git add tools/a.c
git commit -q -m 'Add a C file' 2>"$WORK/format.err" || { cat "$WORK/format.err" >&2; fail "the commit of a C file was refused"; }
git show HEAD:tools/a.c >"$WORK/committed.c"
! cmp -s "$WORK/a.c" "$WORK/committed.c" || fail "the C file was committed unformatted"
grep -qx 'int f(void)' "$WORK/committed.c" || fail "the committed C file is not as clang-format formats it"
git diff --quiet HEAD -- tools/a.c || fail "the formatted C file differs from the commit"
grep -q '^style: formatted tools/a.c$' "$WORK/format.err" || fail "the hook did not say that it formatted tools/a.c"

# refused NAME FILE LINE: require the commit of the staged FILE to be refused
# with a line that names LINE of it, and leave the commit as it was.
refused() {
    before=$(git rev-parse HEAD)
    if git commit -q -m "Add $1" 2>"$WORK/$1.err"; then
        fail "the commit with $1 was not refused"
    fi
    grep -q "^style: $2:$3: " "$WORK/$1.err" || { cat "$WORK/$1.err" >&2; fail "the refusal of $1 does not name $2:$3"; }
    [ "$(git rev-parse HEAD)" = "$before" ] || fail "the refused commit with $1 was made"
    git rm -q -f --cached "$2"
    rm -f "$2"
}
printf '# Notes\n\nA claim \342\200\224 and its reason.\n' >tools/notes.md
git add tools/notes.md
refused em-dash tools/notes.md 3
printf '#!/bin/sh\n#\n# %s\necho ok\n' "$(printf '%090d' 0)" >tools/long.sh
git add tools/long.sh
refused long-comment tools/long.sh 3

# A clean commit passes.
printf '# Notes\n\n* A claim, and its reason.\n' >tools/notes.md
git add tools/notes.md
git commit -q -m 'Add the notes' 2>"$WORK/clean.err" || { cat "$WORK/clean.err" >&2; fail "a clean commit was refused"; }
good=$(git rev-parse HEAD)

# A commit made without the hooks, with a spaced double hyphen as a dash in a
# C comment and a misformatted C file, is refused by the pre-push hook, which
# changes no file; the clean commits before it pass.
printf '/* b.c: a fixture -- with a dash. */\nint  g( void ){return 1;}\n' >"$WORK/b.c"
blob=$(git hash-object -w "$WORK/b.c")
GIT_INDEX_FILE=$WORK/index git read-tree "$good"
GIT_INDEX_FILE=$WORK/index git update-index --add --cacheinfo "100644,$blob,tools/b.c"
tree=$(GIT_INDEX_FILE=$WORK/index git write-tree)
bad=$(echo 'Add a fault' | git commit-tree "$tree" -p "$good")
git status --porcelain >"$WORK/status.before"
if printf 'refs/heads/master %s refs/heads/master %s\n' "$bad" "$good" |
    sh "$REPO/tools/git-hooks/pre-push" origin "$WORK/remote" 2>"$WORK/push.err"; then
    fail "the push of a commit with faults was not refused"
fi
grep -q '^style: tools/b.c:1: prose em-dash' "$WORK/push.err" || { cat "$WORK/push.err" >&2; fail "the push refusal does not name the dash in tools/b.c"; }
grep -q '^style: tools/b.c:[0-9]*: not as .* formats it$' "$WORK/push.err" || { cat "$WORK/push.err" >&2; fail "the push refusal does not name the misformatted tools/b.c"; }
grep -q 'hookcheck.sh format tools/b.c' "$WORK/push.err" || fail "the push refusal does not name the command that formats tools/b.c"
git status --porcelain | cmp -s - "$WORK/status.before" || fail "the pre-push hook changed the tree"
printf 'refs/heads/master %s refs/heads/master %s\n' "$good" "$base" |
    sh "$REPO/tools/git-hooks/pre-push" origin "$WORK/remote" 2>"$WORK/push-good.err" ||
    { cat "$WORK/push-good.err" >&2; fail "the push of clean commits was refused"; }
echo "style-hooks-test: passed: formatted and staged, refused a dash and a long comment, passed a clean commit, refused a push with faults"
