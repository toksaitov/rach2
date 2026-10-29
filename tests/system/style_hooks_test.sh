#!/bin/sh
# style_hooks_test.sh: check the git hook tools/git-hooks/pre-commit, which
# tools/verify/hookcheck.sh runs, in a scratch repository whose
# core.hooksPath names this tree's hooks. A commit of a misformatted C file
# must commit it formatted and leave it staged as committed; a misformatted C
# file with unstaged changes must be committed as staged, with a warning; and
# a commit of other files must pass untouched. Without the recorded
# clang-format the hook formats nothing, and the first check fails.
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
export GIT_AUTHOR_NAME GIT_AUTHOR_EMAIL GIT_COMMITTER_NAME GIT_COMMITTER_EMAIL

TREE=$WORK/repo
mkdir -p "$TREE/tools"
cd "$TREE"
git init -q
git symbolic-ref HEAD refs/heads/master
git config core.hooksPath "$REPO/tools/git-hooks"
cp "$REPO/.clang-format" .clang-format
git add .
git commit -q -m 'Add the fixture tree' || fail "the commit of the fixture tree was refused"

# A misformatted C file is committed formatted, and the index and the file
# match the commit.
printf '/* a.c: a fixture. */\nint  f( void ){return 0;}\n' >tools/a.c
cp tools/a.c "$WORK/a.c"
git add tools/a.c
git commit -q -m 'Add a C file' 2>"$WORK/format.err" || { cat "$WORK/format.err" >&2; fail "the commit of a C file was refused"; }
git show HEAD:tools/a.c >"$WORK/committed.c"
! cmp -s "$WORK/a.c" "$WORK/committed.c" || fail "the C file was committed unformatted"
grep -qx 'int f(void)' "$WORK/committed.c" || { cat "$WORK/format.err" >&2; fail "the committed C file is not as clang-format formats it"; }
git diff --quiet HEAD -- tools/a.c || fail "the formatted C file differs from the commit"
grep -q '^style: formatted tools/a.c$' "$WORK/format.err" || fail "the hook did not say that it formatted tools/a.c"

# A misformatted C file with unstaged changes is committed as staged, and
# its working copy keeps those changes.
printf '/* b.c: a fixture. */\nint  g( void ){return 1;}\n' >tools/b.c
cp tools/b.c "$WORK/b.c"
git add tools/b.c
printf '/* b.c: a fixture, changed. */\nint  g( void ){return 1;}\n' >tools/b.c
cp tools/b.c "$WORK/b-work.c"
git commit -q -m 'Add another C file' 2>"$WORK/unstaged.err" || { cat "$WORK/unstaged.err" >&2; fail "the commit of a C file with unstaged changes was refused"; }
git show HEAD:tools/b.c | cmp -s - "$WORK/b.c" || fail "the C file with unstaged changes was not committed as staged"
cmp -s tools/b.c "$WORK/b-work.c" || fail "the hook changed the working copy of a file with unstaged changes"
grep -q 'tools/b.c is not formatted and has unstaged changes' "$WORK/unstaged.err" || { cat "$WORK/unstaged.err" >&2; fail "the hook did not warn about tools/b.c"; }

# A commit of other files passes, and the hook leaves them as they are.
printf '# Notes\n\nA claim \342\200\224 and its reason.\n' >tools/notes.md
cp tools/notes.md "$WORK/notes.md"
git add tools/notes.md
git commit -q -m 'Add the notes' 2>"$WORK/notes.err" || { cat "$WORK/notes.err" >&2; fail "a commit of a Markdown file was refused"; }
git show HEAD:tools/notes.md | cmp -s - "$WORK/notes.md" || fail "the hook changed a Markdown file"
echo "style-hooks-test: passed: formatted and staged a C file, kept a file with unstaged changes, passed a Markdown file"
