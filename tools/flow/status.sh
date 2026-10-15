#!/bin/sh
#
# status.sh: one look at the whole process, for the manager's check.
#
# Usage: status.sh
#
# It prints master's newest commit; ci's last eight runs, each with the
# commit it started at and the commit whose status it set, or `?` while that
# is not known, since a core run tests the newest master when its first job
# runs; the open issues with their Status on the board, assignees and labels;
# a line for each worker loop that `workers` lists and one for the reviewer
# loop of this machine; and the testing host's load, memory and emulators. A
# worker's line says whether its loop runs, its last log line, the seconds
# since its newest session wrote, how many test commands it runs, and its
# clone's commit and count of changed files; the reviewer's, whether it runs,
# its last log line and its queue. A loop on another host is read over ssh;
# one whose host is `-` runs on this machine. A loop that sends its tests to
# its own copy on the testing host, as the owner's loops do, counts the
# commands that work in that copy.

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

echo "== master $(git ls-remote "https://github.com/$REPO" refs/heads/master | cut -c 1-8)  $(date -u '+%F %T') UTC"
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
    echo "loop $loop: $(tail -n 1 "$st/loop.log" 2> /dev/null) | queue: $(tr '\n' ' ' < "$st/queue" 2> /dev/null)"
fi
echo "== testing host"
printf '%s\n' 'uptime' 'free -g | sed -n 2p' 'echo "qemu $(pgrep -c qemu-system || true)"' |
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" 'wsl.exe -u root -- sh -s' 2>&1 | tail -n 3
