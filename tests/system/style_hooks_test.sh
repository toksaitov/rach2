#!/bin/sh
# style_hooks_test.sh: check the git hook tools/git-hooks/pre-commit, which
# tools/verify/hookcheck.sh runs, in a scratch repository whose
# core.hooksPath names this tree's hooks. A commit of a misformatted C file
# must commit it formatted and leave it staged as committed; a misformatted C
# file with unstaged changes, or one whose formatting cannot be set up, must
# be committed as staged, with a warning; a commit of other files must pass
# untouched; and a function past a size limit, a comment past 80 columns or a
# banned term in a staged file must be refused. Without the recorded
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

# A C file whose formatting cannot be set up, here for want of a scratch
# directory, is committed as staged, with a warning.
printf '/* c.c: a fixture. */\nint  h( void ){return 2;}\n' >tools/c.c
cp tools/c.c "$WORK/c.c"
git add tools/c.c
: >"$WORK/plain"
TMPDIR=$WORK/plain/tmp git commit -q -m 'Add a third C file' 2>"$WORK/unready.err" || { cat "$WORK/unready.err" >&2; fail "the commit was refused when the formatting could not be set up"; }
git show HEAD:tools/c.c | cmp -s - "$WORK/c.c" || fail "the C file was not committed as staged when the formatting could not be set up"
grep -q 'hookcheck: warning: no scratch directory' "$WORK/unready.err" || { cat "$WORK/unready.err" >&2; fail "the hook did not warn that it could not set up the formatting"; }
# A commit of other files passes, and the hook leaves them as they are.
printf '# Notes\n\nA claim \342\200\224 and its reason.\n' >tools/notes.md
cp tools/notes.md "$WORK/notes.md"
git add tools/notes.md
git commit -q -m 'Add the notes' 2>"$WORK/notes.err" || { cat "$WORK/notes.err" >&2; fail "a commit of a Markdown file was refused"; }
git show HEAD:tools/notes.md | cmp -s - "$WORK/notes.md" || fail "the hook changed a Markdown file"
# A function past a size limit is refused, and nothing is committed.
printf '/* d.c: a fixture. */\nint k(int a, int b, int c, int d, int e, int f, int g)\n{\n    return a + b + c + d + e + f + g;\n}\n' >tools/d.c
git add tools/d.c
head=$(git rev-parse HEAD)
! git commit -q -m 'Add a long signature' 2>"$WORK/sizes.err" || fail "a function of seven parameters was committed"
[ "$(git rev-parse HEAD)" = "$head" ] || fail "the refused commit moved HEAD"
grep -q '^sizecheck: tools/d.c:k: params 7' "$WORK/sizes.err" || { cat "$WORK/sizes.err" >&2; fail "the hook did not name the function past the limit"; }
grep -q '^hookcheck: refused: ' "$WORK/sizes.err" || { cat "$WORK/sizes.err" >&2; fail "the hook did not say that it refused the commit"; }
git reset -q
rm -f tools/d.c

# A comment of 81 columns in a staged file is refused, in C and in shell, and
# a term the register rule bans too; 80 columns are not. A finding in a file
# the commit does not stage is left alone.
wide=$(printf '%074d' 0)
: >CLAUDE.md
printf '/* w.c: a fixture. */\n/* %s */\n' "$wide" >tools/w.c
git add CLAUDE.md tools/w.c
git commit -q -m 'Add a comment of 80 columns' 2>"$WORK/w80.err" || { cat "$WORK/w80.err" >&2; fail "a comment of 80 columns was refused"; }
printf '/* w.c: a fixture. */\n/* %s0 */\n' "$wide" >tools/w.c
git add tools/w.c
head=$(git rev-parse HEAD)
! git commit -q -m 'Widen a comment' 2>"$WORK/w81.err" || fail "a comment of 81 columns was committed"
[ "$(git rev-parse HEAD)" = "$head" ] || fail "the refused commit moved HEAD"
grep -q '^tools/w.c:2: comment line is 81 columns$' "$WORK/w81.err" || { cat "$WORK/w81.err" >&2; fail "the hook did not name the wide comment"; }
git reset -q
printf '#!/bin/sh\n# %s\n' "$wide$wide" >tools/w.sh
git add tools/w.sh
! git commit -q -m 'Add a wide script' 2>"$WORK/wsh.err" || fail "a wide comment in a script was committed"
git reset -q
rm tools/w.sh
printf '/* n.c: a fixture, load-''bearing. */\n' >tools/n.c
git add tools/n.c
! git commit -q -m 'Add a banned term' 2>"$WORK/term.err" || fail "a banned term was committed"
grep -q '^hookcheck: refused: ' "$WORK/term.err" || { cat "$WORK/term.err" >&2; fail "the hook did not refuse the banned term"; }
git reset -q
rm tools/n.c
printf 'notes\n' >tools/other.md
git add tools/other.md
git commit -q -m 'Add a note beside a wide comment' 2>"$WORK/other.err" || { cat "$WORK/other.err" >&2; fail "a finding in an unstaged file refused the commit"; }

echo "style-hooks-test: passed: formatted and staged a C file, kept a file with unstaged changes, committed a file it could not format, passed a Markdown file, refused a function past a size limit, a wide comment and a banned term"
