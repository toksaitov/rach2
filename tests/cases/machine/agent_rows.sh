#!/bin/sh
# agent_rows.sh: the SSH workload of one etude agent case.
# Etude supplies the machine, keys, workspace and fake API service.
set -u
REPO=${ETUDE_TREE:?}
. "$REPO/tests/system/deadline.sh"
action=$1
name=$2
work=${AGENT_WORK:?}
PORT=${AGENT_PORT:?}
ROOT_KEY=${AGENT_ROOT_KEY:?}
TEST_KEY=${AGENT_TEST_KEY:?}
DISK=$work/row
KNOWN=$work/known
ROWS_DIR=$REPO/tests/system/agents
PACKAGES=$REPO/vendor/gnu/agents/packages
FAKEAPI=$REPO/tests/system/fakeapi/fakeapi.py
TPID=""

# End the request observer before this action ends.
stop_timing() {
    if [ -n "$TPID" ]; then
        kill -TERM "$TPID" 2>/dev/null || true
        wait "$TPID" 2>/dev/null || true
        TPID=""
    fi
}
trap 'stop_timing' EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
trap 'exit 129' HUP

# Report a failed SSH or application assertion.
fail() {
    echo "agents-test: $name: FAIL: $*" >&2
    exit 1
}
# Run COMMAND as USER by KEY, within the scenario step that owns it.
ssh_as() {
    ssh -F /dev/null -i "$2" -p "$PORT" -o StrictHostKeyChecking=no \
        -o "UserKnownHostsFile=$KNOWN" -o "ConnectTimeout=$(scaled 30)" -o BatchMode=yes \
        -o IdentitiesOnly=yes -o PreferredAuthentications=publickey "$1@127.0.0.1" "$3"
}

file=$ROWS_DIR/$name.sh
script=$(sed -n 's/^# script: //p' "$file")
if [ -n "$script" ]; then
    script=$REPO/tests/system/$script
    [ -f "$script" ] || fail "$file names no server script at $script"
else
    script=$ROWS_DIR/$name.json
fi
package=$(sed -n 's/^# package: //p' "$file")
package=${package:-$name}
version=""
if [ -f "$PACKAGES/$package.sh" ]; then version=$(sed -n 's/^VERSION=//p' "$PACKAGES/$package.sh"); fi
verifier=$(sed -n 's/^# verifier: //p' "$file")
verifier=$ROWS_DIR/${verifier:-${name}_verify.py}
mkdir -p "$work"
HOSTS=$(sed -n 's/^# hosts: //p' "$file" | tr ' ' '\n' | sed '/^$/d' | sort -u | tr '\n' ' ')
[ -n "$HOSTS" ] || HOSTS=fakeapi.test
if [ "$action" = prepare ]; then
    # shellcheck disable=SC2086 # DNS names from the row header
    python3 -B "$FAKEAPI" ca "$work/ca" $HOSTS || exit 1
    filter=$(sed -n 's/^# script-filter: //p' "$file")
    if [ -n "$filter" ]; then
        python3 -B "$ROWS_DIR/$filter" --script "$script" "$work/script.json" "$name" || exit 1
    elif [ -f "$script" ]; then
        cp "$script" "$work/script.json" || exit 1
    else
        printf '{"steps": []}\n' > "$work/script.json"
    fi
    exit 0
fi
case $action in
setup)
echo "agents-test: the test account logs in over SSH: OK"

lines=""
for host in $HOSTS; do
    lines="${lines}10.0.2.100 $host\\n"
done
ssh_as root "$ROOT_KEY" "printf '$lines' >> /etc/hosts" ||
    fail "the fake server's names in /etc/hosts"
ssh_as test "$TEST_KEY" 'rm -rf /tmp/agents && mkdir -p /tmp/agents/fixture' ||
    fail "the rows' directory on the machine"
git -C "$REPO" ls-files tests tools | while IFS= read -r f; do
    [ ! -e "$REPO/$f" ] || printf '%s\n' "$f"
done | tar -cf "$work/tree.tar" -C "$REPO" -T - || fail "the tar of the tree's tests and tools"
scp -F /dev/null -P "$PORT" -i "$TEST_KEY" -o StrictHostKeyChecking=no \
    -o "UserKnownHostsFile=$KNOWN" -o BatchMode=yes -o IdentitiesOnly=yes \
    "$REPO/tests/system/agents_rows.sh" "$ROWS_DIR"/*.sh "$work/ca/ca.pem" "$work/tree.tar" \
    test@127.0.0.1:/tmp/agents/ > /dev/null 2>&1 ||
    fail "scp could not put the rows on the machine"
scp -F /dev/null -P "$PORT" -i "$TEST_KEY" -o StrictHostKeyChecking=no \
    -o "UserKnownHostsFile=$KNOWN" -o BatchMode=yes -o IdentitiesOnly=yes \
    "$REPO/tests/system/fakeapi/repo/"* test@127.0.0.1:/tmp/agents/fixture/ \
    > /dev/null 2>&1 || fail "the shared fixture on the machine"
echo "agents-test: the fake server's names, $HOSTS, and its authority are on the machine: OK"

    ;;
row)
    if [ -f "$verifier" ]; then
        python3 -B "$verifier" --watch "$DISK.$name.record" "$DISK.$name.times.log" &
        TPID=$!
    fi
    began=$(date +%s)
    ssh_as test "$TEST_KEY" \
        "/bin/bash /tmp/agents/agents_rows.sh $name $version" > "$DISK.$name.log" 2>&1
    status=$?
    stop_timing
    took="$(($(date +%s) - began)) s"
    problem=""
    if ! grep -q "^AGENTROW OK $name\$" "$DISK.$name.log"; then
        problem="the row did not report AGENTROW OK $name: $(grep '^AGENTROW FAIL' "$DISK.$name.log" || tail -n 3 "$DISK.$name.log")"
    elif [ "$status" -ne 0 ]; then
        problem="the row printed its OK, but ssh ended with status $status"
    fi
    cat "$DISK.$name.log"
    [ -z "$problem" ] || fail "$problem"
    echo "agents-test: $name: OK in $took"
    ;;
verify)
    problem=""
    if [ -z "$problem" ] && [ -f "$verifier" ]; then
        if ! scp -F /dev/null -P "$PORT" -i "$TEST_KEY" \
            -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" -o BatchMode=yes \
            -o IdentitiesOnly=yes "test@127.0.0.1:/tmp/agents/results/$name.tar" \
            "$work/$name.tar" >> "$DISK.$name.log" 2>&1; then
            problem="the row's result archive could not be fetched"
        elif ! python3 -B "$verifier" "$work/$name.tar" "$DISK.$name.record" "$version" \
            > "$work/$name.verify.log" 2>&1; then
            problem="the host's verification: $(tail -n 1 "$work/$name.verify.log")"
        else
            cat "$work/$name.verify.log"
            echo "agents-test: $name: request number and elapsed host seconds:"
            cat "$DISK.$name.times.log"
        fi
        [ ! -f "$work/$name.verify.log" ] || cat "$work/$name.verify.log" >> "$DISK.$name.log"
    fi
    [ -z "$problem" ] || fail "$problem"
    echo "agents-test: $name: OK"
    ;;
*) exit 2 ;;
esac
