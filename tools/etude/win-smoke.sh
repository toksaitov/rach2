#!/bin/sh
# win-smoke.sh: exercise the native collection on prepared guest artifacts.
# Usage: sh tools/etude/win-smoke.sh TREE [ARCH]
# TREE is a built tree visible to Windows; its files are only read. The
# private preparation fixture records copies of its kernel, image and probe.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
input=$(CDPATH='' cd -- "$1" && pwd)
arch=${2:-i686}
case $arch in
    i686) ab=build ;;
    x86_64) ab=build/x86_64 ;;
    *) echo 'win-smoke: architecture must be i686 or x86_64' >&2; exit 2 ;;
esac
case $(uname -s) in
    MINGW*|MSYS*|UCRT*|CLANG*) ;;
    *) echo 'win-smoke: run in MSYS2 on Windows' >&2; exit 2 ;;
esac
work=
status=1
finished=
trap 'status=$?; [ -n "$finished" ] || [ "$status" -ne 0 ] || status=1; if [ "$status" -eq 0 ]; then [ -z "$work" ] || rm -rf "$work"; else echo "win-smoke: evidence: $work" >&2; fi; exit "$status"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-win-smoke.XXXXXX")
cd "$repo"
# The cache builds the production set driver and its validators with MinGW.
sh tools/etude/version.sh -l -C "$work/cache" >"$work/version.log"
cat "$work/version.log"
bin=$(sed -n 's/^etude-version: result: [a-z]* [0-9a-f]* //p' "$work/version.log")
bin=$bin/bin
[ -d "$bin" ] || { echo 'win-smoke: no cached version' >&2; exit 1; }
mkdir -p "$work/tree/$ab/rach2" "$work/tree/$ab/images" "$work/tree/$ab/tests" \
    "$work/tree/$ab/tools/etude/guest" "$work/tree/build/tests/cases/machine" "$work/tree/build/tests/host"
for path in rach2/kernel images/test.krffs tests/errnoprobe tools/etude/guest/etude-guest; do
    cp "$input/$ab/$path" "$work/tree/$ab/$path"
done
# Native scenario and host test, both with the same sources as the collection.
cat >"$work/compile.mk" <<'MAKE'
include tools/etude/sources.mk
all:
	"$(CC)" -std=c99 -O2 -Wall -Wextra -Werror -static -D_WIN32_WINNT=0x0A00 -Itools/etude/include -Itools/etude -Iutils -o "$(SCENARIO)" tests/cases/machine/default_scenario.c tests/cases/machine/lifecycle.c $(ETUDE_CASE_LIB_WIN_SRC) -lws2_32 -lbcrypt
	"$(CC)" -std=c99 -O2 -Wall -Wextra -Werror -static -Iutils -o "$(HOST)" tests/host/sha256_hosttest.c utils/sha256.c
	"$(CC)" -std=c99 -O2 -Wall -Wextra -Werror -static -o "$(PE)" tools/etude/win/pecheck.c
MAKE
make -f "$work/compile.mk" CC="${ETUDE_WIN_CC:-/ucrt64/bin/gcc}" \
    SCENARIO="$work/scenario.exe" HOST="$work/sha256.exe" PE="$work/pecheck.exe"
"$work/pecheck.exe" "$bin/etude-legacy.exe" "$bin/casecheck.exe" "$bin/history_lines.exe" \
    "$work/scenario.exe" "$work/sha256.exe"
cp "$work/scenario.exe" "$work/tree/build/tests/cases/machine/default_scenario"
cp "$work/sha256.exe" "$work/tree/build/tests/host/sha256_hosttest"
# A private site exercises the ordinary build entry and native admission.
mkdir -p "$work/site/ledger" "$work/site/locks" "$work/site/etude-bin"
printf 'etude-ledger 1\n' >"$work/site/ledger/claims"
cp tools/etude/launch.sh "$work/site/etude-launch"
cp tools/etude/make.sh "$work/site/etude-bin/make"
chmod +x "$work/site/etude-launch" "$work/site/etude-bin/make"
cat >"$work/site/policy" <<SITE
etude-site 1
endpoint windows
launcher $work/site/etude-launch
ledger $work/site/ledger
locks $work/site/locks
bridge $bin/etude-bridge.exe
capacity 4000,8589934592,42949672960
wait 60
seconds 600
SITE
ETUDE_SITE=$work/site/policy JOBS=2 make -j2 build/tools/etude/etude-host \
    build/tools/etude/etude-legacy build/tools/etude/etude-hold \
    build/tests/cases/machine/default_scenario
[ "$(cat "$work/site/ledger/claims")" = 'etude-ledger 1' ] || {
    echo 'win-smoke: build left a claim' >&2; exit 1;
}
"$work/pecheck.exe" build/tools/etude/etude-host build/tools/etude/etude-legacy \
    build/tests/cases/machine/default_scenario
mkdir -p "$work/host-logs" "$work/host-scratch"
printf 'sha256\t60\tshared\tSHA-256 vectors\t\t%s\n' \
    "\"$(cygpath -m "$work/sha256.exe")\"" >"$work/host-selection"
MSYS2_ARG_CONV_EXCL='*' build/tools/etude/etude-host -j 1 \
    -l "$(cygpath -m "$work/host-logs")" -s "$(cygpath -m "$work/host-scratch")" \
    "$(cygpath -m "$work/host-selection")"
# The host example's eight assertions are the eight vectors of the host test.
python3 - "$repo" "$work/tree" <<'PY'
import json
import pathlib
import sys
repo, tree = map(pathlib.Path, sys.argv[1:])
host = json.loads((repo / 'tools/etude/schemas/case.host.example.json').read_text())
case = host['cases'][0]
case['execution']['program'] = 'sha256-report'
case['prepare'].append({'name': 'sha256-report', 'node': 'tests/host/sha256-report', 'per_architecture': False})
script = '#!/bin/sh\nset -eu\n"$ETUDE_TREE/build/tests/host/sha256_hosttest"\n'
script += "printf '%s\\n' '@etude 1 begin host.sha256.vectors' "
script += ' '.join("'@etude 1 assert %s pass'" % a['assertion_id'] for a in case['assertions'])
script += " '@etude 1 end 8'\n"
(tree / 'build/tests/host/sha256-report').write_text(script)
(tree / 'host.json').write_text(json.dumps(host))
family = json.loads((repo / 'tests/cases/guest.default.json').read_text())
family['cases'] = [c for c in family['cases'] if c['case_id'] in ('guest.default.errnoprobe', 'guest.default.timing')]
(tree / 'default.json').write_text(json.dumps(family))
(tree / 'selection').write_text('sha256 cases=host.json\ndefault cases=default.json\n')
(tree / 'map').write_text('sha256 host.sha256\ndefault guest.default\n')
(tree / 'Makefile').write_text('.PHONY: components\ncomponents:\n\t@:\n')
PY
chmod +x "$work/tree/build/tests/host/sha256-report"
# All C entry points receive Windows paths. Supporting sh and make are MSYS2's.
tree=$(cygpath -m "$work/tree")
legacy=$(cygpath -m "$bin/etude-legacy.exe")
schemas=$(cygpath -m "$repo/tools/etude/schemas")
casecheck=$(cygpath -m "$bin/casecheck.exe")
mkdir -p "$work/tree/locks"
export RACH2_QEMU_ACCEL="${ETUDE_WIN_ACCEL:-whpx}"
MSYS2_ARG_CONV_EXCL='*' "$bin/etude-hold.exe" "$tree" win-smoke "$legacy" set \
    -t "$tree" -a "$arch" -l "$tree/ledger" -d "$tree/locks" -c 4000,2147483648,1073741824 \
    -o "$tree/results" -e "$tree/evidence" -S "$schemas" -K "$casecheck" -j 2 "$tree/selection"
"$bin/history_check.exe" "$schemas/history.schema.json" "$work/tree/results/history/"*.jsonl
"$bin/history_lines.exe" -s "$schemas" "$work/tree/map" "$work/tree/results/history/"*.jsonl
printf 'win-smoke: result: passed: %s host, guest and machine cases\n' "$arch"
finished=1
