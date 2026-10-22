#!/bin/sh
#
# release_resume_test.sh: release.sh resumes a release from its record, and
# keeps the contracts of its clean builds, its reruns, intermittent failures
# and the acceptance of the Lenovo.
#
# Each row writes a record to a scratch STATE and runs a private copy of
# tools/flow/release.sh, in a scratch tree, against stand-ins for gh, ssh,
# git, make, sleep, board.sh, notify.sh, testing.sh and metal_accept.sh that
# append each call to a list. The machine answers the reads of `where`,
# `bootctl status` and /etc/rach2/release, takes a write of that file as the
# row says and `bootctl commit`, and refuses every other command; the board
# says what the row gives; gh lists no runs unless the row says otherwise.
# The rows check that a release whose steps are all done only reports, closes
# the phase and removes its worktree, and one whose full run is done goes to the
# build without a second; that a void or failed record stays in place while the
# board gives no candidate, and is copied aside, keeping its `touched` line,
# once it gives one; that a record with `install started` and no `install done`
# stops the script before anything else, and that a machine running its system
# after the installer started fails the install without a write to the machine;
# that a release whose acceptance is done writes /etc/rach2/release before it
# closes the phase, and fails without closing it when the file does not read
# back or when the machine's flush of it fails though it reads back; and that a
# record with `closing` finishes that phase whatever phase was given, checking
# an open phase for readiness again; that a candidate whose only commits since
# a failed full run close the issues of its failures gets a run of only the
# suites that failed, while a commit that closes nothing, one that closes
# another issue, an architecture that failed with no issue, an issue still
# open, or a machine of the tests that differs from the failed commit's gives
# it a full run; that the same commit, given again while the issue of its
# failure is open as intermittent, gets a full run of its own and leaves the
# issue as it is; that a new candidate's run tests every suite, and its build
# for the Lenovo starts with `make clean`; and that metal_accept.sh passing on
# the trial boot leads to `bootctl commit` and a report that names the machine
# of the tests, while its failure ends the release with no commit.
#
# Usage: sh tests/system/release_resume_test.sh
#
set -u

[ "$#" -eq 0 ] || { echo 'release-test: usage: release_resume_test.sh' >&2; exit 2; }
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
trap 'rm -rf ${work:+"$work"}' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-release-test.XXXXXX") || exit 1
tree=$work/tree
mkdir -p "$work/bin" "$tree/tools/flow" "$tree/tools/ci" "$tree/krffs"
: > "$tree/krffs/rach2_demo_ed25519"
cp "$repo/tools/ci/site.sh" "$tree/tools/ci/site.sh" || exit 1

cp "$repo/tools/flow/release.sh" "$tree/tools/flow/release.sh" || exit 1

# board.sh prints RELEASE_TEST_BOARD for `phase` and succeeds otherwise.
cat > "$tree/tools/flow/board.sh" <<'EOF'
printf 'board %s\n' "$*" >> "$RELEASE_TEST_CALLS"
[ "$1" != phase ] || printf '%s\n' "$RELEASE_TEST_BOARD"
EOF
cat > "$tree/tools/flow/notify.sh" <<'EOF'
printf 'notify %s\n' "$*" >> "$RELEASE_TEST_CALLS"
EOF

# gh comments, keeping each comment's arguments in RELEASE_TEST_CALLS.comments,
# closes and gives the phase's state, RELEASE_TEST_STATE; gives
# an issue's state and title from the lines `N STATE TITLE` of
# RELEASE_TEST_ISSUES, and a commit's statuses from RELEASE_TEST_STATUSES;
# lists no run when RELEASE_TEST_RUNS is `none`, until a run is started, and
# then fails; keeps each `workflow run` whole in the calls, as `dispatch`;
# and fails every other query, which the script takes for a passing fault.
cat > "$work/bin/gh" <<'EOF'
#!/bin/sh
printf 'gh %s %s %s\n' "$1" "$2" "$3" >> "$RELEASE_TEST_CALLS"
case "$1 $2 $*" in
    'issue comment '*) printf '%s\n' "$*" >> "$RELEASE_TEST_CALLS.comments" ;;
    'issue close '*) ;;
    'issue view '*'--json state,title'*) printf '%s\n' "$RELEASE_TEST_ISSUES" | sed -n "s/^$3 //p" ;;
    'issue view '*'--json state'*) echo "$RELEASE_TEST_STATE" ;;
    'api repos/'*'/statuses'*) printf '%s\n' "$RELEASE_TEST_STATUSES" ;;
    'run list '*)
        [ "$RELEASE_TEST_RUNS" = none ] && ! grep -q '^dispatch ' "$RELEASE_TEST_CALLS" ;;
    'workflow run '*) printf 'dispatch %s\n' "$*" >> "$RELEASE_TEST_CALLS" ;;
    *) exit 1 ;;
esac
EOF

# ssh answers the testing host and the machine's reads, as the Lenovo
# running its system's candidate slot, takes `bootctl commit`, and refuses
# every other command but a write of /etc/rach2/release. RELEASE_TEST_WRITES
# says what that write does: `yes` keeps it in RELEASE_TEST_CALLS.release and
# reports status 0, `lost` reports status 0 and keeps nothing, `unsynced` keeps
# it and reports the flush's error and status 1, and `no` refuses it.
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
    'bootctl commit')
        printf 'machine write bootctl commit\n' >> "$RELEASE_TEST_CALLS"
        echo 'bootctl: committed: candidate' ;;
    'cat /etc/rach2/release 2> /dev/null; echo answered')
        printf 'machine read\n' >> "$RELEASE_TEST_CALLS"
        cat "$RELEASE_TEST_CALLS.release" 2> /dev/null
        echo answered ;;
    '{ mkdir -p /etc/rach2 && cat > /etc/rach2/release.new &&'*)
        [ "$RELEASE_TEST_WRITES" != no ] || {
            printf 'machine write refused\n' >> "$RELEASE_TEST_CALLS"
            exit 1
        }
        printf 'machine write release\n' >> "$RELEASE_TEST_CALLS"
        case $RELEASE_TEST_WRITES in
            lost) cat > /dev/null; echo 'status 0' ;;
            unsynced)
                cat > "$RELEASE_TEST_CALLS.release"
                echo "sync: error syncing '/etc/rach2/release': Input/output error"
                echo 'status 1' ;;
            *) cat > "$RELEASE_TEST_CALLS.release"; echo 'status 0' ;;
        esac ;;
    *)
        printf 'machine write %s\n' "$command" >> "$RELEASE_TEST_CALLS"
        exit 1 ;;
esac
EOF
# git fails, unless RELEASE_TEST_GIT is set: then the fetch, the ancestry
# check and the worktree's removal and pruning pass; a range between two
# commits has one commit, whose message is RELEASE_TEST_LOG, and the range
# from a commit to itself none; the registry has agents and bigmem on x86_64
# and fs on both architectures; a commit's tools/ci/site gives the runner
# label and the accelerator of its line `COMMIT LABEL ACCELERATOR` in
# RELEASE_TEST_SITES; and a new worktree has the stand-in for testing.sh.
cat > "$work/bin/git" <<'EOF'
#!/bin/sh
printf 'git %s\n' "$*" >> "$RELEASE_TEST_CALLS"
[ -n "$RELEASE_TEST_GIT" ] || exit 1
[ "$1" != -C ] || shift 2
case $1 in
    fetch|merge-base) ;;
    rev-list) [ "${3%%..*}" = "${3#*..}" ] || echo 3333333333333333333333333333333333333333 ;;
    log) printf '%s\n' "$RELEASE_TEST_LOG" ;;
    show)
        case $2 in
            *:tests/registry)
                printf '%s\n' '# the suites' 'agents|qemu|full||4096|x86_64' 'fs|qemu|full' \
                    'bigmem|qemu|full||73728|x86_64' ;;
            *:tools/ci/site)
                printf '%s\n' "$RELEASE_TEST_SITES" | awk -v c="${2%%:*}" '
                    $1 == c { print "runner " $2; print "accelerator " $3; found = 1 }
                    END { exit !found }' ;;
            *) exit 1 ;;
        esac ;;
    worktree)
        [ "$2" != add ] || {
            mkdir -p "$5/tools/dev" && cp "${RELEASE_TEST_CALLS%/*}/testing.sh" "$5/tools/dev/testing.sh"
        } ;;
    *) exit 1 ;;
esac
EOF

# testing.sh, in a worktree that git makes, keeps the copy it was given and
# its arguments in the calls, and says that the testing host did not answer.
cat > "$work/testing.sh" <<'EOF'
printf 'testing %s %s\n' "$RACH2_TESTING_COPY" "$*" >> "$RELEASE_TEST_CALLS"
echo 'testing: the host did not answer'
exit 1
EOF

# metal_accept.sh, in the worktree of every row, keeps its arguments in the
# calls, and passes when RELEASE_TEST_ACCEPT is `pass` and fails otherwise.
cat > "$work/metal_accept.sh" <<'EOF'
printf 'accept %s\n' "$*" >> "$RELEASE_TEST_CALLS"
if [ "$RELEASE_TEST_ACCEPT" = pass ]; then
    echo 'metal-accept: PASS'
else
    echo '  FAIL: the hub section found no hub'
    echo 'metal-accept: FAIL (1)'
    exit 1
fi
EOF

# make builds nothing and keeps its arguments in the calls.
cat > "$work/bin/make" <<'EOF'
#!/bin/sh
printf 'make %s\n' "$*" >> "$RELEASE_TEST_CALLS"
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
issues='' statuses='' runs='' log='' git='' accept=''
sites="$H rach2 tcg
$C rach2 tcg"

# The records of the rows. `head` has a candidate whose full run passed,
# `built` adds every step up to the transfer, `installing` the installer's
# start, `tried` every step up to the trial boot, `accepted` the acceptance,
# and `finished` the installed record.
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
tried="$installing
install done $T root r
trial done $T the new kernel runs from the candidate slot"
accepted="$tried
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
# machine take a write of /etc/rach2/release as WRITES says, no unless given,
# as for ssh above, and run the copy for phase P, keeping its output in out
# and its status in `result`. The variables issues, statuses, runs, log, git,
# sites and accept are RELEASE_TEST_ISSUES, RELEASE_TEST_STATUSES,
# RELEASE_TEST_RUNS, RELEASE_TEST_LOG, RELEASE_TEST_GIT, RELEASE_TEST_SITES
# and RELEASE_TEST_ACCEPT. The worktree has the stand-in for metal_accept.sh
# and a tools/ci/site of `runner rach2` and `accelerator tcg`.
run()
{
    rm -rf "$work/state" "$work/calls" "$work/calls.sleeps" "$work/calls.release" "$work/calls.comments"
    mkdir -p "$work/state/wt/krffs" "$work/state/wt/build/x86_64/images" \
        "$work/state/wt/tests/system/accept" "$work/state/wt/tools/ci"
    : > "$work/state/wt/krffs/rach2_demo_ed25519"
    : > "$work/state/wt/build/x86_64/images/release.sha256"
    cp "$work/metal_accept.sh" "$work/state/wt/tests/system/accept/metal_accept.sh"
    printf '%s\n' 'runner rach2' 'accelerator tcg' > "$work/state/wt/tools/ci/site"
    : > "$work/calls"
    printf '%s\n' "$2" > "$work/state/record"
    cp "$work/state/record" "$work/before"
    result=0
    PATH=$work/bin:$PATH RELEASE_TEST_CALLS=$work/calls \
        RELEASE_TEST_BOARD="phase: #7 phase: seven
$3" RELEASE_TEST_STATE=${4:-OPEN} RELEASE_TEST_WRITES=${5:-no} \
        RELEASE_TEST_ISSUES=$issues RELEASE_TEST_STATUSES=$statuses \
        RELEASE_TEST_RUNS=$runs RELEASE_TEST_LOG=$log RELEASE_TEST_GIT=$git \
        RELEASE_TEST_SITES=$sites RELEASE_TEST_ACCEPT=$accept \
        sh "$tree/tools/flow/release.sh" -s "$work/state" "$1" > "$work/out" 2>&1 < /dev/null ||
        result=$?
}

# aside PREFIX: print the names of the records copied aside that begin PREFIX.
aside()
{
    (cd "$work/state" && ls) | grep "^$1" | grep -v '\.sha256$'
}

# failed_at LOG STATUSES [STATE]: run a new candidate C after a record whose
# full run 101 failed at H, with #20 filed for agents and bigmem on x86_64 and
# in STATE, CLOSED unless given, the commit since H having the message LOG and
# run 101 having set STATUSES on H.
failed_at()
{
    issues="20 ${3:-CLOSED} Release tests fail on x86_64: agents, bigmem"
    statuses=$2 log=$1 git=yes
    run 7 "$(printf '%s\n' "$ended" | sed '$d')
filed 101 #20
failed full: run 101 failed; ci filed its failures as #20" "ready: yes $C"
    issues='' statuses='' log='' git=''
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
                [ -f "$work/state/$a.sha256" ] && called '^git worktree remove --force .*/state/wt$' &&
                ! called '^machine\|^ssh\|^git -C\|^gh run\|^gh workflow' ;;
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
            run 7 "$accepted" "ready: yes $H" OPEN lost
            [ "$result" -eq 1 ] &&
                [ "$(tail -n 1 "$work/state/record")" = "failed record: /etc/rach2/release on the machine does not name $H: " ] &&
                ! grep -q '^reported \|^record done ' "$work/state/record" &&
                ! called '^gh issue close' ;;
        record-unsynced)
            run 7 "$accepted" "ready: yes $H" OPEN unsynced
            [ "$result" -eq 1 ] &&
                [ "$(tail -n 1 "$work/state/record")" = "failed record: writing /etc/rach2/release failed: sync: error syncing '/etc/rach2/release': Input/output error status 1" ] &&
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
        rerun-chosen)
            failed_at "Fix agents

Closes #20" 'ci/full/i686 success
ci/full/x86_64 failure'
            [ "$result" -eq 75 ] &&
                [ "$(sed -n 4p "$work/state/record")" = "rerun 101 $H x86_64:agents x86_64:bigmem" ] ;;
        rerun-other|rerun-unfixed|rerun-unfiled|rerun-open|rerun-moved)
            case $1 in
                rerun-other) failed_at "Fix agents" 'ci/full/i686 success
ci/full/x86_64 failure' ;;
                rerun-unfixed) failed_at "Fix agents

Closes #20
Closes #30" 'ci/full/i686 success
ci/full/x86_64 failure' ;;
                rerun-unfiled) failed_at "Fix agents

Closes #20" 'ci/full/i686 failure
ci/full/x86_64 failure' ;;
                rerun-open) failed_at "Fix agents

Closes #20" 'ci/full/i686 success
ci/full/x86_64 failure' OPEN ;;
                *)
                    sites="$H rach2 tcg
$C rach2-mac kvm"
                    failed_at "Fix agents

Closes #20" 'ci/full/i686 success
ci/full/x86_64 failure'
                    sites="$H rach2 tcg
$C rach2 tcg" ;;
            esac
            [ "$result" -eq 75 ] && [ "$(sed -n 2p "$work/state/record")" = "sha $C" ] &&
                ! grep -q '^rerun ' "$work/state/record" ;;
        rerun-dispatch)
            runs=none
            run 7 "phase 7
sha $C
chosen $T
rerun 101 $H x86_64:agents
touched $T" "ready: yes $C"
            runs=''
            [ "$result" -eq 75 ] &&
                [ "$(grep '^dispatch ' "$work/calls")" = "dispatch workflow run full.yml --repo toksaitov/rach2 --ref master -f sha=$C -f title=Release tests of 22222222 for phase #7: again only agents on x86_64 -f suites=x86_64:agents" ] ;;
        intermittent)
            issues='20 OPEN Release tests fail on x86_64: agents, bigmem' git=yes runs=none
            run 7 "phase 7
sha $H
chosen $T
touched $T
dispatched 1760054400
run 101 failure
filed 101 #20
failed full: run 101 failed; ci filed its failures as #20" "ready: yes $H"
            issues='' git='' runs=''
            a=$(aside 7-11111111-)
            [ "$result" -eq 75 ] && [ -n "$a" ] && cmp -s "$work/before" "$work/state/$a" &&
                [ "$(sed -n '1,2p; 4p' "$work/state/record")" = "phase 7
sha $H
touched $T" ] && ! grep -q '^rerun \|^run ' "$work/state/record" &&
                [ "$(grep '^dispatch ' "$work/calls")" = "dispatch workflow run full.yml --repo toksaitov/rach2 --ref master -f sha=$H -f title=Release tests of 11111111 for phase #7" ] &&
                ! called '^gh issue close\|^gh issue edit\|^gh issue reopen' ;;
        full-every)
            runs=none
            run 7 "phase 7
sha $C
chosen $T
touched $T" "ready: yes $C"
            runs=''
            [ "$result" -eq 75 ] &&
                [ "$(grep '^dispatch ' "$work/calls")" = "dispatch workflow run full.yml --repo toksaitov/rach2 --ref master -f sha=$C -f title=Release tests of 22222222 for phase #7" ] ;;
        build-clean)
            git=yes
            run 7 "$head" "ready: yes $H"
            git=''
            [ "$result" -eq 75 ] && ! grep -q '^build done ' "$work/state/record" &&
                called '^testing /home/testing/rach2-clean run ARCH=x86_64 make clean && ARCH=x86_64 make machine GNU_AGENTS=all && ' ;;
        accept-pass)
            accept=pass
            run 7 "$tried" "ready: yes $H" OPEN yes
            accept=''
            a=$(aside 7-11111111-)
            # shellcheck disable=SC2016 # the backquotes are Markdown
            [ "$result" -eq 0 ] && [ -n "$a" ] &&
                [ "$(grep '^accept \|^machine write\|^gh issue close' "$work/calls" | tr '\n' ,)" = \
                    'accept 192.168.0.106,machine write bootctl commit,machine write release,gh issue close 7,' ] &&
                grep -q '^accept done .* metal-accept: PASS; committed$' "$work/state/$a" &&
                grep -q '^\* The release tests ran on the one machine that the commit.s `tools/ci/site` selects, whose runners have the label `rach2`, with QEMU.s accelerator `tcg`\.$' \
                    "$work/calls.comments" ;;
        accept-fail)
            accept=fail
            run 7 "$tried" "ready: yes $H" OPEN yes
            accept=''
            [ "$result" -eq 1 ] &&
                tail -n 1 "$work/state/record" | grep -q '^failed accept: metal_accept.sh failed: .*FAIL: the hub section found no hub' &&
                called '^accept 192.168.0.106$' && ! called '^machine write\|^gh issue close' ;;
        *) return 1 ;;
    esac
}

# rows: run every row, naming each that fails.
rows()
{
    failed=
    for name in all-done full-done void-waits void-aside failed-aside \
        install-stops install-reboot record-write record-refused record-unsynced closing-other closing-open closing-unready \
        rerun-chosen rerun-other rerun-unfixed rerun-unfiled rerun-open rerun-moved rerun-dispatch intermittent \
        full-every build-clean accept-pass accept-fail; do
        if ! row "$name"; then
            echo "release-test: failed: $name" >&2
            cat "$work/out" "$work/calls" >&2
            failed="$failed $name"
        fi
    done
    [ -z "$failed" ]
}

rows || exit 1
echo 'release-test: result: pass: 25 rows'
