#!/bin/sh
#
# worker_park_test.sh: worker.sh parks a committed issue while a `hold` is
# open, claims only a repair of core meanwhile, and takes the issue back.
#
# Each row runs the loop itself, from a private copy of tools/flow committed
# to a scratch origin, in a clone of that origin whose branch 50-fix has a
# commit master lacks, with issue 50 in STATE/issue and hold 77 open. Stand-ins
# for gh, board.sh, claude and sleep answer from a file of issues: a session
# writes its issue, the clone's branch and STATE/parked to a list, and closes
# its issue and the hold; each sleep is counted, and the row's last touches
# STATE/stop. The rows check that the loop parks 50 and puts the clone on
# master, takes the repair `ci: core: ...` past a bug and another suite's `ci`
# issue, takes 50 back when the repair is closed, takes a group that lists a
# repair and not one that lists only another suite's issue, takes a repair
# listed after five issues it refuses, and neither parks nor claims with
# nothing to claim or with a file not committed. With --mutate each fault
# below is put into the copy and must fail its own row; the copy is restored
# before the rows run again at the end. --texts puts each fault into the copy
# and requires only that it changes the file, and runs no row.
#
# Usage: sh tests/system/worker_park_test.sh [--mutate | --texts]
#
set -u

case ${1:-} in
    '') mode=check ;;
    --mutate) mode=mutate ;;
    --texts) mode=texts ;;
    *) echo 'worker-test: usage: worker_park_test.sh [--mutate | --texts]' >&2; exit 2 ;;
esac
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/worker-test.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$work/bin" "$work/flow" "$work/home"

# fault NAME: put fault NAME into the private copy of worker.sh, or the
# original for `none`, and fail when a fault leaves the copy unchanged. The
# faults: park leaves the clone on the issue's branch; the loop never takes a
# parked issue back; repair takes any issue, or a group whatever it lists;
# claim counts an issue that repair refuses against its five tries; and park
# ignores a file that is not committed.
fault()
{
    # shellcheck disable=SC2016 # the dollars are in the texts sed matches
    case $1 in
        none) cat ;;
        stay) sed 's/^    git switch -q master 2> \/dev\/null && git merge -q --ff-only origin\/master || return 1$/    :/' ;;
        keep) sed 's/^        if \[ -z "\$issue" \] && \[ -s "\$state\/parked" \]; then$/        if false; then/' ;;
        any) awk '/^repair\(\)$/ { r = 1 } r && /^    return 1$/ { sub(/1/, "0"); r = 0 } { print }' ;;
        group) sed 's/^    \[ "\$members" != core \] || return 0$/    [ -z "$members" ] || return 0/' ;;
        count) awk '
            $0 == "        if [ \"${1-}\" = repair ] && ! repair \"$n\"; then" { held = 3 }
            held > 0 { kept = kept $0 "\n"; held--; next }
            { print }
            $0 == "        [ \"$tries\" -le 5 ] || break" { printf "%s", kept }' ;;
        dirty) sed 's/^    \[ -z "\$(git status --porcelain)" \] || return 1$/    :/' ;;
    esac < "$repo/tools/flow/worker.sh" > "$work/flow/worker.sh"
    [ "$1" = none ] || ! cmp -s "$repo/tools/flow/worker.sh" "$work/flow/worker.sh"
}

if [ "$mode" = texts ]; then
    for f in stay keep any group count dirty; do
        fault "$f" || { echo "worker-test: fault $f changes nothing" >&2; exit 1; }
    done
    exit 0
fi

# The stand-ins read the issues from WORKER_TEST_DATA, one a line, as
# `N|STATE|ASSIGNED|LABELS|TITLE|BODY`, with `-` for an empty field and `\n`
# for a newline in BODY, and append each call to WORKER_TEST_CALLS.
cat > "$work/flow/board.sh" <<'EOF'
#!/bin/sh
# board.sh: the test's stand-in for next, take and mine.
printf 'board %s\n' "$*" >> "$WORKER_TEST_CALLS"
data=$WORKER_TEST_DATA
case $1 in
    next) awk -F '|' '$2 == "OPEN" && $3 == "-" { print $1 "\t" $5 }' "$data" ;;
    take)
        awk -F '|' -v n="$2" '$1 == n && $2 == "OPEN" && $3 == "-" { f = 1 } END { exit !f }' "$data" ||
            exit 1
        awk -F '|' -v OFS='|' -v n="$2" '$1 == n { $3 = "me" } { print }' "$data" > "$data.new" &&
            mv "$data.new" "$data" ;;
    mine)
        awk -F '|' -v n="$2" '$1 == n { print $2 == "OPEN" && $3 == "me" ? "yes" : "no" }' "$data" ;;
    *) exit 2 ;;
esac
EOF
printf 'Work on issue @ISSUE@.\n' > "$work/flow/worker.txt"
cp "$repo/tools/flow/lib.sh" "$repo/tools/flow/session.pl" "$work/flow/"

# gh answers each query with what its --jq filter in worker.sh prints.
cat > "$work/bin/gh" <<'EOF'
#!/bin/sh
printf 'gh %s\n' "$*" >> "$WORKER_TEST_CALLS"
data=$WORKER_TEST_DATA
case "$1 $2 $*" in
    'issue list '*'--label hold'*)
        awk -F '|' '$2 == "OPEN" && ("," $4 ",") ~ /,hold,/ { print $1; exit }' "$data" ;;
    'issue view '*)
        case "$*" in
            *'--json labels '*) what=ci ;;
            *'--json title,labels,body '*) what=members ;;
            *'--json title,labels '*) what=core ;;
            *'--json state '*) what=state ;;
            *) exit 2 ;;
        esac
        awk -F '|' -v n="$3" -v what="$what" '
            $1 != n { next }
            {
                ci = ("," $4 ",") ~ /,ci,/
                core = ci && index($5, "ci: core: ") == 1
            }
            what == "ci" { print ci ? "true" : "false" }
            what == "core" { print core ? "true" : "false" }
            what == "state" { print $2 }
            what == "members" && core { print "core" }
            what == "members" && ci && !core && ("," $4 ",") ~ /,group,/ {
                k = split($6, line, /\\n/)
                for (i = 1; i <= k; i++)
                    if (line[i] ~ /^\* #[0-9]+$/)
                        print substr(line[i], 4)
            }' "$data" ;;
    *) exit 2 ;;
esac
EOF

# claude: record the session's issue, the clone's branch and the parked
# issue, then close the issue and the hold, as a session that pushed would.
cat > "$work/bin/claude" <<'EOF'
#!/bin/sh
n=$(printf '%s\n' "$2" | sed 's/^Work on issue \([0-9]*\)\.$/\1/')
printf '%s %s %s\n' "$n" "$(git rev-parse --abbrev-ref HEAD)" \
    "$(cat "$WORKER_TEST_STATE/parked" 2> /dev/null || echo -)" >> "$WORKER_TEST_SESSIONS"
awk -F '|' -v OFS='|' -v n="$n" '$1 == n || ("," $4 ",") ~ /,hold,/ { $2 = "CLOSED" } { print }' \
    "$WORKER_TEST_DATA" > "$WORKER_TEST_DATA.new" && mv "$WORKER_TEST_DATA.new" "$WORKER_TEST_DATA"
EOF

# sleep: count the call, and end the loop at the row's last.
cat > "$work/bin/sleep" <<'EOF'
#!/bin/sh
echo "$1" >> "$WORKER_TEST_SLEEPS"
[ "$(wc -l < "$WORKER_TEST_SLEEPS")" -lt "$WORKER_TEST_LAST" ] || touch "$WORKER_TEST_STATE/stop"
EOF
chmod +x "$work/bin/"* "$work/flow/board.sh"

# git ARGS...: run git as a scratch author with no global configuration.
g()
{
    git -c user.name=worker-test -c user.email=worker-test@example.invalid "$@"
}

# origin: make the scratch origin, whose master has the private tools/flow.
origin()
{
    rm -rf "$work/origin.git" "$work/seed"
    git init -q --bare -b master "$work/origin.git" &&
        git init -q -b master "$work/seed" &&
        mkdir -p "$work/seed/tools" &&
        cp -R "$work/flow" "$work/seed/tools/flow" &&
        g -C "$work/seed" add -A &&
        g -C "$work/seed" commit -q -m 'Add the flow scripts' &&
        g -C "$work/seed" push -q "$work/origin.git" master
}

# row NAME LAST ISSUES: run the loop until its LAST sleep on the issues 50
# and 77 and ISSUES, then check what row NAME requires.
row()
{
    rm -rf "$work/clone" "$work/state" "$work/data"* "$work/calls" "$work/sessions" "$work/sleeps"
    printf '%s\n' '50|OPEN|me|bug|tools: a fix|-' '77|OPEN|-|hold|hold: core fails|-' \
        "$3" > "$work/data"
    : > "$work/sessions"
    : > "$work/sleeps"
    g clone -q "$work/origin.git" "$work/clone" &&
        g -C "$work/clone" switch -q -c 50-fix &&
        echo fix > "$work/clone/fix.txt" &&
        g -C "$work/clone" add fix.txt &&
        g -C "$work/clone" commit -q -m 'Fix the tool' || return 1
    [ "$1" != dirty ] || echo notes > "$work/clone/notes.txt"
    mkdir -p "$work/state"
    echo 50 > "$work/state/issue"
    HOME=$work/home PATH=$work/bin:$PATH WORKER_TEST_DATA=$work/data \
        WORKER_TEST_CALLS=$work/calls WORKER_TEST_SESSIONS=$work/sessions \
        WORKER_TEST_SLEEPS=$work/sleeps WORKER_TEST_STATE=$work/state \
        WORKER_TEST_LAST=$2 sh "$work/clone/tools/flow/worker.sh" -s "$work/state" \
        > "$work/out" 2>&1 < /dev/null || return 1
    sessions=$(tr '\n' ',' < "$work/sessions")
    log=$work/state/loop.log
    case $1 in
        park)
            [ "$sessions" = '61 master 50,50 master -,' ] &&
                grep -q ' #50 parked for #61$' "$log" &&
                grep -q ' #50 taken back from parking$' "$log" &&
                [ -n "$(g -C "$work/clone" rev-list -n 1 origin/master..50-fix)" ] ;;
        group) [ "$sessions" = '63 master 50,' ] ;;
        sixth) [ "$sessions" = '61 master 50,' ] ;;
        none|dirty)
            [ -z "$sessions" ] && [ "$(cat "$work/state/issue")" = 50 ] &&
                [ ! -e "$work/state/parked" ] &&
                grep -q ' #50 is committed and waits for hold #77$' "$log" &&
                ! grep -q '^board take' "$work/calls" ;;
        *) return 1 ;;
    esac
}

# rows: run every row, naming each that fails.
rows()
{
    failed=
    while IFS=' ' read -r name last issues; do
        if ! row "$name" "$last" "$(printf '%s\n' "$issues" | tr ';' '\n')"; then
            echo "worker-test: failed: $name" >&2
            [ ! -f "$work/state/loop.log" ] || cat "$work/state/loop.log" >&2
            cat "$work/out" >&2
            failed="$failed $name"
        fi
    done <<'ROWS'
park 2 60|OPEN|-|bug|tests: a bug|-;62|OPEN|-|ci|ci: style: style fails on i686|-;61|OPEN|-|ci|ci: core: default fails on i686|-
group 1 65|OPEN|-|ci,group|ci: style group|* #62;62|OPEN|-|ci|ci: style: style fails on i686|-;63|OPEN|-|ci,group|ci: core group|* #66\n* #64;66|OPEN|-|ci|ci: style: host fails on i686|-;64|OPEN|-|ci|ci: core: default fails on i686|-
sixth 1 70|OPEN|-|bug|a|-;71|OPEN|-|bug|b|-;72|OPEN|-|ci|ci: style: c|-;73|OPEN|-|bug|d|-;74|OPEN|-|bug|e|-;61|OPEN|-|ci|ci: core: default fails on i686|-
none 1 60|OPEN|-|bug|tests: a bug|-;62|OPEN|-|ci|ci: style: style fails on i686|-
dirty 1 61|OPEN|-|ci|ci: core: default fails on i686|-
ROWS
    [ -z "$failed" ]
}

# caught NAME ROW: put fault NAME into the copy and require row ROW to fail.
caught()
{
    fault "$1" && origin || exit 1
    if rows > "$work/mutated.out" 2>&1; then
        echo "worker-test: fault passed: $1" >&2
        exit 1
    fi
    grep -q "^worker-test: failed: $2\$" "$work/mutated.out" || {
        echo "worker-test: fault $1 did not fail $2" >&2
        exit 1
    }
}

fault none && origin || exit 1
rows || exit 1
if [ "$mode" = mutate ]; then
    caught stay park
    caught keep park
    caught any park
    caught group group
    caught count sixth
    caught dirty dirty
    fault none && origin || exit 1
    rows || exit 1
    echo 'worker-test: result: pass: 6 faults caught and restored'
fi
echo 'worker-test: result: pass: 5 rows'
