#!/bin/sh
#
# worker_park_test.sh: worker.sh parks a committed issue while a `hold` is
# open, claims only a repair of core meanwhile, and takes the issue back; and
# it passes by an intermittent issue handed back with nothing new from ci.
#
# Each row runs the loop itself, from a private copy of tools/flow committed
# to a scratch origin, in a clone of that origin whose branch 50-fix has a
# commit master lacks, with issue 50 in STATE/issue and hold 77 open. Stand-ins
# for gh, board.sh, claude and sleep answer from a file of issues: a session
# writes its issue, the clone's branch and STATE/parked to a list, and closes
# its issue and the hold; each sleep is counted, and the row's last touches
# STATE/stop. The rows check that the loop parks 50 and puts the clone on
# master, takes the repair `Core tests fail on ...` past a bug and a `ci`
# issue of the release tests, takes 50 back when the repair is closed, takes a
# group that lists a repair and not one that lists only a release tests'
# issue, takes a repair `Core tests did not finish on ...` listed after five
# issues it refuses, and neither parks nor claims with nothing to claim or
# with a file not committed, leaving the clone on 50-fix.
# The rows rest, fresh and stale start with no issue and no hold, and give
# the timeline of issue 80, labelled `ci` and `intermittent`, which a session
# hands back: the loop runs one session on 80 and then passes it by, whether
# ci last reported on 80 before any hand-back, after an earlier one, or not at
# all since a hand-back two days old. The row plain hands back a `ci` issue
# that is not intermittent a minute before, and the loop takes it.
# WORKER_SH names the worker.sh to copy, by default the tree's;
# tests/proofs/worker-park.json runs the rows against copies with faults in
# them.
#
# Usage: sh tests/system/worker_park_test.sh
#
set -u

[ "$#" -eq 0 ] || { echo 'worker-test: usage: worker_park_test.sh' >&2; exit 2; }
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
trap 'rm -rf ${work:+"$work"}' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-worker-test.XXXXXX") || exit 1
mkdir -p "$work/bin" "$work/flow" "$work/home"

cp "${WORKER_SH:-$repo/tools/flow/worker.sh}" "$work/flow/worker.sh" || exit 1

# The stand-ins read the issues from WORKER_TEST_DATA, one a line, as
# `N|STATE|ASSIGNED|LABELS|TITLE|BODY`, with `-` for an empty field and `\n`
# for a newline in BODY, and append each call to WORKER_TEST_CALLS. The
# timelines are in WORKER_TEST_EVENTS, one event a line, as `N|KIND|TIME`, KIND
# being `back` for an unassignment or `ci` for a comment or label change of
# ci's, and TIME in seconds since the epoch.
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
        awk -F '|' -v n="$2" '$1 == n { print (($2 == "OPEN" && $3 == "me") ? "yes" : "no") }' "$data" ;;
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
    'api '*'/timeline '*)
        for a; do
            case $a in */timeline) n=${a%/timeline} n=${n##*/} ;; esac
        done
        awk -F '|' -v n="$n" '$1 == n { print $2, $3 }' "$WORKER_TEST_EVENTS" ;;
    'issue view '*)
        case "$*" in
            *'"intermittent"'*) what=flaky ;;
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
                core = ci && (index($5, "Core tests fail on ") == 1 ||
                    index($5, "Core tests did not finish on ") == 1)
            }
            what == "flaky" { print ("," $4 ",") ~ /,intermittent,/ ? "true" : "false" }
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
# issue, then close the issue and the hold, as a session that pushed would,
# or hand back an intermittent issue, unchanged, as a session that found
# nothing new would.
cat > "$work/bin/claude" <<'EOF'
#!/bin/sh
n=$(printf '%s\n' "$2" | sed 's/^Work on issue \([0-9]*\)\.$/\1/')
printf '%s %s %s\n' "$n" "$(git rev-parse --abbrev-ref HEAD)" \
    "$(cat "$WORKER_TEST_STATE/parked" 2> /dev/null || echo -)" >> "$WORKER_TEST_SESSIONS"
awk -F '|' -v OFS='|' -v n="$n" -v now="$(date +%s)" -v events="$WORKER_TEST_EVENTS" '
    { flaky = ("," $4 ",") ~ /,intermittent,/ }
    $1 == n && flaky { $3 = "-"; print n "|back|" now >> events }
    ($1 == n && !flaky) || ("," $4 ",") ~ /,hold,/ { $2 = "CLOSED" }
    { print }' \
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

# row NAME LAST ISSUES: run the loop until its LAST sleep on ISSUES, and,
# except in the rows of a hand-back, on the issues 50 and 77, then check what
# row NAME requires.
row()
{
    rm -rf "$work/clone" "$work/state" "$work/data"* "$work/calls" "$work/sessions" \
        "$work/sleeps" "$work/events"
    now=$(date +%s)
    case $1 in
        rest) printf '80|ci|%s\n' $((now - 7200)) ;;
        fresh) printf '80|back|%s\n80|ci|%s\n' $((now - 7200)) $((now - 3600)) ;;
        stale) printf '80|back|%s\n' $((now - 172800)) ;;
        plain) printf '81|back|%s\n' $((now - 60)) ;;
    esac > "$work/events"
    case $1 in
        rest|fresh|stale|plain) back=yes ;;
        *) back=no ;;
    esac
    if [ "$back" = yes ]; then
        printf '%s\n' "$3" > "$work/data"
    else
        printf '%s\n' '50|OPEN|me|bug|A fix to the tools|-' \
            '77|OPEN|-|hold|Pushes are held: master does not build, pass style or boot|-' \
            "$3" > "$work/data"
    fi
    : > "$work/sessions"
    : > "$work/sleeps"
    g clone -q "$work/origin.git" "$work/clone" || return 1
    mkdir -p "$work/state"
    if [ "$back" = no ]; then
        g -C "$work/clone" switch -q -c 50-fix &&
            echo fix > "$work/clone/fix.txt" &&
            g -C "$work/clone" add fix.txt &&
            g -C "$work/clone" commit -q -m 'Fix the tool' || return 1
        echo 50 > "$work/state/issue"
    fi
    [ "$1" != dirty ] || echo notes > "$work/clone/notes.txt"
    HOME=$work/home PATH=$work/bin:$PATH WORKER_TEST_DATA=$work/data \
        WORKER_TEST_EVENTS=$work/events \
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
                [ "$(g -C "$work/clone" rev-parse --abbrev-ref HEAD)" = 50-fix ] &&
                grep -q ' #50 is committed and waits for hold #77$' "$log" &&
                ! grep -q '^board take' "$work/calls" ;;
        rest|fresh|stale)
            [ "$sessions" = '80 master -,' ] &&
                grep -q ' #80 rests: handed back with nothing new from ci$' "$log" ;;
        plain) [ "$sessions" = '81 master -,' ] ;;
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
park 2 60|OPEN|-|bug|A bug in the tests|-;62|OPEN|-|ci|Release tests fail on i686: style|-;61|OPEN|-|ci|Core tests fail on i686: default|-
group 1 65|OPEN|-|ci,group|Fix the style of the release tests|* #62;62|OPEN|-|ci|Release tests fail on i686: style|-;63|OPEN|-|ci,group|Make the test image boot again|* #66\n* #64;66|OPEN|-|ci|Release tests fail on i686: host|-;64|OPEN|-|ci|Core tests fail on i686: default|-
sixth 1 70|OPEN|-|bug|a|-;71|OPEN|-|bug|b|-;72|OPEN|-|ci|Release tests fail on i686: c|-;73|OPEN|-|bug|d|-;74|OPEN|-|bug|e|-;61|OPEN|-|ci|Core tests did not finish on i686|-
none 1 60|OPEN|-|bug|A bug in the tests|-;62|OPEN|-|ci|Release tests fail on i686: style|-
dirty 1 61|OPEN|-|ci|Core tests fail on i686: default|-
rest 2 80|OPEN|-|ci,intermittent|Release tests fail on i686: default|-
fresh 2 80|OPEN|-|ci,intermittent|Release tests fail on i686: default|-
stale 2 80|OPEN|-|ci,intermittent|Release tests fail on i686: default|-
plain 2 81|OPEN|-|ci|Release tests fail on i686: default|-
ROWS
    [ -z "$failed" ]
}

origin || exit 1
rows || exit 1
echo 'worker-test: result: pass: 9 rows'
