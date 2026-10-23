#!/bin/sh
#
# host_suite_test.sh: the host cases run their checks at once, and every check
# a case selects gives a result and an assertion: tests/cases/host/host.sh
# and etude-host, run for the cases tests, checkers and scripts in a private
# tree whose tests/host has fixture tests, whose host.list has fixture
# scripts, and whose manifest names the checks.
#
# Eight tests each make a file of one name in their TMPDIR, refusing one that
# is there, and then wait until all eight have started, so they pass only
# when the suite runs them at once, each in a scratch directory of its own.
# A test with an AddressSanitizer fault and one with an undefined signed
# overflow must fail while the caller's environment tells the sanitizers to
# exit 0 and to go on after an error, and so must a leak where LeakSanitizer
# runs, on Linux. A test with a missing `host-deps:` source fails and names
# it; a failing test that left a file keeps its scratch directory and names
# it; a test repeats its `not run:` line under its PASS; and a test finds
# SIGINT and SIGQUIT at their defaults. Two `host-alone:` tests find no other
# check's scratch directory beside theirs, though a test before them waits
# up to ten seconds for one to start. Every check of a selection has one
# result line, each followed by the report record `progress checks N`, N
# counting the checks ended, the last line of etude-host's counts them, and
# no scratch is left but the kept directory. Each check has an assertion of
# its outcome in the case's report; a test the manifest does not name is not
# run and fails `listed`, and so does an assertion the tree has no test for,
# which fails too. A selection with a name given twice is refused and runs
# nothing. A script that never ends is ended only by SIGTERM sent to the
# process group of host.sh, which then exits 143 with the script's process
# gone and its private directory removed.
#
# Usage: sh tests/system/host_suite_test.sh
#
set -u

[ $# -eq 0 ] || { echo 'host-suite-test: usage: host_suite_test.sh' >&2; exit 2; }
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

made=$(mktemp -d "${TMPDIR:-/tmp}/rach2-host-suite-test.XXXXXX") || exit 1
work=$(cd "$made" && pwd -P) || exit 1
tree=$work/tree
mkdir -p "$tree/tests/host" "$tree/tests/cases/host" "$tree/build/.run.lock" \
    "$tree/build/tools/etude" "$tree/tools/inspect" "$tree/tools/verify" \
    "$tree/tools/etude/win" "$work/meet" "$work/tmp" || exit 1
cp "$repo/tests/cases/host/host.sh" "$tree/tests/cases/host/" || exit 1
cp "$repo/tools/etude/hold.sh" "$tree/tools/etude/" || exit 1
# The tree is held for this script, as tests/run holds it for the cases.
echo "pid $$" > "$tree/build/.run.lock/owner"

# The checkers whose selftests the checkers case compiles: each passes its
# selftest.
for c in tools/inspect/elfcheck.c tools/inspect/cfgcheck.c tools/inspect/setidcheck.c \
    tools/verify/prosecheck.c tools/etude/win/pecheck.c; do
    printf '#include <string.h>\nint main(int c, char **v) { return !(c == 2 && strcmp(v[1], "--selftest") == 0); }\n' \
        > "$tree/$c"
done

# meet_hosttest.c, compiled as eight tests: a file of one name in TMPDIR, and
# a wait for all eight in the meeting directory, for at most 30 seconds.
cat > "$work/meet.c" <<'EOF'
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

static int count(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;

    while (d != NULL && (e = readdir(d)) != NULL)
        n += e->d_name[0] != '.';
    if (d != NULL)
        closedir(d);
    return n;
}

int main(void)
{
    const char *tmp = getenv("TMPDIR"), *meet = getenv("HOST_SUITE_MEET");
    char path[4096];
    struct timespec tick = {0, 100000000};
    int fd, tries;

    snprintf(path, sizeof(path), "%s/mine", tmp != NULL ? tmp : "/nonexistent");
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        printf("meet: %s: another check has it\n", path);
        return 1;
    }
    close(fd);
    snprintf(path, sizeof(path), "%s/%ld", meet, (long)getpid());
    fd = open(path, O_WRONLY | O_CREAT, 0600);
    close(fd);
    for (tries = 0; count(meet) < 8; tries++) {
        if (tries == 300) {
            printf("meet: %d of 8 checks started\n", count(meet));
            return 1;
        }
        nanosleep(&tick, NULL);
    }
    printf("meet: all 8 checks running\n");
    return 0;
}
EOF
for i in 1 2 3 4 5 6 7 8; do
    cp "$work/meet.c" "$tree/tests/host/meet${i}_hosttest.c" || exit 1
done

cat > "$tree/tests/host/asan_hosttest.c" <<'EOF'
#include <stdlib.h>
int main(void)
{
    volatile char *p = malloc(4);
    p[4] = 1;
    return 0;
}
EOF
cat > "$tree/tests/host/ubsan_hosttest.c" <<'EOF'
#include <limits.h>
int main(int argc, char **argv)
{
    int x = INT_MAX - 1 + (argv[0] != 0);
    x += argc;
    return x == 0;
}
EOF
leak=
if [ "$(uname -s)" = Linux ]; then
    leak=leak_hosttest
    cat > "$tree/tests/host/leak_hosttest.c" <<'EOF'
#include <stdlib.h>
void *volatile lost;
int main(void)
{
    lost = malloc(64);
    lost = 0;
    return 0;
}
EOF
fi
cat > "$tree/tests/host/gone_hosttest.c" <<'EOF'
/* host-deps: tests/host/gone.c */
int main(void)
{
    return 0;
}
EOF
cat > "$tree/tests/host/keep_hosttest.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
int main(void)
{
    char path[4096];
    FILE *f;

    snprintf(path, sizeof(path), "%s/evidence", getenv("TMPDIR"));
    f = fopen(path, "w");
    if (f != NULL)
        fclose(f);
    printf("keep: failing after writing %s\n", path);
    return 1;
}
EOF
cat > "$tree/tests/host/signals_hosttest.c" <<'EOF'
#include <signal.h>
#include <stdio.h>
int main(void)
{
    struct sigaction a, b;
    sigset_t mask;

    sigaction(SIGINT, NULL, &a);
    sigaction(SIGQUIT, NULL, &b);
    sigprocmask(SIG_BLOCK, NULL, &mask);
    printf("signals_hosttest: not run: nothing: this line is repeated under the pass\n");
    return a.sa_handler != SIG_DFL || b.sa_handler != SIG_DFL || sigismember(&mask, SIGINT);
}
EOF
# alone_hosttest.c, compiled as two tests that run alone: no scratch directory
# but their own and keep_hosttest's kept one is beside theirs.
cat > "$work/alone.c" <<'EOF'
/* host-alone: it must find no other check running */
#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char tmp[4096], path[4096], own[4096];
    struct dirent *e;
    DIR *d;
    int others = 0;

    snprintf(path, sizeof(path), "%s/alone", getenv("HOST_SUITE_MEET"));
    close(open(path, O_WRONLY | O_CREAT, 0600));
    snprintf(tmp, sizeof(tmp), "%s", getenv("TMPDIR"));
    snprintf(own, sizeof(own), "%s", strrchr(tmp, '/') + 1);
    d = opendir(dirname(tmp));
    while (d != NULL && (e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.' && strcmp(e->d_name, own) != 0 &&
            strcmp(e->d_name, "keep_hosttest") != 0) {
            printf("alone: %s runs beside it\n", e->d_name);
            others++;
        }
    }
    if (d != NULL)
        closedir(d);
    return d == NULL || others != 0;
}
EOF
cp "$work/alone.c" "$tree/tests/host/alone1_hosttest.c" || exit 1
cp "$work/alone.c" "$tree/tests/host/alone2_hosttest.c" || exit 1
# A test that waits for an alone test to start, for at most ten seconds, and
# passes either way: an alone test started beside it finds it.
cat > "$tree/tests/host/linger_hosttest.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    char path[4096];
    struct timespec tick = {0, 100000000};
    int tries;

    snprintf(path, sizeof(path), "%s/alone", getenv("HOST_SUITE_MEET"));
    for (tries = 0; tries < 100 && access(path, F_OK) != 0; tries++)
        nanosleep(&tick, NULL);
    return 0;
}
EOF
# A test the manifest does not name, which fails if it is run.
printf 'int main(void)\n{\n    return 1;\n}\n' > "$tree/tests/host/unlisted_hosttest.c" || exit 1
cat > "$tree/tests/cases/host/host.list" <<'EOF'
# The fixture's scripted checks.
script_pass|a script that passes|test -d "$TMPDIR" && echo fine
script_fail|a script that fails|echo 'script: it failed'; exit 4
EOF

# A second tree, whose scripts case has one script, which writes its process
# number to the meeting directory and then never ends.
stop=$work/stop
mkdir -p "$stop/tests/cases/host" "$stop/build/.run.lock" \
    "$stop/build/tools/etude" "$stop/tools/etude" || exit 1
cp "$repo/tests/cases/host/host.sh" "$stop/tests/cases/host/" || exit 1
cp "$repo/tools/etude/hold.sh" "$stop/tools/etude/" || exit 1
echo "pid $$" > "$stop/build/.run.lock/owner"
cat > "$stop/tests/cases/host/host.list" <<'EOF'
script_stuck|a script that never ends|echo $$ > "$HOST_SUITE_MEET/stuck"; exec tail -f /dev/null
EOF
printf '{"cases": [{"case_id": "host.host.scripts", "assertions": [{"assertion_id": "listed"}, {"assertion_id": "script_stuck"}]}]}\n' \
    > "$stop/tests/cases/host.host.json" || exit 1

# The fixture's manifest, with the members the program reads: each test but
# unlisted_hosttest.c, and ghost, which the tree does not have.
{
    printf '{"cases": [\n  {"case_id": "host.host.tests", "assertions": [{"assertion_id": "listed"}'
    for t in "$tree"/tests/host/*_hosttest.c; do
        t=$(basename "$t" _hosttest.c)
        [ "$t" = unlisted ] || printf ', {"assertion_id": "%s"}' "$t"
    done
    printf ', {"assertion_id": "ghost"}]},\n  {"case_id": "host.host.checkers", "assertions": [{"assertion_id": "listed"}'
    for c in elfcheck cfgcheck setidcheck prosecheck pecheck; do
        printf ', {"assertion_id": "%s"}' "$c"
    done
    printf ']},\n  {"case_id": "host.host.scripts", "assertions": [{"assertion_id": "listed"}'
    printf ', {"assertion_id": "script_pass"}, {"assertion_id": "script_fail"}]}\n]}\n'
} > "$tree/tests/cases/host.host.json" || exit 1

# run_case CASE: run the private tree's case CASE, keeping its output in
# $work/out.CASE and its status in $status_CASE. The variables of the case
# that runs this script are not the fixture's.
run_case()
{
    rc=0
    (unset ETUDE_CASE ARCH
     HOST_SUITE_MEET=$work/meet RACH2_TREE_HELD=$$ RACH2_WORKERS=16 TMPDIR=$work/tmp ETUDE_TREE=$tree \
         ASAN_OPTIONS=exitcode=0:halt_on_error=0:abort_on_error=0:detect_leaks=0 \
         UBSAN_OPTIONS=exitcode=0:halt_on_error=0:abort_on_error=0 LSAN_OPTIONS=exitcode=0 \
         exec sh "$tree/tests/cases/host/host.sh" "$1") > "$work/out.$1" 2>&1 < /dev/null ||
        rc=$?
}

# run_stop: run the second tree's scripts case in a process group of its
# own, send the group SIGTERM once the script has started, and keep the
# output in $work/out.stop and the status in $status_stop, or 1 when the case
# ended before its script started.
run_stop()
{
    rm -f "$work/meet/stuck"
    status_stop=0
    (unset ETUDE_CASE ARCH
     HOST_SUITE_MEET=$work/meet RACH2_TREE_HELD=$$ TMPDIR=$work/tmp ETUDE_TREE=$stop \
         exec python3 -I - "$stop/tests/cases/host/host.sh" "$work/meet/stuck" <<'PY'
import os
import signal
import sys
import time

host, mark = sys.argv[1:]
pid = os.fork()
if pid == 0:
    os.setsid()
    os.dup2(os.open(os.devnull, os.O_RDONLY), 0)
    os.execvp('sh', ['sh', host, 'scripts'])
while not os.path.exists(mark):
    if os.waitpid(pid, os.WNOHANG)[0] == pid:
        sys.exit(1)
    time.sleep(0.05)
os.killpg(pid, signal.SIGTERM)
status = os.waitpid(pid, 0)[1]
sys.exit(os.WEXITSTATUS(status) if os.WIFEXITED(status) else 128 + os.WTERMSIG(status))
PY
    ) > "$work/out.stop" 2>&1 < /dev/null || status_stop=$?
}

# run_suite DRIVER: run the three cases and the stopped one with DRIVER,
# keeping the output of the three together in $work/out.
run_suite()
{
    cp "$1" "$tree/build/tools/etude/etude-host" || exit 1
    cp "$1" "$stop/build/tools/etude/etude-host" || exit 1
    rm -rf "$work/meet" && mkdir "$work/meet" || exit 1
    run_case tests
    status_tests=$rc
    run_case checkers
    status_checkers=$rc
    run_case scripts
    status_scripts=$rc
    cat "$work/out.tests" "$work/out.checkers" "$work/out.scripts" > "$work/out"
    run_stop
}

# line PATTERN: succeed when one line of the output matches PATTERN, an
# extended regular expression for the whole line.
line()
{
    [ "$(grep -cE "^$1\$" "$work/out")" -eq 1 ]
}

# miss WHAT: report an expectation the last run missed.
miss()
{
    echo "host-suite-test: failed: $1"
    bad=1
}

# results CASE N: succeed when the case CASE has N result lines, the last
# line of etude-host's counts N checks, and its report ends with the count of
# its assertions.
results()
{
    [ "$(grep -cE '^  (PASS|FAIL)' "$work/out.$1")" -eq "$2" ] &&
        grep '^etude-host: result: ' "$work/out.$1" | tail -n 1 |
        grep -qE "^etude-host: result: (pass: $2 checks|fail: [0-9]+ of $2 checks:.*)\$" &&
        grep -qx "@etude 1 end $(grep -c '^@etude 1 assert ' "$work/out.$1")" "$work/out.$1"
}

# progressed CASE N: succeed when each of the N result lines of the case CASE
# is followed, before the next, by the record `progress checks K`, K counting
# from 1 to N.
progressed()
{
    awk -v want="$2" '
        /^  (PASS|FAIL)/ { if (pending) bad = 1; pending = 1; next }
        /^@etude 1 progress / {
            n++
            if (!pending || $4 != "checks" || $5 != n || NF != 5) bad = 1
            pending = 0
        }
        END { exit bad || pending || n != want }
    ' "$work/out.$1"
}

# judge: print `host-suite-test: failed: WHAT` for each expectation the last
# run missed, and fail when any did.
judge()
{
    bad=0
    tests=$(find "$tree/tests/host" -name '*_hosttest.c' | wc -l | tr -d ' ')
    [ "$status_tests" -eq 1 ] || miss 'the tests case fails'
    [ "$status_checkers" -eq 0 ] || miss 'the checkers case passes'
    [ "$status_scripts" -eq 1 ] || miss 'the scripts case fails'
    for i in 1 2 3 4 5 6 7 8; do
        line "  PASS: meet${i}_hosttest" || miss "meet$i passes beside the others in its own scratch"
        line "@etude 1 assert meet$i pass" || miss "meet$i's assertion passes"
    done
    for i in 1 2; do
        line "  PASS: alone${i}_hosttest" || miss "alone$i runs with no other check"
        line "@etude 1 assert alone$i pass" || miss "alone$i's assertion passes"
    done
    line '  PASS: linger_hosttest' || miss 'the test before the alone ones passes'
    for t in asan ubsan ${leak%_hosttest}; do
        line "  FAIL \\(run, [^)]*\\): ${t}_hosttest" || miss "$t fails"
        line "@etude 1 assert $t fail run, .*" || miss "$t's assertion fails"
    done
    grep -q '^    ! .*AddressSanitizer' "$work/out" || miss 'a sanitizer report is shown'
    line '  FAIL \(run, status 1; see .*\): gone_hosttest' || miss 'a missing source fails its test'
    line '    \| host: gone_hosttest: missing host-deps: tests/host/gone.c' ||
        miss 'a missing source is named'
    line '  FAIL \(run, status 4; see .*\): a script that fails' || miss 'a failing script fails'
    line '@etude 1 assert script_fail fail .*' || miss "a failing script's assertion fails"
    line '  PASS: a script that passes' || miss 'a passing script passes'
    line '@etude 1 assert script_pass pass' || miss "a passing script's assertion passes"
    line '  PASS: signals_hosttest' || miss 'the signals are at their defaults'
    line '    signals_hosttest: not run: nothing: this line is repeated under the pass' ||
        miss 'the not-run line is repeated'
    [ "$(grep -c '^  PASS: [a-z]* selftest under the sanitizers$' "$work/out")" -eq 5 ] ||
        miss 'each selftest passes'
    [ "$(grep -c '^@etude 1 assert [a-z]* pass$' "$work/out.checkers")" -eq 6 ] ||
        miss 'each selftest and the list of checkers pass'
    ! grep -q 'unlisted_hosttest$' "$work/out" || miss 'a test the manifest does not name is not run'
    line '@etude 1 assert listed fail checks the case does not name: unlisted; checks the tree does not have: ghost' ||
        miss 'a test the manifest does not name, and a test the tree does not have, fail the list'
    line '@etude 1 assert ghost fail the tree has no such check.*' || miss 'a test the tree does not have fails'
    results tests $((tests - 1)) || miss 'every test named has one result and one assertion'
    results checkers 5 || miss 'every checker has one result and one assertion'
    results scripts 2 || miss 'every script has one result and one assertion'
    progressed tests $((tests - 1)) || miss 'every test that ends gives a record of progress'
    progressed checkers 5 || miss 'every checker that ends gives a record of progress'
    progressed scripts 2 || miss 'every script that ends gives a record of progress'
    [ "$status_stop" -eq 143 ] || miss 'the stopped case exits 143'
    grep -qx '  STOPPED: a script that never ends' "$work/out.stop" ||
        miss 'the endless script is reported stopped'
    grep -qx 'etude-host: result: stopped by signal 15: 0 of 1 checks finished' "$work/out.stop" ||
        miss 'etude-host reports the stop'
    ! grep -q '^@etude 1 progress ' "$work/out.stop" || miss 'a stopped check gives no record of progress'
    stuck=$(cat "$work/meet/stuck" 2>/dev/null)
    if [ -z "$stuck" ] || kill -0 "$stuck" 2>/dev/null; then
        miss "the endless script's process is gone"
    fi
    logs=$(sed -n 's/^host: logs: //p' "$work/out.stop")
    [ -n "$logs" ] && [ ! -e "$logs" ] || miss "the stopped case's private directory is removed"
    kept=$(sed -n 's/^    scratch kept: //p' "$work/out")
    case $kept in
        */keep_hosttest) [ -f "$kept/evidence" ] || miss 'the failed test keeps its scratch' ;;
        *) miss 'the failed test keeps its scratch' ;;
    esac
    left=$(find "$work"/tmp/rach2-host.*/scratch -mindepth 1 -maxdepth 1 ! -name keep_hosttest 2>/dev/null |
        wc -l | tr -d ' ')
    [ "$left" -eq 0 ] || miss 'no other scratch is left'
    [ -z "$kept" ] || rm -rf "$kept"
    rm -rf "$work"/tmp/rach2-host.*
    return $bad
}

# refused DRIVER: a selection with a name given twice is refused before any
# check runs.
refused()
{
    printf 'twice\tshared\tthe first\t\ttouch %s/ran\ntwice\tshared\tthe second\t\ttrue\n' \
        "$work" > "$work/twice"
    rc=0
    "$1" -j 2 -l "$work" -s "$work/tmp" "$work/twice" > "$work/twice.out" 2>&1 || rc=$?
    if [ "$rc" -ne 2 ] || [ -e "$work/ran" ]; then
        echo 'host-suite-test: failed: a name given twice refuses the selection'
        return 1
    fi
}

driver=${ETUDE_HOST:-$repo/build/tools/etude/etude-host}
[ -x "$driver" ] || { echo "host-suite-test: $driver is not built"; exit 1; }
run_suite "$driver"
if ! judge || ! refused "$driver"; then
    cat "$work/out" "$work/out.stop"
    exit 1
fi
echo "host-suite-test: result: pass: every check selected gave one result"
