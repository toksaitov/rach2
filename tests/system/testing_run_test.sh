#!/bin/sh
# testing_run_test.sh: check that `testing.sh run` delivers its command intact.
# --mutate pastes the command into a quoted argument of a private copy, and
# then decodes it inside double quotes; --texts checks those faults' texts.

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

# fault NAME: write the private copy with fault NAME: `paste` puts the command
# into the single-quoted argument, and `quoted` decodes it inside double
# quotes, which cmd.exe on the host misreads.
fault()
{
    case $1 in
        paste)
            cat > "$work/fault.sed" <<'EOF'
s/read -r c && \. <(base64 .*)'"/$*'"/
EOF
            ;;
        quoted)
            cat > "$work/fault.sed" <<'EOF'
s/\. <(base64 -d <<< \\\$c)/eval \\"\\$(printf %s \\"\\$c\\" | base64 -d)\\"/
EOF
            ;;
    esac
    sed -f "$work/fault.sed" "$repo/tools/dev/testing.sh" > "$script"
    ! cmp -s "$repo/tools/dev/testing.sh" "$script"
}
if [ "$mode" = texts ]; then
    fault paste
    fault quoted
    exit 0
fi

# The ssh fixture stands for the host's sshd and its default shell, a batch
# file that starts MSYS's bash: it answers the holder's script at once and
# keeps the lock until its input ends. It refuses any other command line that
# is longer than cmd.exe's limit of 8191 characters or has a double quote, `%`
# or `!`, which cmd.exe reads as its own, and gives the rest to a shell to
# parse, as the batch file gives it to MSYS's.
cat > "$work/bin/ssh" <<'EOF'
#!/bin/sh
[ "$1 $3" = '-o testing' ] || exit 255
[ "${#4}" -le 8191 ] ||
    { echo "ssh fixture: cmd.exe refuses a line of ${#4} characters" >&2; exit 255; }
case $4 in
    *'bash -s -- '*) echo held; exec cat > /dev/null ;;
    *[\"%!]*) echo "ssh fixture: cmd.exe misreads: $4" >&2; exit 255 ;;
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
    row cmd 0 '%OS% !x! ^' '' "echo '%OS% !x! ^'" || failed=1
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
    row long 0 9000 '' "echo $long | awk '{ print length(\$0) }'" || failed=1
    return "$failed"
}
long=$(printf '%9000s' '' | tr ' ' x)
rows || exit 1
if [ "$mode" = mutate ]; then
    fault paste
    if rows > "$work/mutated.out" 2>&1; then
        echo 'testing-run-test: fault passed: command pasted into the argument' >&2
        exit 1
    fi
    grep -q '^testing-run-test: failed: quotes:' "$work/mutated.out"
    grep -q '^testing-run-test: failed: long:' "$work/mutated.out"
    if grep -q '^testing-run-test: failed: words:' "$work/mutated.out"; then
        echo 'testing-run-test: fault failed a command without quotes' >&2
        exit 1
    fi
    fault quoted
    if rows > "$work/mutated.out" 2>&1; then
        echo 'testing-run-test: fault passed: command decoded inside double quotes' >&2
        exit 1
    fi
    if ! grep -q '^testing-run-test: failed: words:' "$work/mutated.out"; then
        echo 'testing-run-test: fault passed a command without quotes' >&2
        exit 1
    fi
    cp "$repo/tools/dev/testing.sh" "$script"
    rows || exit 1
    echo 'testing-run-test: result: pass: both faults caught and restored'
fi
echo 'testing-run-test: result: pass: 8 rows'
