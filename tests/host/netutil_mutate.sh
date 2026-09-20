#!/bin/sh
# Compile fixed, faulty and restored network utilities in a private directory.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/netutil-proof.XXXXXX")
mkdir "$work/utils" "$work/saved"
cp utils/*.c utils/*.h "$work/utils/"
cp "$work/utils/"* "$work/saved/"
child=

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
for test in netcfg_io dhcpc netup; do
    run_phase "$test-fixed" "$test" 0 'result: pass'
done
for fault in read-first read-second seek cleanup release expiry mask resolver \
    claim-open claim-read holder-open holder-read detach dup; do
    python3 - "$work/utils" "$fault" <<'PY'
from pathlib import Path
import sys

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
}
name, old, new = changes[fault]
path = root / name
source = path.read_text()
if source.count(old) != 1:
    raise SystemExit("netutil-mutation: source pattern is not unique: " + fault)
path.write_text(source.replace(old, new))
PY
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
    esac
    run_phase "$fault-mutated" "$test" 1 "$message"
    cleanup
    run_phase "$fault-restored" "$test" 0 'result: pass'
    cmp "$work/saved/dhcpc.c" "$work/utils/dhcpc.c"
    cmp "$work/saved/netcfg.c" "$work/utils/netcfg.c"
    cmp "$work/saved/netup.c" "$work/utils/netup.c"
done
