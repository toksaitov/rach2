#!/usr/bin/env sh
#
# dbcheck.sh: check that every in-house C source is compiled by a build or a
# test.
#
# tools/dev/gen_compile_commands.sh writes one database per unit under
# build/compile_commands/. A source none of them names is one no build
# compiles, or one the generator missed; either is a fault, so this fails
# naming it. The sources are the in-house trees: the kernel, the KRFFS tools,
# the utilities, the shell, the tests with their host and musl programs, the
# tools, and Doom's platform layer. The vendored trees are not in-house.
#
# Two kinds of source are compiled without an entry of their own. A fragment
# that another source includes by name, as `ish/ish_syscalls.c` includes
# `ish_syscalls.i386.c`, is compiled as part of that source's entry, and so is
# one that a source of a database includes by a path relative to itself, as
# `tests/host/etude_route_hosttest.c` includes
# `../../tools/ci/fixtures/routine_route_actions_test.c`. A
# fixture under tests/system/fixtures or tests/host/fixtures is compiled by
# the test script that names it, with the flags the script sets, or by a rule
# of mk/host-fixtures.mk that preparation runs, so no fixed command describes
# it; such a source passes when a script under that same tree names it or a
# rule of that file compiles it.
#
# A Windows program's source under tools/etude/win is compiled by
# tools/etude/wincheck.sh with MinGW-w64, which the host's databases cannot
# describe; such a source passes when that script names it, or when
# tools/etude/sources.mk lists it for the Windows etude that the script
# builds. The release's Job Object helper, tools/flow/winjob.c, is compiled
# on Windows with MSYS2's gcc by the lines that tools/flow/winjob.sh prints;
# it passes when that script names it.
# A program that runs on another machine is compiled there, by the script
# that runs it or by hand as the Readme beside it says, against that
# machine's libraries: MinGW's in the Debian guest, Weston's, FFmpeg's, X11's
# and Vulkan's on Linux, the GNU root's glibc on Rach2. The host has none of
# them and no build compiles the program, so no command of the host describes
# it; such a source passes when the script or Readme named for its directory
# below compiles it.
#
# The Windows scenario actor uses the native host preparation adapter.
# The native mandoc build copies its entry and mapping adapter to
# batch_main.c and batch_mmap.c, compiled by tools/setup/mandoc-batch.mk;
# these two sources have no POSIX translation-unit command.
#
# Usage: dbcheck.sh [DBDIR]
set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
DBDIR="${1:-$REPO/build/compile_commands}"
cd "$REPO" || exit 1
if ! ls "$DBDIR"/*.json >/dev/null 2>&1; then
    echo "dbcheck: no database under $DBDIR; run tools/dev/gen_compile_commands.sh" >&2
    exit 1
fi
# The absolute path of every entry: its directory joined with its file, a
# file that is already absolute taken as it is. The generator writes one
# entry per line, and this reads that shape.
known=
fragments=
trap 'rm -f ${known:+"$known"} ${fragments:+"$fragments"}' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
known=$(mktemp) || exit 2
cat "$DBDIR"/*.json | LC_ALL=C awk -F'"' '
    /"directory"/ {
        for (i = 1; i <= NF; i++) {
            if ($i == "directory") d = $(i + 2)
            if ($i == "file") f = $(i + 2)
        }
        if (f ~ /^\//) print f; else print d "/" f
    }' | python3 -c 'from pathlib import Path; import sys
for line in sys.stdin:
    print(Path(line.rstrip("\n")).resolve())' | sort -u >"$known"
# The absolute path of every C file that a source of a database includes by a
# path relative to the source's own directory.
fragments=$(mktemp) || exit 2
python3 -c 'import re, sys
from pathlib import Path
for line in open(sys.argv[1]):
    source = Path(line.rstrip("\n"))
    try:
        text = source.read_text(errors="replace")
    except OSError:
        continue
    for name in re.findall(r"^#include \"([^\"]*/[^\"]*\.c)\"", text, re.M):
        print((source.parent / name).resolve())' "$known" | sort -u >"$fragments"
sources=$( {
    find rach2 -name '*.c'
    find krffs utils ish tests -path '*/vendor' -prune -o -path '*/vs-dokany' -prune -o -name '*.c' -print
    find tools -name '*.c'
    find ports/doom -maxdepth 1 -name '*.c'
} 2>/dev/null | sort -u )
# included FILE: true when a C file in FILE's directory includes FILE by name,
# or a source of a database includes it by a relative path.
included() {
    grep -rqF --include='*.c' "#include \"${1##*/}\"" "$(dirname "$1")" && return 0
    grep -qxF "$REPO/$1" "$fragments" && return 0
    case $1 in
        tests/host/compat/*)
            grep -qF "#include \"compat/${1##*/}\"" tests/host/*.c ;;
        *) return 1 ;;
    esac
}
# fixture FILE: true when FILE is under tests/system/fixtures or
# tests/host/fixtures and a script under tests/system or tests/host names it,
# or a rule of mk/host-fixtures.mk compiles it.
fixture() {
    case $1 in
        tests/system/fixtures/*) tree=tests/system ;;
        tests/host/fixtures/*) tree=tests/host ;;
        *) return 1 ;;
    esac
    grep -rqF --include='*.sh' "fixtures/${1##*/}" "$tree" ||
        awk -v f="$1" '$1 == "$(HOSTCC)" { for (i = 2; i <= NF; i++) if ($i == f) found = 1 }
            END { exit !found }' mk/host-fixtures.mk
}
# windows FILE: true for the exact Windows-only build routes described above.
windows() {
    case $1 in
        tools/etude/win/*) grep -qF "${1##*/}" tools/etude/wincheck.sh ||
            grep -qF "$1" tools/etude/sources.mk ;;
        tools/flow/winjob.c)
            # shellcheck disable=SC2016 # the script's own words, unexpanded
            grep -qF 'src=$(dirname "$0")/winjob.c' tools/flow/winjob.sh ;;
        tests/host/compat/scenario_guest.c)
            grep -qF "\"$1\"" tools/etude/host_prepare.c ;;
        tools/setup/mandoc-win/main.c|tools/setup/mandoc-win/mmap.c)
            mapped=batch_${1##*/}
            grep -qF "compat=\$TREE/tools/setup/mandoc-win" tools/setup/build_host_tool.sh &&
            grep -qF "cp \"\$compat/${1##*/}\" \"\$m/$mapped\"" tools/setup/build_host_tool.sh &&
            grep -qF "${mapped%.c}.o" tools/setup/mandoc-batch.mk ;;
        *) return 1 ;;
    esac
}
# guest FILE: true for the programs compiled on the machines they run on, as
# described above, each by the script that compiles it.
guest() {
    base=${1##*/}
    case $1 in
        tests/graphics/browser/*.c)
            grep -qF "/$base" tests/graphics/browser/seat-build.sh ;;
        tests/graphics/d3d/d3d*probe.c)
            n=${base#d3d}
            n=${n%probe.c}
            # shellcheck disable=SC2016 # the script's own words, unexpanded
            grep -qF 'src/d3d${n}probe.c' tests/graphics/d3d/build.sh &&
            grep -Eq "for n in ([0-9]+ )*$n( |;|\$)" tests/graphics/d3d/build.sh ;;
        tests/system/games/*.c)
            grep -qF " $base" tests/system/games/build.sh ;;
        tests/graphics/runners/*.c)
            grep -qF "\"\$RUNNER_DIR/$base\"" tests/graphics/runners/*.sh ;;
        tests/graphics/lenovo/vahash.c)
            # Built by hand on Debian, as the graphics Readme writes it.
            grep -qE "\`$base\` built with \`[^\`]* gcc [^\`]* lenovo/$base " tests/graphics/Readme.md ;;
        tests/graphics/lenovo/*.c)
            grep -qF "$base" tests/graphics/lenovo/*.sh tests/graphics/lenovo/*.py ;;
        tests/kvm/nocopy.c)
            grep -qF "$base" tests/kvm/bed-shm.sh ;;
        tools/lenovo/debian-ref/*.c)
            grep -qF "\`$base\` built with" tools/lenovo/debian-ref/Readme.md ;;
        *) return 1 ;;
    esac
}
missing=0
for f in $sources; do
    grep -qxF "$REPO/$f" "$known" && continue
    included "$f" && continue
    fixture "$f" && continue
    windows "$f" && continue
    guest "$f" && continue
    echo "dbcheck: $f is in no compilation database"
    missing=$((missing + 1))
done
if [ "$missing" -gt 0 ]; then
    echo "dbcheck: $missing source(s) in no database" >&2
    exit 1
fi
echo "dbcheck: ok: $(echo "$sources" | wc -l | tr -d ' ') sources, each compiled by a build or a test"
