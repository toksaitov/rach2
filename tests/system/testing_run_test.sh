#!/bin/sh
# testing_run_test.sh: check that `testing.sh run` delivers its command intact,
# that a copy synced from a git worktree is a repository of its own, and that
# `testing.sh wait` reports a run as it is on the host.

set -eu
[ "$#" -eq 0 ] || { echo 'testing-run-test: usage: testing_run_test.sh' >&2; exit 2; }
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
trap 'rm -rf ${work:+"$work"}' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-testing-run-test.XXXXXX")
mkdir -p "$work/bin" "$work/home" "$work/copy" "$work/tree/tools/dev"
script=$work/tree/tools/dev/testing.sh
cp "$repo/tools/dev/testing.sh" "$script"

# The ssh fixture stands for the host's sshd and its default shell, a batch
# file that starts MSYS's bash: it answers the holder's script at once and
# keeps the lock until its input ends. It refuses any other command line that
# is longer than cmd.exe's limit of 8191 characters or has a double quote, `%`
# or `!`, which cmd.exe reads as its own, and gives the rest to a shell to
# parse, as the batch file gives it to MSYS's. The terminal of `testing.sh sh`
# runs its command with its input, in which the host's `/home/testing/` is the
# directory HOSTHOME names, or exits 255 when the file `unreachable` is beside
# that directory.
cat > "$work/bin/ssh" <<'EOF'
#!/bin/sh
if [ "$1 $2" = '-t testing' ]; then
    [ ! -e "$HOSTHOME/../unreachable" ] || exit 255
    sed "s|/home/testing/|$HOSTHOME/|g" | bash -c "$3"
    exit
fi
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
case $1 in
    -e|--) ;;
    *) echo "wsl.exe fixture: unsupported: $*" >&2; exit 2 ;;
esac
shift
exec "$@"
EOF
# The rsync fixture copies to the local path that stands for the host's,
# with this machine's rsync.
real_rsync=$(command -v rsync)
{
    echo '#!/bin/sh'
    echo "real_rsync='$real_rsync'"
    cat <<'EOF'
[ "$1" != --version ] || { echo 'rsync  version 3.2.7  protocol version 31'; exit 0; }
skip=
for a do
    shift
    if [ -n "$skip" ]; then
        skip=
        continue
    fi
    case $a in
        --rsync-path=*) ;;
        -e) skip=1 ;;
        testing:*) set -- "$@" "${a#testing:}" ;;
        *) set -- "$@" "$a" ;;
    esac
done
exec "$real_rsync" "$@"
EOF
} > "$work/bin/rsync"
chmod +x "$work/bin/ssh" "$work/bin/wsl.exe" "$work/bin/rsync"
HOME=$work/home
HOSTHOME=$work/host/home
PATH=$work/bin:$PATH
RACH2_TESTING_COPY=$work/copy
GIT_AUTHOR_NAME=test GIT_AUTHOR_EMAIL=test@example.com
GIT_COMMITTER_NAME=test GIT_COMMITTER_EMAIL=test@example.com
export HOME HOSTHOME PATH RACH2_TESTING_COPY
export GIT_AUTHOR_NAME GIT_AUTHOR_EMAIL GIT_COMMITTER_NAME GIT_COMMITTER_EMAIL
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_COMMON_DIR

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

# worktree: sync from a git worktree into a copy whose `.git` is the file an
# earlier sync left, move the repository away as the host lacks it, and
# require git in the copy to read the worktree's commit, tree and index.
worktree()
{
    rm -rf "$work/repo" "$work/away" "$work/wt" "$work/wtcopy"
    git -c init.defaultBranch=master init -q "$work/repo"
    echo /tools/ > "$work/repo/.gitignore"
    echo one > "$work/repo/first"
    git -C "$work/repo" add .gitignore first
    git -C "$work/repo" commit -qm one
    git -C "$work/repo" worktree add -q -b topic "$work/wt" 2> /dev/null
    echo two > "$work/wt/second"
    git -C "$work/wt" add second
    git -C "$work/wt" commit -qm two
    mkdir -p "$work/wt/tools/dev" "$work/wtcopy"
    cp "$script" "$work/wt/tools/dev/testing.sh"
    cp "$work/wt/.git" "$work/wtcopy/.git"
    expected="$(git -C "$work/wt" rev-parse HEAD)
HEAD:second"
    result=0
    RACH2_TESTING_COPY=$work/wtcopy sh "$work/wt/tools/dev/testing.sh" sync \
        > "$work/out" 2> "$work/err" || result=$?
    mv "$work/repo" "$work/away"
    if [ "$result" -eq 0 ]; then
        (cd "$work/wtcopy" && git rev-parse HEAD && git grep -l two HEAD &&
            git status --porcelain) > "$work/out" 2> "$work/err" || result=$?
    fi
    if [ "$result" -ne 0 ] || [ "$(cat "$work/out")" != "$expected" ]; then
        echo "testing-run-test: failed: worktree: status $result" >&2
        awk '{ print "testing-run-test:   " $0 }' "$work/out" "$work/err" >&2
        return 1
    fi
}

# The run that `wait` watches has a name no other process on the host has in
# its command line. The sleep fixture stands for the minute between the
# passes of `wait`: in it, the run writes its first result line.
run=testingwait$$
mkdir -p "$work/waitbin" "$HOSTHOME"
{
    echo '#!/bin/sh'
    echo "echo '$run: first' >> \"\$HOSTHOME/$run.out\""
} > "$work/waitbin/sleep"
chmod +x "$work/waitbin/sleep"

# waitrow NAME STATUS EXPECTED RUNNING FRESH OUT [SEEN]: set up the run on
# the fixture's host, running when RUNNING is 1, with a file under its work
# directory changed in the last 15 minutes when FRESH is 1, and with the
# result file OUT holds, none when OUT is `-`; then require `testing.sh wait`
# with SEEN to exit with status STATUS and print EXPECTED.
waitrow()
{
    name=$1 status=$2 expected=$3 running=$4 fresh=$5 results=$6
    shift 6
    rm -rf "${HOSTHOME:?}/$run" "$HOSTHOME/$run.out" "$HOSTHOME/$run.sh"
    pid=
    if [ "$running" -eq 1 ]; then
        printf 'while :; do sleep 1; done\n' > "$HOSTHOME/$run.sh"
        sh "$HOSTHOME/$run.sh" &
        pid=$!
    fi
    if [ "$fresh" -eq 1 ]; then
        mkdir "$HOSTHOME/$run"
        : > "$HOSTHOME/$run/log"
    fi
    [ "$results" = - ] || printf '%s\n' "$results" > "$HOSTHOME/$run.out"
    result=0
    PATH=$work/waitbin:$PATH sh "$script" wait "$run" "$@" \
        > "$work/out" 2> "$work/err" || result=$?
    if [ -n "$pid" ]; then
        kill "$pid" 2>/dev/null || :
        wait "$pid" 2>/dev/null || :
    fi
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
    worktree || failed=1
    waitrow 'wait first' 0 "$run: first
testing: $run: running, 1 lines" 1 1 - || failed=1
    waitrow 'wait stalled' 0 "testing: $run: stalled, nothing under\
 /home/testing/$run changed for 15 minutes, 0 lines" 1 0 - || failed=1
    waitrow 'wait ended' 0 "$run: two
testing: $run: ended, 2 lines" 0 0 "$run: one
$run: two" 1 || failed=1
    : > "$work/host/unreachable"
    waitrow 'wait unreachable' 1 '' 1 1 "$run: one" || failed=1
    rm -f "$work/host/unreachable"
    return "$failed"
}
long=$(printf '%9000s' '' | tr ' ' x)
rows || exit 1
echo 'testing-run-test: result: pass: 13 rows'
