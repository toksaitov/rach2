#!/bin/sh
#
# release_resume_test.sh: release.sh resumes a release from its record.
#
# Each row writes a record to a scratch STATE and runs a private copy of
# tools/flow/release.sh, in a scratch tree, against stand-ins for gh, ssh,
# git, sleep, board.sh and notify.sh that append each call to a list. The
# machine answers the reads of `where`, `bootctl status` and
# /etc/rach2/release, takes a write of that file when the row allows it, and
# refuses every other command; the board says what the row gives; gh lists
# no runs.
# The rows check that a release whose steps are all done only reports and
# closes the phase, and one whose full run is done goes to the build without
# a second; that a void or failed record stays in place while the board
# gives no candidate, and is copied aside, keeping its `touched` line, once
# it gives one; that a record with `install started` and no `install done`
# stops the script before anything else, and that a machine running its
# system after the installer started fails the install without a write to
# the machine; that a release whose acceptance is done writes
# /etc/rach2/release before it closes the phase, and fails without closing
# it when the file does not read back; and that a record with `closing`
# finishes that phase whatever phase was given, checking an open phase for
# readiness again. With --mutate
# each fault below is put into the copy and must fail its own row; the copy
# is restored before the rows run again at the end. --texts puts each fault
# into the copy and requires only that it changes the file, and runs no row.
#
# Usage: sh tests/system/release_resume_test.sh [--mutate | --texts]
#
set -u

case ${1:-} in
    '') mode=check ;;
    --mutate) mode=mutate ;;
    --texts) mode=texts ;;
    *) echo 'release-test: usage: release_resume_test.sh [--mutate | --texts]' >&2; exit 2 ;;
esac
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/release-test.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
tree=$work/tree
mkdir -p "$work/bin" "$tree/tools/flow" "$tree/krffs"
: > "$tree/krffs/rach2_demo_ed25519"

# fault NAME: put fault NAME into the private copy of release.sh, or the
# original for `none`, and fail when a fault leaves the copy unchanged. The
# faults: no step counts as done; a done full run is applied again; an ended
# record is copied aside before the board is read; the new record drops the
# `touched` line; an ended record with `install started` is replaced; a
# machine running its system after the installer started is sent to
# recovery; the record step is skipped once the acceptance is done; the
# record is not read back; the phase of a record with `closing` is ignored;
# and an open phase is closed without its readiness checked again.
fault()
{
    # shellcheck disable=SC2016 # the dollars are in the texts sed matches
    case $1 in
        none) cat ;;
        undone) sed 's/^    grep -q "^\$1 done " "\$state\/record"$/    false/' ;;
        full) sed 's/^    done_ full || full$/    full/' ;;
        aside) awk '
            $0 == "    if [ -n \"$old\" ] || [ ! -f \"$state/record\" ]; then" {
                print "    [ -z \"$old\" ] || cp \"$state/record\" \"$state/$old-early\""
            }
            { print }' ;;
        touched) sed 's/^                grep .^touched . "\$state\/record" | tail -n 1$/                :/' ;;
        started) sed 's/^        if grep -q .^install started . "\$state\/record" &&$/        if false \&\&/' ;;
        reboot) sed 's/^                ! grep -q .^install started . "\$state\/record" ||$/                true ||/' ;;
        unrecorded) sed 's/^    if ! done_ record; then$/    if ! done_ accept; then/' ;;
        unread) sed 's/^        \[ .* = "\$want" \] ||$/        true ||/' ;;
        closing) sed 's/^        phase=\$(sed -n .s\/^phase \/\/p. "\$state\/record")$/        :/' ;;
        ready) awk '
            /^    issue_state=/ { r = 1 }
            r && $0 == "        why=$(still_ready)" { $0 = "        why=; true"; r = 0 }
            { print }' ;;
    esac < "$repo/tools/flow/release.sh" > "$tree/tools/flow/release.sh"
    [ "$1" = none ] || ! cmp -s "$repo/tools/flow/release.sh" "$tree/tools/flow/release.sh"
}

if [ "$mode" = texts ]; then
    for f in undone full aside touched started reboot unrecorded unread closing ready; do
        fault "$f" || { echo "release-test: fault $f changes nothing" >&2; exit 1; }
    done
    exit 0
fi

# board.sh prints RELEASE_TEST_BOARD for `phase` and succeeds otherwise.
cat > "$tree/tools/flow/board.sh" <<'EOF'
printf 'board %s\n' "$*" >> "$RELEASE_TEST_CALLS"
[ "$1" != phase ] || printf '%s\n' "$RELEASE_TEST_BOARD"
EOF
cat > "$tree/tools/flow/notify.sh" <<'EOF'
printf 'notify %s\n' "$*" >> "$RELEASE_TEST_CALLS"
EOF

# gh comments, closes and gives the phase's state, RELEASE_TEST_STATE, and
# fails every other query, which the script takes for a passing fault.
cat > "$work/bin/gh" <<'EOF'
#!/bin/sh
printf 'gh %s %s %s\n' "$1" "$2" "$3" >> "$RELEASE_TEST_CALLS"
case "$1 $2 $*" in
    'issue comment '*|'issue close '*) ;;
    'issue view '*'--json state'*) echo "$RELEASE_TEST_STATE" ;;
    *) exit 1 ;;
esac
EOF

# ssh answers the testing host and the machine's reads, as the Lenovo
# running its system's candidate slot, keeps a write of /etc/rach2/release
# in RELEASE_TEST_CALLS.release when RELEASE_TEST_WRITES is yes, and refuses
# every other command.
cat > "$work/bin/ssh" <<'EOF'
#!/bin/sh
for a; do host=$command command=$a; done
case $host in
    testing) printf 'ssh testing %s\n' "$command" >> "$RELEASE_TEST_CALLS"; exit 0 ;;
esac
case $command in
    'cat /sys/class/net/eth0/address;'*)
        printf 'machine read\n' >> "$RELEASE_TEST_CALLS"
        printf '%s\n' 28:d2:44:7e:e0:af 'root=/dev/disk0p3 rach2.slot=candidate' \
            '5000.00 9000.00' 'identity 0 nvme S6P2NX0T381638Z' ;;
    'bootctl status')
        printf 'machine read\n' >> "$RELEASE_TEST_CALLS"
        echo 'bootctl: next_entry: empty' ;;
    'cat /etc/rach2/release 2> /dev/null; echo answered')
        printf 'machine read\n' >> "$RELEASE_TEST_CALLS"
        cat "$RELEASE_TEST_CALLS.release" 2> /dev/null
        echo answered ;;
    'mkdir -p /etc/rach2 && cat > /etc/rach2/release.new &&'*)
        [ "$RELEASE_TEST_WRITES" = yes ] || {
            printf 'machine write refused\n' >> "$RELEASE_TEST_CALLS"
            exit 1
        }
        printf 'machine write release\n' >> "$RELEASE_TEST_CALLS"
        cat > "$RELEASE_TEST_CALLS.release" ;;
    *)
        printf 'machine write %s\n' "$command" >> "$RELEASE_TEST_CALLS"
        exit 1 ;;
esac
EOF
cat > "$work/bin/git" <<'EOF'
#!/bin/sh
printf 'git %s\n' "$*" >> "$RELEASE_TEST_CALLS"
exit 1
EOF

# sleep returns at once, and stops a script that waits more than twenty times.
cat > "$work/bin/sleep" <<'EOF'
#!/bin/sh
echo "$1" >> "$RELEASE_TEST_CALLS.sleeps"
[ "$(wc -l < "$RELEASE_TEST_CALLS.sleeps")" -le 20 ] || kill -TERM "$PPID"
EOF
chmod +x "$work/bin/"*

H=1111111111111111111111111111111111111111
C=2222222222222222222222222222222222222222
T=2026-10-10T00:00:00Z

# The records of the rows. `head` has a candidate whose full run passed,
# `built` adds every step up to the transfer, `installing` the installer's
# start, `accepted` every step up to the acceptance, and `finished` the
# installed record.
head="phase 7
sha $H
chosen $T
dispatched 1760054400
run 101 success
full done $T run 101
touched $T"
built="$head
build done $T kernel k root r
recovery done $T kernel k image i
transfer done $T kernel and root in /dev/shm
kernel done $T candidate slot"
installing="$built
install started $T"
accepted="$installing
install done $T root r
trial done $T the new kernel runs from the candidate slot
accept done $T metal-accept: PASS; committed"
finished="$accepted
record done $T /etc/rach2/release names $H"
ended="phase 7
sha $H
chosen $T
touched $T
void: ready: no: work: #14"

# run P RECORD BOARD [STATE [WRITES]]: write RECORD, have the board report
# BOARD for phase 7, gh report the phase as STATE, OPEN unless given, and the
# machine take a write of /etc/rach2/release when WRITES is yes, and run the
# copy for phase P, keeping its output in out and its status in `result`.
run()
{
    rm -rf "$work/state" "$work/calls" "$work/calls.sleeps" "$work/calls.release"
    mkdir -p "$work/state/wt/krffs" "$work/state/wt/build/x86_64/images"
    : > "$work/state/wt/krffs/rach2_demo_ed25519"
    : > "$work/state/wt/build/x86_64/images/release.sha256"
    : > "$work/calls"
    printf '%s\n' "$2" > "$work/state/record"
    cp "$work/state/record" "$work/before"
    result=0
    PATH=$work/bin:$PATH RELEASE_TEST_CALLS=$work/calls \
        RELEASE_TEST_BOARD="phase: #7 phase: seven
$3" RELEASE_TEST_STATE=${4:-OPEN} RELEASE_TEST_WRITES=${5:-no} \
        sh "$tree/tools/flow/release.sh" -s "$work/state" "$1" > "$work/out" 2>&1 < /dev/null ||
        result=$?
}

# aside PREFIX: print the names of the records copied aside that begin PREFIX.
aside()
{
    (cd "$work/state" && ls) | grep "^$1"
}

# called PATTERN: succeed when a call matches the grep PATTERN.
called()
{
    grep -q "$1" "$work/calls"
}

# row NAME: run row NAME and check what it leaves.
row()
{
    case $1 in
        all-done)
            run 7 "$finished" "ready: yes $H"
            a=$(aside 7-11111111-)
            [ "$result" -eq 0 ] && [ ! -e "$work/state/record" ] && [ -n "$a" ] &&
                [ "$(tail -n 1 "$work/state/$a" | cut -d ' ' -f 1)" = closed ] &&
                [ "$(grep -c ' done ' "$work/state/$a")" -eq 9 ] &&
                called '^gh issue close 7$' && called '^board archive 7$' &&
                ! called '^machine\|^ssh\|^git\|^gh run\|^gh workflow' ;;
        full-done)
            run 7 "$head" "ready: yes $H"
            [ "$result" -eq 75 ] && [ "$(grep -c '^full done ' "$work/state/record")" -eq 1 ] &&
                called '^git -C .* fetch' && ! called '^machine write\|^gh run\|^gh workflow' ;;
        void-waits)
            run 7 "$ended" 'ready: no: work: #14'
            [ "$result" -eq 75 ] && cmp -s "$work/before" "$work/state/record" &&
                [ -z "$(aside 7-)" ] && ! called '^machine\|^gh' ;;
        void-aside|failed-aside)
            case $1 in
                void-aside) run 7 "$ended" "ready: yes $C" ;;
                *) run 7 "$(printf '%s\n' "$ended" |
                    sed 's/^phase 7$/phase 6/; s/^void: .*/failed full: run 101 failed/')" "ready: yes $C" ;;
            esac
            a=$(aside "$(sed -n 's/^phase //p' "$work/before")-11111111-")
            [ "$result" -eq 75 ] && [ -n "$a" ] && cmp -s "$work/before" "$work/state/$a" &&
                [ "$(sed -n '1,2p; 4p' "$work/state/record")" = "phase 7
sha $C
touched $T" ] && [ "$(wc -l < "$work/state/record")" -eq 4 ] &&
                ! called '^machine write' ;;
        install-stops)
            run 7 "$installing
failed install: rach2-install ended 1: no space" "ready: yes $C"
            [ "$result" -eq 1 ] && cmp -s "$work/before" "$work/state/record" &&
                [ -z "$(aside 7-)" ] && [ ! -s "$work/calls" ] &&
                grep -q "delete the record's \`install started\` line" "$work/out" ;;
        install-reboot)
            run 7 "$installing" "ready: yes $H"
            [ "$result" -eq 1 ] &&
                [ "$(tail -n 1 "$work/state/record")" = 'failed install: the machine runs system candidate after the installer was started, so a reboot came before the release saw its result; check the root by hand' ] &&
                ! called '^machine write\|^board' ;;
        record-write)
            run 7 "$accepted" "ready: yes $H" OPEN yes
            a=$(aside 7-11111111-)
            [ "$result" -eq 0 ] && [ -n "$a" ] &&
                [ "$(cat "$work/calls.release")" = "commit $H
phase 7
full 101
root " ] && [ "$(grep -c '^record done ' "$work/state/$a")" -eq 1 ] &&
                [ "$(grep -n '^machine write release$' "$work/calls" | cut -d : -f 1)" -lt \
                    "$(grep -n '^gh issue close 7$' "$work/calls" | cut -d : -f 1)" ] ;;
        record-refused)
            run 7 "$accepted" "ready: yes $H"
            [ "$result" -eq 1 ] &&
                [ "$(tail -n 1 "$work/state/record")" = "failed record: /etc/rach2/release on the machine does not name $H: " ] &&
                ! grep -q '^reported \|^record done ' "$work/state/record" &&
                ! called '^gh issue close' ;;
        closing-other)
            run 7 "$(printf '%s\n' "$finished" | sed 's/^phase 7$/phase 6/')
reported $T
closing $T" "ready: yes $C" CLOSED
            a=$(aside 6-11111111-)
            [ "$result" -eq 0 ] && [ -n "$a" ] && [ ! -e "$work/state/record" ] &&
                [ "$(tail -n 1 "$work/state/$a" | cut -d ' ' -f 1)" = closed ] &&
                [ "$(grep '^board' "$work/calls" | tr '\n' ,)" = 'board done,board archive 6,' ] &&
                ! called '^gh issue close\|^gh issue comment\|^machine' ;;
        closing-open)
            run 7 "$finished
reported $T
closing $T" "ready: yes $H"
            a=$(aside 7-11111111-)
            [ "$result" -eq 0 ] && [ -n "$a" ] && [ "$(grep -c '^closing ' "$work/state/$a")" -eq 2 ] &&
                [ "$(grep '^board' "$work/calls" | tr '\n' ,)" = 'board phase,board done,board archive 7,' ] &&
                called '^gh issue close 7$' ;;
        closing-unready)
            run 7 "$finished
reported $T
closing $T" 'ready: no: work: #14'
            [ "$result" -eq 0 ] &&
                [ "$(tail -n 1 "$work/state/record")" = 'void: ready: no: work: #14' ] &&
                [ "$(grep '^board' "$work/calls" | tr '\n' ,)" = 'board phase,' ] &&
                called '^gh issue comment 7$' && ! called '^gh issue close' ;;
        *) return 1 ;;
    esac
}

# rows: run every row, naming each that fails.
rows()
{
    failed=
    for name in all-done full-done void-waits void-aside failed-aside \
        install-stops install-reboot record-write record-refused closing-other closing-open closing-unready; do
        if ! row "$name"; then
            echo "release-test: failed: $name" >&2
            cat "$work/out" "$work/calls" >&2
            failed="$failed $name"
        fi
    done
    [ -z "$failed" ]
}

# caught NAME ROW: put fault NAME into the copy and require row ROW to fail.
caught()
{
    fault "$1" || exit 1
    if rows > "$work/mutated.out" 2>&1; then
        echo "release-test: fault passed: $1" >&2
        exit 1
    fi
    grep -q "^release-test: failed: $2\$" "$work/mutated.out" || {
        echo "release-test: fault $1 did not fail $2" >&2
        exit 1
    }
}

fault none || exit 1
rows || exit 1
if [ "$mode" = mutate ]; then
    caught undone all-done
    caught full full-done
    caught aside void-waits
    caught touched void-aside
    caught started install-stops
    caught reboot install-reboot
    caught unrecorded record-write
    caught unread record-refused
    caught closing closing-other
    caught ready closing-unready
    fault none || exit 1
    rows || exit 1
    echo 'release-test: result: pass: 10 faults caught and restored'
fi
echo 'release-test: result: pass: 12 rows'
