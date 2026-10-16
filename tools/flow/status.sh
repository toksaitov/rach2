#!/bin/sh
#
# status.sh: one look at the whole process, for the manager's check.
#
# Usage: status.sh
#        status.sh day DATE
#
# It prints master's newest commit; the phase In progress as `board.sh phase`
# reports it, whose last line says whether the phase is ready for its release;
# the record of the release under way and whether its tmux session
# `rach2-release` runs; ci's last eight runs, each with the commit it started
# at and the commit whose status it set, or `?` while that is not known, since
# a core run tests the newest master when its first job runs; the open issues
# with their Status on the board, assignees and labels; a line for each
# worker loop that `workers` lists and one for the reviewer loop of this
# machine; and the testing host's load, memory and emulators. A worker's line
# says whether its loop runs, its last log line, the seconds since its newest
# session wrote, how many test commands it runs, and its clone's commit and
# count of changed files; the reviewer's, whether it runs, its last log line
# and the runs of the range it reviews. A loop on another host is read over
# ssh; one whose host is `-` runs on this machine. A loop that sends its tests
# to its own copy on the testing host, as the owner's loops do, counts the
# commands that work in that copy.
#
# `day DATE` prints the facts of one day, DATE as YYYY-MM-DD, counted at the
# owner's UTC+6, for the manager's day report: the commits that reached
# master, the issues closed, the core and full runs by their result, and the
# phases closed, each with its list, so that the report's numbers are counted
# by a program and not by the model that writes it.

# shellcheck disable=SC2016 # the dollars are GraphQL's, jq's and the probe's
REPO=toksaitov/rach2
TESTING=testing

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

# day DATE: print the facts of DATE, as the header says.
day()
{
    from="$1T00:00:00+06:00"
    to=$(date -j -v+1d -f %F "$1" +%F 2> /dev/null || date -d "$1 + 1 day" +%F)
    to="${to}T00:00:00+06:00"
    git fetch -q origin
    echo "== $1 at UTC+6: $from to $to"
    commits=$(git log --first-parent --format='%h %s' --since="$from" --until="$to" origin/master)
    echo "commits on master: $(printf '%s' "$commits" | grep -c .)"
    printf '%s\n' "$commits" | grep . | sed 's/^/  /'
    closed=$(gh issue list --repo "$REPO" --state closed --limit 500 \
        --search "closed:$from..$to" --json number,title,stateReason \
        --jq '.[] | "#\(.number) \(.stateReason | ascii_downcase) \(.title[0:70])"')
    echo "issues closed: $(printf '%s' "$closed" | grep -c .)"
    printf '%s\n' "$closed" | grep . | sed 's/^/  /'
    for w in ci.yml full.yml; do
        runs=$(gh run list --repo "$REPO" --workflow "$w" --limit 200 --created "$from..$to" \
            --json conclusion,status,displayTitle,databaseId \
            --jq '.[] | "\(.databaseId) \(.conclusion // .status) \(.displayTitle)"')
        echo "$w runs: $(printf '%s' "$runs" | grep -c .), by result: $(printf '%s\n' "$runs" |
            awk 'NF { n[$2]++ } END { for (r in n) printf "%s %d; ", r, n[r] }')"
        printf '%s\n' "$runs" | grep . | sed 's/^/  /'
    done
    phases=$(gh issue list --repo "$REPO" --state closed --label phase --limit 50 \
        --search "closed:$from..$to" --json number,title --jq '.[] | "#\(.number) \(.title)"')
    echo "phases closed: $(printf '%s' "$phases" | grep -c .)"
    printf '%s\n' "$phases" | grep . | sed 's/^/  /'
}

if [ "$1" = day ]; then
    [ -n "$2" ] || { echo "usage: status.sh day YYYY-MM-DD" >&2; exit 2; }
    day "$2"
    exit
fi

echo "== master $(git ls-remote "https://github.com/$REPO" refs/heads/master | cut -c 1-8)  $(date -u '+%F %T') UTC"
echo "== phase"
sh "$(dirname "$0")/board.sh" phase 2>&1
echo "== release"
if tmux has-session -t rach2-release 2> /dev/null; then echo "release.sh runs"; else echo "release.sh is not running"; fi
r=$HOME/flow/manager/release/record
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
        printf '%s' "$probe" | sh -s "$st" 2>&1 | tail -n 1
    else
        printf '%s' "$probe" | ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" sh -s "$st" 2>&1 | tail -n 1
    fi
done
echo "== reviewer"
st=$HOME/flow/reviewer
if [ ! -d "$st" ]; then
    echo "no reviewer loop has run here: no $st"
else
    if pgrep -f "$st/run/reviewer.sh" > /dev/null; then loop=runs; else loop="not running"; fi
    echo "loop $loop: $(tail -n 1 "$st/loop.log" 2> /dev/null) | tries: $(tr '\n' ' ' < "$st/tries" 2> /dev/null)"
fi
echo "== testing host"
printf '%s\n' 'uptime' 'free -g | sed -n 2p' 'echo "qemu $(pgrep -c qemu-system || true)"' |
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" 'wsl.exe -u root -- sh -s' 2>&1 | tail -n 3
