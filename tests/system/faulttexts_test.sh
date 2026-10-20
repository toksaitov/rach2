#!/bin/sh
# faulttexts_test.sh: check tools/verify/faulttexts.sh on a small tree in five
# cases: fault scripts and a catalogue whose texts are in their sources pass
# with the verdict alone, though tests/system/accept/ has no script, and a
# changed text of a script or of a catalogue, a script without --texts and an
# unbuilt proof driver fail. FAULTTEXTS, when set, is the checker to run in
# place of tools/verify/faulttexts.sh.
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=
trap 'rm -rf ${WORK:+"$WORK"}' 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rach2-faulttexts-test.XXXXXX")
TREE=$WORK/tree
mkdir -p "$TREE/src" "$TREE/tests/system/accept" "$TREE/tests/host" \
    "$TREE/tests/proofs" "$TREE/tools/dev" "$TREE/build/tools/etude"
printf 'alpha text\n' > "$TREE/src/alpha.c"
printf 'one text\n' > "$TREE/src/one.c"
cat > "$TREE/tests/system/alpha_mutate.sh" <<'EOF'
[ "$1" = --texts ] && grep -q -F 'alpha text' src/alpha.c
EOF
# The mode is written apart so that faulttexts.sh does not take this file for
# a fault script.
mode=--mutate
cat > "$TREE/tests/host/beta_test.sh" <<EOF
case \$1 in --texts) exit 0 ;; $mode) exit 1 ;; esac
EOF
cp "$TREE/tests/host/beta_test.sh" "$TREE/tools/dev/poll_kernels.sh"
# The driver stands in for etude-proof: its catalogue is the text to find.
printf 'one text\n' > "$TREE/tests/proofs/one.json"
cat > "$TREE/build/tools/etude/etude-proof" <<'EOF'
#!/bin/sh
[ "$3" = --texts ] && grep -q -F -e "$(cat "$4")" src/one.c
EOF
chmod +x "$TREE/build/tools/etude/etude-proof"

# check STATUS NAME LINE: run faulttexts.sh over the tree and require STATUS,
# and LINE as the whole output for 0 or as one of its lines otherwise.
check() {
    result=0
    FAULTTEXTS_ROOT="$TREE" sh "${FAULTTEXTS:-$REPO/tools/verify/faulttexts.sh}" > "$WORK/log" 2>&1 ||
        result=$?
    if [ "$result" -ne "$1" ]; then
        cat "$WORK/log" >&2
        echo "faulttexts-test: $2: expected $1, got $result" >&2
        exit 1
    fi
    if [ "$1" -eq 0 ] && [ "$(cat "$WORK/log")" != "$3" ]; then
        cat "$WORK/log" >&2
        echo "faulttexts-test: $2: output beside the verdict" >&2
        exit 1
    fi
    if [ "$1" -ne 0 ] && ! grep -q -x -F -e "$3" "$WORK/log"; then
        cat "$WORK/log" >&2
        echo "faulttexts-test: $2: no line: $3" >&2
        exit 1
    fi
}
check 0 'texts in their sources' \
    'faulttexts: result: pass: 4 fault scripts and catalogues, each fault'"'"'s text in its source'
printf 'alpha changed\n' > "$TREE/src/alpha.c"
check 1 'changed script text' \
    'faulttexts: tests/system/alpha_mutate.sh: a fault'"'"'s text is not in its source:'
printf 'alpha text\n' > "$TREE/src/alpha.c"
printf 'one changed\n' > "$TREE/src/one.c"
check 1 'changed catalogue text' \
    'faulttexts: tests/proofs/one.json: a fault'"'"'s text is not once in its source:'
printf 'one text\n' > "$TREE/src/one.c"
printf 'exit 0\n' > "$TREE/tests/system/accept/gamma_mutate.sh"
check 1 'script without --texts' \
    'faulttexts: tests/system/accept/gamma_mutate.sh: no --texts mode'
rm "$TREE/tests/system/accept/gamma_mutate.sh"
rm "$TREE/build/tools/etude/etude-proof"
check 1 'unbuilt driver' \
    'faulttexts: build/tools/etude/etude-proof: not built; make build/tools/etude/etude-proof'
echo "faulttexts-test: passed: 5 cases"
