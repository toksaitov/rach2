#!/bin/sh
# Prove the netcfg_io, dhcpc, netup, ncport and httpd host tests with private
# mutations of utils/netcfg.c, dhcpc.c, netup.c, nc.c and httpd.c.
#
# Usage: sh tests/host/netutil_mutate.sh [FAULT ...] | --texts
# With no operand every fault is proved; an operand proves only the ones named.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault DIR FAULT: put FAULT into its source in DIR, a copy of utils/;
# with RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

root = Path(sys.argv[1])
fault = sys.argv[2]
changes = {
    "read-first": ("netcfg.c", "if (ferror(f)) {", "if (pass == 1 && ferror(f)) {"),
    "read-second": ("netcfg.c", "if (ferror(f)) {", "if (pass == 0 && ferror(f)) {"),
    "seek": ("netcfg.c", "pass == 0 && fseek(f, 0, SEEK_SET) != 0", "pass == 0 && (fseek(f, 0, SEEK_SET), 0)"),
    "cleanup": ("dhcpc.c", 'strerror(errno));\n        return -1;\n    }\n    holding = 0;',
                'strerror(errno));\n    }\n    holding = 0;'),
    "release": ("dhcpc.c", "if (holds_interface && holding == 0) {", "if (holds_interface) {"),
    "expiry": ("dhcpc.c", '"dhcpc: the lease ran out; giving the address up\\n");\n            return unconfigure(fd);',
               '"dhcpc: the lease ran out; giving the address up\\n");\n            unconfigure(fd);\n            return 0;'),
    "mask": ("dhcpc.c", "if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0) {",
             "if (l->netmask && ioctl(fd, SIOCSIFNETMASK, &ifr) < 0) {"),
    "resolver": ("dhcpc.c", 'snprintf(tmp, sizeof(tmp), "%s.XXXXXX", RESOLV_CONF);\n    fd = mkstemp(tmp);',
                 'snprintf(tmp, sizeof(tmp), "%s.new", RESOLV_CONF);\n    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);'),
    "claim-open": ("dhcpc.c", "*present = f != NULL || errno != ENOENT;", "*present = f != NULL;"),
    "claim-read": ("dhcpc.c", "if (ferror(f)) {", "if (0 && ferror(f)) {"),
    "holder-open": ("netup.c", "return errno == ENOENT ? 0 : -1;", "return 0;"),
    "holder-read": ("netup.c", 'fscanf(f, "%ld", &pid) != 1 || ferror(f)', 'fscanf(f, "%ld", &pid) != 1'),
    "detach": ("dhcpc.c", 'for (i = 0; i < 3; i++) {\n        close(i);\n    }\n    fd = open',
               'fd = open'),
    "dup": ("dhcpc.c", "if (dup2(fd, i) < 0) {", "if ((dup2(fd, i), 0)) {"),
    "receive": ("dhcpc.c",
                "        if (n == 0) {\n            continue;\n        }\n"
                "        if (!(p.revents & POLLIN)) {",
                "        if (n == 0 || !(p.revents & POLLIN)) {"),
    "dhcp-status": ("netup.c",
                    "    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {\n"
                    "        return 0;\n"
                    "    }\n"
                    "    if (WIFEXITED(status)) {\n"
                    "        fprintf(stderr, \"netup: %s: dhcpc exited with status %d before "
                    "reaching the background\\n\",\n"
                    "                s->name, WEXITSTATUS(status));\n"
                    "    } else {\n"
                    "        fprintf(stderr, \"netup: %s: dhcpc was killed by signal %d before "
                    "reaching the background\\n\",\n"
                    "                s->name, WTERMSIG(status));\n"
                    "    }\n"
                    "    return -1;\n",
                    "    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {\n"
                    "        fprintf(stderr, \"netup: %s: dhcpc did not configure the "
                    "interface\\n\", s->name);\n"
                    "        return -1;\n"
                    "    }\n"
                    "    return 0;\n"),
    "nc-port": ("nc.c",
                "    long p = 0;\n"
                "\n"
                "    if (*s == '\\0') {\n"
                "        return 0;\n"
                "    }\n"
                "    while (*s >= '0' && *s <= '9') {\n"
                "        p = p * 10 + (*s - '0');\n"
                "        if (p > 65535) {\n"
                "            return 0;\n"
                "        }\n"
                "        s++;\n"
                "    }\n"
                "    return *s == '\\0' ? (int)p : 0;\n",
                "    int p = 0;\n"
                "\n"
                "    while (*s >= '0' && *s <= '9') {\n"
                "        p = p * 10 + (*s - '0');\n"
                "        s++;\n"
                "    }\n"
                "    return p;\n"),
    "httpd-page": ("httpd.c",
                   "    put_s(&o, \"<div class=top><h1>rach2</h1><div class=stat>\");\n"
                   "    put_ip(&o, cur_server_ip);\n"
                   "    put_c(&o, ':');\n"
                   "    put_port(&o, cur_server_port);\n"
                   "    put_s(&o, \"<br>up \");\n",
                   "    put_s(&o, \"<div class=top><h1>rach2</h1><div class=stat>10.0.2.15:80<br>up \");\n"),
    "httpd-port": ("httpd.c", "while (*p >= '0' && *p <= '9' && v <= 65535) {",
                   "while (*p >= '0' && *p <= '9') {"),
    "initial-record": ("dhcpc.c", "    if (holding == 0) {\n        return 0;\n    }\n    return unconfigure(fd) == 0 ? 0 : -1;",
                       "    return unconfigure(fd) == 0 ? 0 : -1;"),
    "adopted": ("dhcpc.c", "record_claim((long)getpid(), adopted ? 0 : l->address)",
                "record_claim((long)getpid(), l->address)"),
    "handoff-adopted": ("dhcpc.c", "if (holding == 0 || adopted || drop_recorded_address(holding) == 0)",
                        "if (holding == 0 || drop_recorded_address(holding) == 0)"),
    "claim-rewrite": ("dhcpc.c", "write_claim(fd, (long)getpid(), adopted ? 0 : holding) == 0;",
                      "write_claim(fd, (long)getpid(), holding) == 0;"),
}
changes["rebinding"] = changes["receive"]
if fault not in changes:
    raise SystemExit("netutil-mutation: unknown fault: " + fault)
name, old, new = changes[fault]
path = root / name
source = path.read_text()
if source.count(old) != 1:
    raise SystemExit("netutil-mutation: source pattern is not unique: " + fault)
path.write_text(source.replace(old, new))
PY
}

faults='read-first read-second seek cleanup release expiry mask resolver claim-open
    claim-read holder-open holder-read detach dup receive rebinding dhcp-status nc-port
    httpd-page httpd-port initial-record adopted handoff-adopted claim-rewrite'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault utils "$fault" >/dev/null || exit 1
    done
    echo 'netutil-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" netutil-mutation "$repo/tests/host/netutil_mutate.sh" "$@"

mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/netutil-proof.XXXXXX")
mkdir "$work/utils" "$work/saved"
cp utils/*.c utils/*.h "$work/utils/"
cp "$work/utils/"* "$work/saved/"
child=

# cleanup: stop the running test and put the saved sources back; the loop calls
# it after each fault, and it is the exit trap.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/saved/"* "$work/utils/"
    touch "$work/utils/"*.c
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE TEST STATUS MESSAGE: compile TEST_hosttest.c over the copy as
# $work/PHASE, run it for at most 15 seconds, and exit unless it ends with
# STATUS and its log contains MESSAGE.
run_phase()
{
    phase=$1
    test=$2
    expected=$3
    message=$4
    binary="$work/$phase"
    deps="$work/utils/ipcheck.c"
    case $test in
        dhcpc) deps="$deps $work/utils/dhcpmsg.c" ;;
        netup) deps="$deps $work/utils/netcfg.c" ;;
    esac
    # shellcheck disable=SC2086
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -g -O1 -Wno-empty-body -ffreestanding \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/utils" -I tests/host/compat/netutil \
        "tests/host/${test}_hosttest.c" $deps -o "$binary" \
        >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/utils/"*.c "$binary" >"$binary.cksum"
    perl -e 'alarm 15; exec @ARGV or die "netutil-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "netutil-mutation: result: $phase status $status"
}

echo "netutil-mutation: logs: $work"
for test in netcfg_io dhcpc netup ncport httpd; do
    run_phase "$test-fixed" "$test" 0 'result: pass'
done
if [ "$#" -eq 0 ]; then
    # shellcheck disable=SC2086 # the faults are separate words
    set -- $faults
fi
for fault do
    apply_fault "$work/utils" "$fault"
    test=dhcpc
    case $fault in
        read-first) test=netcfg_io; message='first-pass read error' ;;
        read-second) test=netcfg_io; message='second-pass read error' ;;
        seek) test=netcfg_io; message='seek error' ;;
        cleanup) message='failed removal retains address and claim' ;;
        release) message='failed removal retains address and claim' ;;
        expiry) message='expiry propagates removal failure' ;;
        mask) message='application uses validation mask on unchanged address' ;;
        resolver) message='published resolver stays complete during overlapping writes' ;;
        claim-open) message='unreadable claim remains present' ;;
        claim-read) message='claim read error is busy' ;;
        holder-open) test=netup; message='unreadable claim blocks static ioctls' ;;
        holder-read) test=netup; message='claim read error is busy' ;;
        detach | dup) message='inherited standard descriptor is detached' ;;
        receive) message='receive waits until its deadline' ;;
        rebinding) message='rebinding waits for half of what is left of the lease' ;;
        dhcp-status) test=netup
            message='a client that exited is reported as never reaching the background' ;;
        nc-port) test=ncport; message='a port past 65535 is refused' ;;
        httpd-page) test=httpd
            message='the page names the address and port the connection reached' ;;
        # Without the bound the twenty-digit operand overflows the sum, and the
        # sanitizer ends the run with this report and status 1 on the testing
        # host, which is Linux; on macOS it aborts instead.
        httpd-port) test=httpd; message='runtime error: signed integer overflow' ;;
        initial-record) message="a first lease that cannot be recorded leaves the interface's address" ;;
        adopted) message='a first lease the interface already had is recorded as nothing to take off' ;;
        handoff-adopted) message='a child never handed the claim leaves an address it did not put on' ;;
        claim-rewrite) message='a claim written again for an adopted address records nothing to take off' ;;
    esac
    run_phase "$fault-mutated" "$test" 1 "$message"
    cleanup
    run_phase "$fault-restored" "$test" 0 'result: pass'
    for file in dhcpc.c netcfg.c netup.c nc.c httpd.c; do
        cmp "$work/saved/$file" "$work/utils/$file"
    done
done
