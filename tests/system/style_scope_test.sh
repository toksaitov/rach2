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
# comment, a "Phase" tag in a suppression file, and a spaced "--" in an echo
# of a script with no suffix.
#
# With --mutate each fault below is put into the private copy of one checker,
# must fail the row named for it, and is restored and touched before every row
# runs again.
#
# Usage: sh tests/system/style_scope_test.sh [--mutate]
#
set -eu

case ${1:-} in
    '') mutate=0 ;;
    --mutate) mutate=1 ;;
    *) echo 'style-scope-test: usage: style_scope_test.sh [--mutate]' >&2; exit 2 ;;
esac

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/style-scope.XXXXXX")
trap 'rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
tree=$work/tree
for d in rach2 krffs/manifests krffs/vendor utils ish tests/host tests/system/fixtures \
    tools/verify tools/setup mk man ports/doom/src vendor/busybox/busybox-9.9 \
    vendor/musl/install build/tools/verify saved; do
    mkdir -p "$tree/$d"
done
for f in scripts.sh textcheck.sh; do
    cp "$repo/tools/verify/$f" "$tree/tools/verify/$f"
    cp "$repo/tools/verify/$f" "$tree/saved/$f"
done
# The byte scanner the text check runs, compiled into the private tree; with no
# makefile there, the check's own make finds it present and does nothing.
"${CC:-cc}" -O2 -Wall -Wextra -Werror -o "$tree/build/tools/verify/asciiscan" \
    "$repo/tools/verify/asciiscan.c"

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
for f in vendor/busybox/busybox-9.9/v.sh vendor/musl/install/w.sh ports/doom/src/u.sh \
    krffs/vendor/t.sh; do
    printf '#!/bin/sh\necho vendored\n' >"$tree/$f"
done
for f in tests/run utils/tool tests/registry krffs/manifests/x.manifest \
    tools/verify/cppcheck.suppress rach2/sys/arch/i686/loader.s; do
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

pass()
{
    echo "style-scope-test: result: pass: $1"
}

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
}

# Change one private checker by the named fault.
change()
{
    python3 - "$tree/tools/verify" "$1" <<'PY'
import pathlib
import sys

directory, fault = pathlib.Path(sys.argv[1]), sys.argv[2]
faults = {
    'list-extensionless': ('scripts.sh',
        " -o ! -name '*.*' \\) -print", " \\) -print"),
    'list-script': ('scripts.sh',
        " -o -name '*.script' -o", " -o"),
    'list-vendored': ('scripts.sh',
        "    -path 'vendor/busybox/busybox-*' -prune -o \\\n", ""),
    'text-ascii': ('textcheck.sh',
        "py_files; config_files; } |", "py_files; } |"),
    'text-config': ('textcheck.sh',
        "        grep -n ' -- ' \"$f\" \\\n", "        grep -n ' --- ' \"$f\" \\\n"),
    'text-diary': ('textcheck.sh',
        "        grep -nE '\\b(Phase|Part) [0-9]' \"$f\" \\\n",
        "        grep -nE '\\b(Phase|Part) [0-9]x' \"$f\" \\\n"),
    'text-code': ('textcheck.sh',
        "    { ch_files; other_code_files; } | sort -u | while IFS= read -r f; do\n"
        "        [ -f \"$f\" ] || continue\n"
        "        sed 's/\"[^\"]*\"//g' \"$f\" | grep -n ' -- '",
        "    ch_files | sort -u | while IFS= read -r f; do\n"
        "        [ -f \"$f\" ] || continue\n"
        "        sed 's/\"[^\"]*\"//g' \"$f\" | grep -n ' -- '"),
}
name, old, new = faults[fault]
path = directory / name
text = path.read_text()
if text.count(old) != 1:
    raise SystemExit("style-scope-test: pattern is not unique: " + fault)
path.write_text(text.replace(old, new))
PY
}

rows
[ -z "$failed" ] || exit 1
echo "style-scope-test: result: pass: every row"
[ "$mutate" -eq 1 ] || exit 0

for fault in list-extensionless list-script list-vendored text-ascii text-config \
    text-diary text-code; do
    case $fault in
        list-*) copy=scripts.sh; named=scripts-list ;;
        text-ascii) copy=textcheck.sh; named=text-config-ascii ;;
        text-config) copy=textcheck.sh; named=text-config-dash ;;
        text-diary) copy=textcheck.sh; named=text-config-diary ;;
        text-code) copy=textcheck.sh; named=text-code-dash ;;
    esac
    change "$fault"
    touch "$tree/tools/verify/$copy"
    rows >"$work/$fault.log"
    case " $failed " in
        *" $named "*) echo "style-scope-test: result: $fault: the fault fails $named" ;;
        *)
            cat "$work/$fault.log"
            echo "style-scope-test: result: fail: the $fault fault passed $named" >&2
            exit 1 ;;
    esac
    cp "$tree/saved/$copy" "$tree/tools/verify/$copy"
    touch "$tree/tools/verify/$copy"
    rows >"$work/$fault.restored.log"
    [ -z "$failed" ] || { cat "$work/$fault.restored.log"; exit 1; }
    echo "style-scope-test: result: $fault: restored, every row passes"
done
echo "style-scope-test: result: pass: seven faults failed their rows; restored rows passed"
