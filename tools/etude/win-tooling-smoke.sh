#!/bin/sh
# win-tooling-smoke.sh: prepare real guest disks and check native readback.
# Run in an admitted MSYS2 shell, with QEMU on PATH and mtools and the cross
# compilers absent. The optional argument is i686 or x86_64.
set -eu
unset MSYS_NO_PATHCONV MSYS2_ARG_CONV_EXCL
REPO=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
arch=${1:-i686}
case $arch in i686|x86_64) ;; *) exit 2 ;; esac
case $(uname -s) in MINGW*|MSYS*|UCRT*|CLANG*) ;; *) exit 2 ;; esac
for tool in mformat mcopy mtype i686-elf-gcc x86_64-elf-gcc mandoc; do
    if command -v "$tool" >/dev/null 2>&1; then
        echo "win-tooling-smoke: excluded tool is present: $tool" >&2
        exit 2
    fi
done
cd "$REPO"
. "$REPO/tools/etude/hold.sh"
etude_hold "$REPO" win-tooling-smoke "$REPO/tools/etude/win-tooling-smoke.sh" "$@"
work=
finished=
status=1
trap 'status=$?; if [ -n "$finished" ] && [ "$status" -eq 0 ]; then [ -z "$work" ] || rm -rf "$work"; else echo "win-tooling-smoke: evidence: $work" >&2; fi; exit "$status"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p build/tests
work=$(mktemp -d "$REPO/build/tests/rach2-win-tooling.XXXXXX")
make build/tools/etude/etude-legacy build/tools/etude/casecheck
. "$REPO/tools/etude/collection-windows.sh"
collection_windows
mkdir "$work/ledger" "$work/locks" "$work/results" "$work/evidence"
python3 "$REPO/tests/system/case_select.py" -c 'machine.parts.tables host.style.manual' \
    "$arch" "$REPO/tests/cases" "$work/chosen" parts style
printf 'parts cases=%s\nstyle cases=%s\n' "$(cygpath -m "$work/chosen/parts.json")" \
    "$(cygpath -m "$work/chosen/style.json")" > "$work/selection"
RACH2_QEMU_ACCEL=whpx
MSYS2_ARG_CONV_EXCL='*'
export RACH2_QEMU_ACCEL MSYS2_ARG_CONV_EXCL
"$(cygpath -m "$REPO/build/tools/etude/etude-legacy")" set -a "$arch" -j 1 -s 7200 \
    -t "$(cygpath -m "$REPO")" -c 4000,8589934592,171798691840 \
    -l "$(cygpath -m "$work/ledger")" -d "$(cygpath -m "$work/locks")" \
    -o "$(cygpath -m "$work/results")" -e "$(cygpath -m "$work/evidence")" \
    -S "$(cygpath -m "$REPO/tools/etude/schemas")" \
    -K "$(cygpath -m "$REPO/build/tools/etude/casecheck")" "$(cygpath -m "$work/selection")"
finished=1
echo 'win-tooling-smoke: result: passed: guest preparation, disk readback and manual analysis'
