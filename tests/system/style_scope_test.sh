#!/bin/sh
#
# style_scope_test.sh: the files the style check's ShellCheck step and its
# text check read.
#
# tools/verify/scripts.sh lists every in-house shell script for ShellCheck,
# and tools/verify/textcheck.sh reads the code, the scripts and the
# configuration files. Both run here on copies in a private tree with one
# file of each kind. The list must name the scripts, those with no suffix
# among them, and leave out the vendored trees and a list that is not a
# script. The text check must pass the tree as it is, and must find each fault
# planted, one at a time, in a kind of file it reads: a non-ASCII byte in
# tests/registry, a spaced "--" on a manifest line and in an assembly
# comment, a "Phase" tag in a suppression file, a spaced "--" in an echo
# of a script with no suffix, a spaced "--" in a Python docstring, message,
# raised exception and comment, and in a component document a heading out of
# order, one not in the list, a milestone cited by number and a link into a
# milestone's directory. The clean Python file generates a shell command that
# ends its options with "--", which the check must pass. A vendored Readme
# with a heading out of the list must not be read.
#
# Usage: sh tests/system/style_scope_test.sh
#
set -eu

[ "$#" -eq 0 ] || { echo 'style-scope-test: usage: style_scope_test.sh' >&2; exit 2; }

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
. "$repo/tools/etude/hold.sh"
etude_hold "$repo" style-scope-test "$repo/tests/system/style_scope_test.sh" "$@"
work=
trap 'rm -rf ${work:+"$work"}' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-style-scope.XXXXXX")
tree=$work/tree
for d in rach2/sys/arch/i686 krffs/manifests krffs/vendor utils ish tests/host tests/system/fixtures \
    tools/verify tools/setup mk man ports/doom/src vendor/busybox/busybox-9.9 \
    vendor/musl/install vendor/mtools/mtools-9.9 build/tools/verify saved; do
    mkdir -p "$tree/$d"
done
for f in scripts.sh textcheck.sh pyprose.py; do
    cp "$repo/tools/verify/$f" "$tree/tools/verify/$f"
done
# The byte scanner the text check runs, compiled into the private tree; with no
# makefile there, the check's own make finds it present and does nothing.
"${CC:-cc}" -O2 -Wall -Wextra -Werror \
    -o "$tree/build/tools/verify/asciiscan" "$repo/tools/verify/asciiscan.c"

# One clean file of each kind, and the vendored scripts the list must leave out.
printf '#!/bin/sh\necho "run: starting"\n' >"$tree/tests/run"
printf '#!/usr/bin/env bash\necho ready\n' >"$tree/utils/tool"
printf '#!/bin/sh\nset -- a b\necho "$#"\n' >"$tree/tests/host/a_test.sh"
printf 'echo fixture\n' >"$tree/tests/system/fixtures/b.sh"
printf '#!/bin/sh\necho lease\n' >"$tree/krffs/c.script"
printf '# the suites\nstyle|shellcheck|full\n' >"$tree/tests/registry"
printf '# an image\ntext 0644 /motd hello\n' >"$tree/krffs/manifests/x.manifest"
printf '# the exceptions\nvariableScope\n' >"$tree/tools/verify/cppcheck.suppress"
printf '.text\n# the entry point\n_start:\n' >"$tree/rach2/sys/arch/i686/loader.s"
cat >"$tree/utils/gen.py" <<'EOF'
"""Write a script that removes a file."""

SCRIPT = """#!/bin/sh
rm -f -- "$1"
"""

print("gen: writing the script")
EOF
for f in vendor/busybox/busybox-9.9/v.sh vendor/musl/install/w.sh ports/doom/src/u.sh \
    krffs/vendor/t.sh vendor/mtools/mtools-9.9/m.sh; do
    printf '#!/bin/sh\necho vendored\n' >"$tree/$f"
done
printf '# mk\n\nA summary.\n\n## Interfaces\n\n## Tests\n' >"$tree/mk/Readme.md"
printf '# man\n\nA summary.\n\n## Use\n' >"$tree/man/Readme.md"
printf '# busybox\n\n## Notes\n' >"$tree/vendor/busybox/busybox-9.9/Readme.md"
printf '# mtools\n\n## Notes\n' >"$tree/vendor/mtools/mtools-9.9/Readme.md"
for f in tests/run utils/tool tests/registry krffs/manifests/x.manifest \
    tools/verify/cppcheck.suppress rach2/sys/arch/i686/loader.s mk/Readme.md \
    man/Readme.md utils/gen.py; do
    mkdir -p "$tree/saved/$(dirname "$f")"
    cp "$tree/$f" "$tree/saved/$f"
done
expected='krffs/c.script
tests/host/a_test.sh
tests/run
tests/system/fixtures/b.sh
tools/verify/scripts.sh
tools/verify/textcheck.sh
utils/tool'

# pass ROW: report that ROW passed.
pass()
{
    echo "style-scope-test: result: pass: $1"
}

# fail ROW DETAIL: report that ROW failed and add it to $failed.
fail()
{
    echo "style-scope-test: result: fail: $1: $2"
    failed="$failed $1"
}

# The text check over the private tree: its output in $out and its status in
# $rc.
textcheck()
{
    rc=0
    out=$(sh "$tree/tools/verify/textcheck.sh" 2>&1) || rc=$?
}

# planted NAME FILE LINE FINDING: the text in $work/plant, added to FILE as
# line LINE, must make the text check fail with FINDING reported at that line.
# The text comes from a file and not a pipe, since the last command of a
# pipeline runs in a subshell and would lose what `fail` records.
planted()
{
    cat "$work/plant" >>"$tree/$2"
    textcheck
    cp "$tree/saved/$2" "$tree/$2"
    case $rc:$out in
        1:*"$2:$3:"*"$4"*) pass "$1" ;;
        *) fail "$1" "status $rc: $out" ;;
    esac
}

# Run every row on the private tree, with the names of those that fail in
# $failed.
rows()
{
    failed=
    listed=$(sh "$tree/tools/verify/scripts.sh")
    if [ "$listed" = "$expected" ]; then
        pass scripts-list
    else
        fail scripts-list "$(printf '%s' "$listed" | tr '\n' ' ')"
    fi
    textcheck
    case $rc:$out in
        0:*'textcheck: clean'*) pass text-clean ;;
        *) fail text-clean "status $rc: $out" ;;
    esac
    printf 'core|x \342\200\224 y\n' >"$work/plant"
    planted text-config-ascii tests/registry 3 'non-ASCII byte'
    printf 'text 0644 /a one -- two\n' >"$work/plant"
    planted text-config-dash krffs/manifests/x.manifest 3 '[prose em-dash]'
    printf '# Phase 2 leftovers\n' >"$work/plant"
    planted text-config-diary tools/verify/cppcheck.suppress 3 '[Phase/Part diary tag]'
    printf '# save -- then return\n' >"$work/plant"
    planted text-code-dash rach2/sys/arch/i686/loader.s 4 '[prose em-dash]'
    # The dashes come from a variable, so this line is not the fault it plants.
    dashes=--
    printf 'echo "a %s b"\n' "$dashes" >"$work/plant"
    planted text-script-dash tests/run 3 '[prose em-dash in a message]'
    printf '"""Write it %s then run it."""\n' "$dashes" >"$work/plant"
    planted text-python-docstring utils/gen.py 8 '[prose em-dash]'
    printf 'print("gen: done %s the script is written")\n' "$dashes" >"$work/plant"
    planted text-python-message utils/gen.py 8 '[prose em-dash]'
    printf 'raise SystemExit(\n    "gen: no script %s see above")\n' "$dashes" >"$work/plant"
    planted text-python-raise utils/gen.py 9 '[prose em-dash]'
    printf '# Write it %s then run it.\n' "$dashes" >"$work/plant"
    planted text-python-comment utils/gen.py 8 '[prose em-dash]'
    printf '## Design\n' >"$work/plant"
    planted text-doc-order mk/Readme.md 8 '[heading out of order]'
    printf '## Notes\n' >"$work/plant"
    planted text-doc-heading mk/Readme.md 8 '[heading not in the list]'
    printf 'Milestone 42 added it.\n' >"$work/plant"
    planted text-doc-milestone mk/Readme.md 8 '[milestone cited]'
    printf 'See [the plan](../dev-log/42-x/plan.md).\n' >"$work/plant"
    planted text-doc-link man/Readme.md 6 '[milestone cited]'
}

rows
[ -z "$failed" ] || exit 1
echo "style-scope-test: result: pass: every row"
