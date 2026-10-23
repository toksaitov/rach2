#!/bin/sh
# style_cases_test.sh: check that a deliberate fault stays fatal in the style
# cases, which etude judges by `etude check attempt`. A private tree with a
# fault in its sources must fail the conventions case, and a manifest with a
# fault must fail the judgment of a report that passes, or be refused by
# `etude check families`, which tests/run reads before any suite runs. The
# case program is tests/cases/host/style.sh and its manifest
# tests/cases/host.style.json; the build's etude and asciiscan must be built.
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
STYLE=$REPO/tests/cases/host/style.sh
ETUDE=$REPO/build/tools/etude/etude
ASCIISCAN=$REPO/build/tools/verify/asciiscan
SCHEMAS=$REPO/tools/etude/schemas
MANIFEST=$REPO/tests/cases/host.style.json
CASE=host.style.conventions
WORK=
trap 'rm -rf ${WORK:+"$WORK"}' 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rach2-style-cases-test.XXXXXX")
# Report $* and exit 1.
fail() { echo "style-cases-test: $*" >&2; exit 1; }
for f in "$ETUDE" "$ASCIISCAN"; do
    [ -x "$f" ] || fail "$f is not built"
done

# A tree with no fault: one C file, one guarded header and one script that
# keeps its flag, with the style program's file list and checker.
TREE=$WORK/tree
for d in rach2 krffs utils ish tests tools/verify mk man ports vendor build/tools/verify; do
    mkdir -p "$TREE/$d"
done
: > "$TREE/tests/registry"
cp "$REPO/tools/verify/scripts.sh" "$TREE/tools/verify/"
cp "$ASCIISCAN" "$TREE/build/tools/verify/"
printf '/* clean.c: a file with no fault. */\nint clean;\n' > "$TREE/rach2/clean.c"
printf '#ifndef CLEAN_H\n#define CLEAN_H\nextern int clean;\n#endif\n' > "$TREE/rach2/clean.h"
cat > "$TREE/tools/flagged.sh" <<'EOF'
#!/bin/sh
set -eu
finished=
trap '[ -n "$finished" ] || exit 1' EXIT
finished=1
EOF

# conventions LABEL: run the conventions case in the tree, with its report
# in WORK/LABEL.out and its exit status in WORK/LABEL.status.
conventions() {
    status=0
    (cd "$TREE" && ETUDE_CASE=$CASE TMPDIR=$WORK sh "$STYLE" conventions) \
        > "$WORK/$1.out" 2>&1 || status=$?
    echo "$status" > "$WORK/$1.status"
}
# judge LABEL MANIFEST: judge the report of LABEL by MANIFEST, with the
# document in WORK/LABEL.json, and print the exit status of `etude check`.
judge() {
    status=0
    "$ETUDE" check -s "$SCHEMAS" attempt "$2" "$CASE" none host "$WORK/$1.out" \
        run-1 attempt-1 item-1 exited "$(cat "$WORK/$1.status")" \
        > "$WORK/$1.json" 2>&1 || status=$?
    echo "$status"
}
# expect LABEL TEXT: the document of the judgment of LABEL has TEXT.
expect() {
    grep -qF "$2" "$WORK/$1.json" || {
        sed 's/^/    /' "$WORK/$1.out" "$WORK/$1.json" >&2
        fail "$1: the judgment does not have $2"
    }
}

conventions clean
[ "$(cat "$WORK/clean.status")" = 0 ] || { cat "$WORK/clean.out" >&2; fail "the case failed on a tree with no fault"; }
[ "$(judge clean "$MANIFEST")" = 0 ] || { cat "$WORK/clean.json" >&2; fail "the judgment of a clean report did not pass"; }

# Each fault in the sources fails its assertion and the case.
# source_fault NAME ASSERTION: run the case on the tree as it is now and
# require it, and its judgment, to fail ASSERTION.
source_fault() {
    conventions "$1"
    [ "$(cat "$WORK/$1.status")" = 1 ] || { cat "$WORK/$1.out" >&2; fail "$1: the case did not fail"; }
    [ "$(judge "$1" "$MANIFEST")" = 1 ] || { cat "$WORK/$1.json" >&2; fail "$1: the judgment did not fail"; }
    expect "$1" "{\"assertion_id\":\"$2\",\"source\":\"program\",\"outcome\":\"fail\""
}
printf 'int line; // a line comment\n' >> "$TREE/rach2/clean.c"
source_fault comment no-line-comments
# A failure's detail is cut at 160 bytes, and here the cut falls just after a
# space, which the record must not end with: "a // comment: rach2/clean.c:3:"
# and the line's first 130 bytes, the last of them a space.
printf '/* clean.c: a file with no fault. */\nint clean;\n' > "$TREE/rach2/clean.c"
printf 'int line; // %s tail\n' "$(printf '%0116d' 0 | tr 0 x)" >> "$TREE/rach2/clean.c"
source_fault cut no-line-comments
printf '/* clean.c: a file with no fault. */\nint clean;\n' > "$TREE/rach2/clean.c"
printf 'extern int unguarded;\n' > "$TREE/rach2/unguarded.h"
source_fault guard header-guards
rm "$TREE/rach2/unguarded.h"
printf '#!/bin/sh\nset -eu\ntrap "rm -f x" EXIT\n' > "$TREE/tools/unflagged.sh"
source_fault flag exit-flag
rm "$TREE/tools/unflagged.sh"

# Each fault in the manifest fails the judgment of the clean report: an
# assertion the program reports and the case does not declare, and one the
# case declares and the program never reports.
# edit NAME PYTHON: write WORK/NAME.manifest.json, the manifest m with its
# conventions case C changed by PYTHON.
edit() {
    python3 -I -c '
import json, sys
m = json.load(open(sys.argv[1]))
C = [c for c in m["cases"] if c["case_id"] == sys.argv[3]][0]
exec(sys.argv[4])
json.dump(m, open(sys.argv[2], "w"), indent=1)
' "$MANIFEST" "$WORK/$1.manifest.json" "$CASE" "$2"
}
# manifest_fault NAME TEXT: judge the clean report by WORK/NAME.manifest.json
# and require the judgment to fail with TEXT in its document.
manifest_fault() {
    cp "$WORK/clean.out" "$WORK/$1.out"
    cp "$WORK/clean.status" "$WORK/$1.status"
    [ "$(judge "$1" "$WORK/$1.manifest.json")" = 1 ] || { cat "$WORK/$1.json" >&2; fail "$1: the judgment did not fail"; }
    expect "$1" "$2"
}
edit undeclared 'C["assertions"] = [a for a in C["assertions"] if a["assertion_id"] != "exit-flag"]'
manifest_fault undeclared '"undeclared":[{"assertion_id":"exit-flag","outcome":"pass"'
edit unreported 'C["assertions"].append({"assertion_id": "extra", "checks": "nothing", "source": "program"})'
manifest_fault unreported '{"assertion_id":"extra","source":"program","outcome":"missing"'

# A manifest that its rules refuse refuses every run: `etude check families`
# prints no family for it, and tests/run stops on its status.
"$ETUDE" check -s "$SCHEMAS" families "$MANIFEST" > "$WORK/families" ||
    fail "etude check families refused $MANIFEST"
grep -qx "host.style $MANIFEST" "$WORK/families" || fail "etude check families did not print host.style"
edit duplicate 'm["cases"].append(dict(C))'
status=0
"$ETUDE" check -s "$SCHEMAS" families "$WORK/duplicate.manifest.json" > "$WORK/families" 2> "$WORK/families.err" || status=$?
[ "$status" = 1 ] || fail "etude check families exited $status on a manifest with a case twice"
[ ! -s "$WORK/families" ] || fail "etude check families printed a family of a manifest at fault"
echo "style-cases-test: passed: four source faults and three manifest faults are fatal"
