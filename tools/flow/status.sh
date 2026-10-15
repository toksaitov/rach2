#!/bin/sh
#
# status.sh: one look at the whole process, for the manager's check.
#
# Usage: status.sh
#
# It prints master's newest commit, ci's last eight runs, the open issues
# with their Status, assignees and labels, a line for each worker loop that
# `workers` lists, and the testing host's load, memory and emulators. A
# worker's line says whether its loop runs, its last log line, the seconds
# since its newest session wrote, how many test commands it runs, and its
# clone's commit and count of changed files. A loop on another host is read
# over ssh; one whose host is `-` runs on this machine. A loop that sends its
# tests to its own copy on the testing host, as the owner's loops do, counts
# the commands that work in that copy.

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
gh run list --repo "$REPO" --limit 8 \
    --json workflowName,status,conclusion,headSha,createdAt,databaseId \
    --jq '.[] | "\(.databaseId) \(.workflowName) \(.status) \(.conclusion // "-") \(.headSha[0:8]) \(.createdAt)"'
echo "== open issues"
gh api graphql -f o="${REPO%/*}" -f r="${REPO#*/}" -f query='
    query($o: String!, $r: String!) { repository(owner: $o, name: $r) {
        issues(states: OPEN, first: 100) { nodes { number title
            labels(first: 5) { nodes { name } }
            assignees(first: 5) { nodes { login } }
            projectItems(first: 2) { nodes { fieldValueByName(name: "Status") {
                ... on ProjectV2ItemFieldSingleSelectValue { name } } } } } } } }' \
    --jq '.data.repository.issues.nodes[] |
        "#\(.number) [\(.projectItems.nodes[0].fieldValueByName.name // "-")] \([.assignees.nodes[].login] | join(",")) {\([.labels.nodes[].name] | join(","))} \(.title[0:70])"'
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
echo "== testing host"
printf '%s\n' 'uptime' 'free -g | sed -n 2p' 'echo "qemu $(pgrep -c qemu-system || true)"' |
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" 'wsl.exe -u root -- sh -s' 2>&1 | tail -n 3
