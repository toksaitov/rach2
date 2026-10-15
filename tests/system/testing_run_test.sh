#!/bin/sh
# testing_run_test.sh: check that `testing.sh run` delivers its command intact.
# --mutate pastes the command into a quoted argument of a private copy;
# --texts checks that fault's text.

set -eu
case ${1:-} in
    '') mode=check ;;
    --mutate) mode=mutate ;;
    --texts) mode=texts ;;
    *) echo 'testing-run-test: usage: testing_run_test.sh [--mutate | --texts]' >&2; exit 2 ;;
esac
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/testing-run-test.XXXXXX")
trap 'rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$work/bin" "$work/home" "$work/copy" "$work/tree/tools/dev"
script=$work/tree/tools/dev/testing.sh
cp "$repo/tools/dev/testing.sh" "$script"

# fault: paste the command into the single-quoted argument of the private copy.
fault()
{
    cat > "$work/fault.sed" <<'EOF'
s/eval .* bash \$encoded"/$*'"/
EOF
    sed -f "$work/fault.sed" "$repo/tools/dev/testing.sh" > "$script"
    ! cmp -s "$repo/tools/dev/testing.sh" "$script"
}
if [ "$mode" = texts ]; then
    fault
    exit 0
fi

# The ssh fixture stands for the host's sshd and MSYS shell: it answers the
# holder's script at once and keeps the lock until its input ends, and gives
# any other command line to a shell to parse, as sshd gives it to MSYS's.
cat > "$work/bin/ssh" <<'EOF'
#!/bin/sh
[ "$1 $3" = '-o testing' ] || exit 255
case $4 in
    *'bash -s -- '*) echo held; exec cat > /dev/null ;;
esac
exec bash -c "$4"
EOF
cat > "$work/bin/wsl.exe" <<'EOF'
#!/bin/sh
[ "$1" = -e ] || { echo "wsl.exe fixture: unsupported: $*" >&2; exit 2; }
shift
exec "$@"
EOF
cat > "$work/bin/rsync" <<'EOF'
#!/bin/sh
[ "$1" != --version ] || echo 'rsync  version 3.2.7  protocol version 31'
EOF
chmod +x "$work/bin/ssh" "$work/bin/wsl.exe" "$work/bin/rsync"
HOME=$work/home
PATH=$work/bin:$PATH
RACH2_TESTING_COPY=$work/copy
export HOME PATH RACH2_TESTING_COPY

# row NAME STATUS EXPECTED INPUT COMMAND...: run COMMAND with INPUT as its
# standard input, and require exit status STATUS and output EXPECTED.
row()
{
    name=$1 status=$2 expected=$3 input=$4
    shift 4
    result=0
    printf '%s' "$input" | sh "$script" run "$@" > "$work/out" 2> "$work/err" ||
        result=$?
    if [ "$result" -ne "$status" ] || [ "$(cat "$work/out")" != "$expected" ]; then
        echo "testing-run-test: failed: $name: status $result" >&2
        awk '{ print "testing-run-test:   " $0 }' "$work/out" "$work/err" >&2
        return 1
    fi
}

# rows: run each command and report every row that fails.
rows()
{
    failed=0
    row words 0 'a b' '' echo a b || failed=1
    row single 0 ok '' "printf '%s\\n' ok" || failed=1
    row quotes 0 "it's
say \"hi\"
x
a\\b" '' "printf '%s\\n' \"it's\" 'say \"hi\"' '(x)' 'a\\b' | tr -d '()'" ||
        failed=1
    # shellcheck disable=SC2016 # The host's shell expands these.
    row shell 0 "tcg $work/copy" '' 'echo "$RACH2_QEMU_ACCEL $(pwd)"' || failed=1
    # shellcheck disable=SC2016
    row input 0 'got line' 'line
' 'read -r v; echo "got $v"' || failed=1
    row status 7 '' '' '(exit 7)' || failed=1
    return "$failed"
}
rows || exit 1
if [ "$mode" = mutate ]; then
    fault
    if rows > "$work/mutated.out" 2>&1; then
        echo 'testing-run-test: fault passed: command pasted into the argument' >&2
        exit 1
    fi
    grep -q '^testing-run-test: failed: quotes:' "$work/mutated.out"
    if grep -q '^testing-run-test: failed: words:' "$work/mutated.out"; then
        echo 'testing-run-test: fault failed a command without quotes' >&2
        exit 1
    fi
    cp "$repo/tools/dev/testing.sh" "$script"
    rows || exit 1
    echo 'testing-run-test: result: pass: pasted command caught and restored'
fi
echo 'testing-run-test: result: pass: 6 rows'
