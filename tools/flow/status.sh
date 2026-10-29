#!/bin/sh
#
# status.sh: one look at the whole process, for the manager's check.
#
# Usage: status.sh
#        status.sh day DATE
#        status.sh sizes
#
# It prints master's newest commit; the phase In progress as `board.sh phase`
# reports it, whose last line says whether the phase is ready for its release;
# the record of the release under way and whether its tmux session
# `rach2-release` runs on the default tmux server, whichever server the caller
# is in; ci's last eight runs, each with the commit it started at and the
# commit whose status it set, or `?` while that is not known, since a core run
# tests the newest master when its first job runs; the open issues
# with their Status on the board, assignees and labels; a line for each
# worker loop that `workers` lists and one for the reviewer loop of this
# machine; and the testing host's load, memory and emulators. A worker's line
# says whether its loop runs, its last log line, the seconds since its newest
# session wrote, how many test commands it runs, and its clone's commit and
# count of changed files; the reviewer's, whether it runs, its last log line
# and the runs of the range it reviews. A loop on another host is read over
# ssh; one whose host is `-` runs on this machine. A loop that sends its tests
# to its own copy on the testing host, as the owner's loops do, counts the
# commands that work in that copy. Each run then lists its admission waits,
# their start times and holders, and whether each holder lives.
#
# Last come the resources: the free space of this machine's disk, of the
# testing host's WSL root and /tmp, whose tmpfs is the host's memory, and of
# its Windows drives C: and D:, which hold Windows and WSL's disk file; the
# free inodes of this machine's disk, the WSL root and /tmp; and the free
# memory of this machine, of Windows and of WSL, and WSL's free swap.
# The section ends with the verdict `resources: ok`, `resources: low: WHAT` or
# `resources: critical: WHAT`, WHAT naming each reading below its threshold:
# low is under 20% free, and critical under 10%, or under 40 GiB on a disk,
# the floor that tests/run keeps. Memory is reported and not judged, since a
# suite takes it for minutes. A reading that is missing or malformed has a
# line `NAME KIND: did not answer`; the verdict is then `resources: unknown`,
# or a low or critical one that ends `; unknown: NAME KIND ... did not answer`.
#
# `sizes` prints the sizes of the build trees, the GNU caches, the ci checkouts,
# the loops' state, the copies under `build/worktrees` and /tmp on the testing
# host, and of the loops' state, the worktrees and the build tree of this
# clone's main working tree here, largest first, for the manager to choose what
# to remove when space is low. It then lists, through `leftovers -n` in lib.sh,
# the failed tests' directories, loose files and copies that the manager's loop
# would remove before its next check, and the copies and old entries it keeps,
# with why. It reads every file of those trees, which takes minutes, so the
# manager's check runs it only then.
#
# `day DATE` prints the facts of one day, DATE as YYYY-MM-DD, counted at the
# owner's UTC+6, for the manager's day report: the commits that reached
# master, the issues closed, the core and full runs by their result, and the
# phases closed, each with its list, so that the report's numbers are counted
# by a program and not by the model that writes it.

# shellcheck disable=SC2016 # the dollars are GraphQL's, jq's and the probe's
REPO=toksaitov/rach2
TESTING=testing
# shellcheck source=tools/flow/lib.sh
. "$(dirname "$0")/lib.sh"
here=$(primary "$(dirname "$0")")

probe='
st=$1
case $st in "~"*) st=$HOME${st#"~"} ;; esac
if [ ! -d "$st" ]; then echo "no loop has run here: no $st"; exit 0; fi
repo=$(cat "$st/repo" 2> /dev/null)
copy=$(cat "$st/copy" 2> /dev/null)
if pgrep -f "$st/run/worker.sh" > /dev/null; then loop=runs; else loop="not running"; fi
cur=$(ls -t "$st"/s-*.jsonl 2> /dev/null | head -n 1)
age=none
[ -z "$cur" ] || age=$(perl -e "print time - (stat shift)[9]" "$cur")s
if [ -n "$copy" ]; then
    tests=$(ps -u "$(id -u)" -o args= | grep -c "[c]d $copy &&")
else
    tests=$(ps -u "$(id -u)" -o args= | grep -c "[t]ests/run ")
fi
echo "loop $loop: $(tail -n 1 "$st/loop.log" 2> /dev/null) | silent $age | $tests tests |" \
    "$(git -C "$repo" log -1 --format=%h 2> /dev/null)" \
    "$(git -C "$repo" status --porcelain 2> /dev/null | wc -l | tr -d " ") changed"
'
# Send this copy of the reader even when a worker's checkout is older.
probe="$probe
if [ -n \"\$copy\" ]; then
    echo \"test-copy \$copy\"
else
    python3 - \"\$repo\" <<'TEST_STATUS_PY'
$(cat "$(dirname "$0")/test_status.py")
TEST_STATUS_PY
fi
"

# whole PATH JQ: print the rows that JQ makes of every page of the GitHub API's
# list at PATH, JQ writing each row as the list's `total_count`, a space and
# the row. Fail when a request fails or the rows are fewer than the total, as
# they are past the API's limit of 1000 rows for a search.
whole()
{
    rows=$(gh api --paginate "$1" --jq "$2") || return 1
    printf '%s\n' "$rows" | awk 'NF { t = $1; n++; sub(/^[^ ]* /, ""); print }
        END { if (n != t + 0) { printf "status: %d of %d rows\n", n, t > "/dev/stderr"; exit 1 } }'
}

# runs WORKFLOW RANGE: print WORKFLOW's runs created in RANGE, each as
# `ID RESULT TITLE`.
runs()
{
    whole "repos/$REPO/actions/workflows/$1/runs?per_page=100&created=$2" \
        '.total_count as $t | .workflow_runs[] |
        "\($t) \(.id) \(.conclusion // .status) \(.display_title)"'
}

# list WHAT ROWS: print the count of ROWS, labelled WHAT, and ROWS indented.
list()
{
    echo "$1: $(printf '%s' "$2" | grep -c .)"
    printf '%s\n' "$2" | grep . | sed 's/^/  /'
}

# tally WORKFLOW ROWS: print the count of the runs ROWS lists, by result, and
# ROWS indented.
tally()
{
    echo "$1 runs: $(printf '%s' "$2" | grep -c .), by result: $(printf '%s\n' "$2" |
        awk 'NF { n[$2]++ } END { for (r in n) printf "%s %d; ", r, n[r] }')"
    printf '%s\n' "$2" | grep . | sed 's/^/  /'
}

# readings: print one line for each resource the header names, as `NAME KIND
# FREE TOTAL`, KIND being disk, tmpfs, inodes or memory, and FREE and TOTAL
# in KiB, in inodes, or for this machine's memory in percent. A reading the
# host does not give is missing; `resources` finds it so.
readings()
{
    df -Pk "$HOME" | awk 'NR == 2 { print "mac:disk disk", $4, $2 }'
    df -Pi "$HOME" | awk 'NR == 2 { print "mac:disk inodes", $(NF - 2), $(NF - 3) + $(NF - 2) }'
    memory_pressure -Q 2> /dev/null |
        awk -F ': ' '/free percentage/ { sub(/%/, "", $2); print "mac:memory memory", $2, 100 }'
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" 'df -Pk / /d' 2> /dev/null |
        awk 'NR > 1 { print "windows:" substr($1, 1, 1), "disk", $(NF - 2), $(NF - 4) }'
    # shellcheck disable=SC2016 # the dollars are PowerShell's and awk's
    printf '%s\n' \
        'df -Pk / /tmp | awk '"'"'NR > 1 { print "wsl:" $6, ($6 == "/tmp" ? "tmpfs" : "disk"), $4, $2 }'"'" \
        'df -Pi / /tmp | awk '"'"'NR > 1 { print "wsl:" $6, "inodes", $4, $2 }'"'" \
        'awk '"'"'{ m[$1] = $2 } END { print "wsl:memory memory", m["MemAvailable:"], m["MemTotal:"]; print "wsl:swap memory", m["SwapFree:"], m["SwapTotal:"] }'"'"' /proc/meminfo' \
        'cd / && timeout 60 powershell.exe -NoProfile -Command '"'"'$o = Get-CimInstance Win32_OperatingSystem; "$($o.FreePhysicalMemory) $($o.TotalVisibleMemorySize)"'"'"' | tr -d "\r" | awk '"'"'NF == 2 { print "windows:memory memory", $1, $2 }'"'" |
        ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" 'wsl.exe -u root -- sh -s' 2> /dev/null
}

# resources: print the readings in words and the verdict, as the header says.
resources()
{
    readings | awk '
        BEGIN {
            n = split("windows:C disk,windows:D disk,windows:memory memory,wsl:/ disk," \
                "wsl:/tmp tmpfs,wsl:/ inodes,wsl:/tmp inodes,wsl:memory memory,wsl:swap memory," \
                "mac:disk disk,mac:disk inodes,mac:memory memory", want, ",")
        }
        NF == 4 && $3 ~ /^[0-9]+$/ && $4 ~ /^[0-9]+$/ {
            seen[$1 " " $2] = 1
            pct = $4 > 0 ? int(100 * $3 / $4) : 0
            if ($2 == "inodes") printf "%s inodes: %d%% free\n", $1, pct
            else if ($1 == "mac:memory") printf "%s: %d%% free\n", $1, pct
            else printf "%s: %.0f GiB free of %.0f (%d%%)\n", $1, $3 / 1048576, $4 / 1048576, pct
            if ($2 == "memory") next
            if (pct < 10 || ($2 == "disk" && $3 < 40 * 1048576)) critical = critical " " $1 " " $2
            else if (pct < 20) low = low " " $1 " " $2
        }
        END {
            for (i = 1; i <= n; i++) {
                if (want[i] in seen) continue
                split(want[i], w, " ")
                printf "%s %s: did not answer\n", w[1], w[2]
                unknown = unknown " " w[1] " " w[2]
            }
            u = unknown != "" ? "; unknown:" unknown " did not answer" : ""
            if (critical != "") print "resources: critical:" critical u
            else if (low != "") print "resources: low:" low u
            else if (unknown != "") print "resources: unknown:" unknown " did not answer"
            else print "resources: ok"
        }'
}

# sizes: print the sizes the header names, in GiB, largest first.
sizes()
{
    echo "== sizes on the testing host, in GiB"
    printf '%s\n' 'du -sxk /home/*/rach2*/build /home/*/rach2*/build/vendor/gnu \
        /home/*/rach2*/build/x86_64/vendor/gnu /home/*/rach2/build/flow \
        /home/*/rach2/build/worktrees/* /home/ghrunner/rach2-ci-*/_work /tmp 2> /dev/null | sort -rn |
        awk '"'"'{ printf "%7.1f %s\n", $1 / 1048576, $2 }'"'" |
        ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" 'wsl.exe -u root -- sh -s' 2>&1
    echo "== sizes here, in GiB"
    du -sxk "$here"/build/flow/* "$here"/build/worktrees/* "$here"/build \
        "$here"/.claude/worktrees 2> /dev/null | sort -rn |
        awk '{ printf "%7.1f %s\n", $1 / 1048576, $2 }'
    echo "== leftovers that the manager's loop removes, and the old ones it keeps"
    leftovers -n
}

# day DATE: print the facts of DATE, as the header says. Print nothing and fail
# when one of them cannot be read whole.
day()
{
    from="$1T00:00:00+06:00"
    to=$(date -j -v+1d -f %F "$1" +%F 2> /dev/null || date -d "$1 + 1 day" +%F) || return 2
    to="${to}T00:00:00+06:00"
    range=$(printf '%s..%s' "$from" "$to" | sed 's/+/%2B/g')
    search="search/issues?per_page=100&q=repo:$REPO+is:issue+is:closed+closed:$range"
    git fetch -q origin || { echo "status: day: cannot fetch origin" >&2; return 1; }
    commits=$(git log --first-parent --format='%h %s' --since="$from" --until="$to" origin/master) ||
        { echo "status: day: cannot read master's log" >&2; return 1; }
    closed=$(whole "$search" '.total_count as $t | .items[] |
        "\($t) #\(.number) \(.state_reason) \(.title[0:70])"') ||
        { echo "status: day: cannot read the closed issues" >&2; return 1; }
    ci=$(runs ci.yml "$range") || { echo "status: day: cannot read the runs of ci.yml" >&2; return 1; }
    full=$(runs full.yml "$range") || { echo "status: day: cannot read the runs of full.yml" >&2; return 1; }
    phases=$(whole "$search+label:phase" '.total_count as $t | .items[] |
        "\($t) #\(.number) \(.title)"') ||
        { echo "status: day: cannot read the closed phases" >&2; return 1; }
    echo "== $1 at UTC+6: $from to $to"
    list "commits on master" "$commits"
    list "issues closed" "$closed"
    tally ci.yml "$ci"
    tally full.yml "$full"
    list "phases closed" "$phases"
}

if [ "$1" = sizes ]; then
    sizes
    exit
fi
if [ "$1" = day ]; then
    [ -n "$2" ] || { echo "usage: status.sh day YYYY-MM-DD" >&2; exit 2; }
    day "$2"
    exit
fi

echo "== master $(git ls-remote "https://github.com/$REPO" refs/heads/master | cut -c 1-8)  $(date -u '+%F %T') UTC"
echo "== phase"
sh "$(dirname "$0")/board.sh" phase 2>&1
echo "== progress"
python3 -B "$(dirname "$0")/progress.py" 2>&1 || echo "status: progress could not be read; investigate before acting"
echo "== release"
if env -u TMUX tmux has-session -t rach2-release 2> /dev/null; then echo "release.sh runs"; else echo "release.sh is not running"; fi
r=$here/build/flow/release/record
if [ -f "$r" ]; then
    echo "record $r:"
    tail -n 6 "$r"
else
    echo "no release record"
fi
echo "== ci runs"
tested=$(gh api graphql -f o="${REPO%/*}" -f r="${REPO#*/}" -f query='
    query($o: String!, $r: String!) { repository(owner: $o, name: $r) {
        ref(qualifiedName: "refs/heads/master") { target { ... on Commit {
            history(first: 50) { nodes { oid status { contexts { targetUrl } } } } } } } } }' \
    --jq '.data.repository.ref.target.history.nodes[] | .oid[0:8] as $c |
        (.status.contexts // [])[] | "\(.targetUrl | split("/") | .[-1]) \($c)"')
gh run list --repo "$REPO" --limit 8 \
    --json workflowName,status,conclusion,headSha,createdAt,databaseId \
    --jq '.[] | "\(.databaseId) \(.workflowName) \(.status) \(.conclusion // "-") \(.headSha[0:8]) \(.createdAt)"' |
while read -r id rest; do
    echo "$id $rest tested $(printf '%s\n' "$tested" | awk -v id="$id" '$1 == id { print $2; exit }' | grep . || echo '?')"
done
echo "== open issues"
issues=$(gh api graphql --paginate -f o="${REPO%/*}" -f r="${REPO#*/}" -f query='
    query($o: String!, $r: String!, $endCursor: String) {
        repository(owner: $o, name: $r) {
        issues(states: OPEN, first: 100, after: $endCursor) {
            pageInfo { hasNextPage endCursor } nodes { number title
            labels(first: 10) { nodes { name } }
            assignees(first: 5) { nodes { login } }
            projectItems(first: 10) { nodes { project { number }
                fieldValueByName(name: "Status") {
                ... on ProjectV2ItemFieldSingleSelectValue { name } } } } } } } }' \
    --jq '.data.repository.issues.nodes[] |
        "#\(.number) [\([.projectItems.nodes[] | select(.project.number == 6)][0].fieldValueByName.name // "-")] \([.assignees.nodes[].login] | join(",")) {\([.labels.nodes[].name] | join(","))} \(.title[0:70])"') ||
    issues="status: the open issues cannot be read"
printf '%s\n' "$issues"
printf '%s\n' "$issues" | awk '
    /^#/ {
        if ($2 == "[Ready]") r++
        else if ($2 == "[In") p++
        if ($0 ~ /[{,]owner[,}]/) o++
        else if ($2 == "[Backlog]" && $0 !~ /[{,]phase[,}]/) b++
    }
    END { printf "queue: %d Ready, %d In progress, %d owner decisions, %d other in Backlog\n", r, p, o, b }'
echo "== workers"
grep -v '^#' "$(dirname "$0")/workers" | while read -r name host st; do
    [ -n "$name" ] || continue
    printf '%s: ' "$name"
    if [ "$host" = - ]; then
        case $st in /*|"~"*) ;; *) st=$here/$st ;; esac
        report=$(printf '%s' "$probe" | sh -s "$st" 2>&1)
    else
        report=$(printf '%s' "$probe" | ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" sh -s "$st" 2>&1)
    fi
    printf '%s\n' "$report" | sed '/^test-copy /d'
    copy=$(printf '%s\n' "$report" | sed -n 's/^test-copy //p')
    case $copy in
        '') ;;
        *[!A-Za-z0-9/._-]*|*/..*) echo "  test status: invalid copy path" ;;
        /*) ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" \
                "MSYS_NO_PATHCONV=1 wsl.exe -u root -- python3 - $copy" < "$(dirname "$0")/test_status.py" 2>&1 ;;
        *) echo "  test status: invalid copy path" ;;
    esac
done
echo "== reviewer"
st=$here/build/flow/reviewer
if [ ! -d "$st" ]; then
    echo "no reviewer loop has run here: no $st"
else
    if pgrep -f "$st/run/reviewer.sh" > /dev/null; then loop=runs; else loop="not running"; fi
    echo "loop $loop: $(tail -n 1 "$st/loop.log" 2> /dev/null) | tries: $(tr '\n' ' ' 2> /dev/null < "$st/tries")"
fi
echo "== testing host"
printf '%s\n' 'uptime' 'free -g | sed -n 2p' 'echo "qemu $(pgrep -c qemu-system || true)"' |
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" 'wsl.exe -u root -- sh -s' 2>&1 | tail -n 3
echo "== resources"
resources
