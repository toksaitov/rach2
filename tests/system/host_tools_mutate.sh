#!/bin/sh
# Prove the host-tool regression tests with restored, rebuilt source mutations.
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
cd "$REPO"
mkdir -p build
WORK=$(mktemp -d "$REPO/build/host-tools-proof.XXXXXX")
source_file=
created=
active=
restore() {
    if [ -n "$active" ]; then
        kill -TERM "$active" 2>/dev/null || :
        wait "$active" 2>/dev/null || :
        active=
    fi
    if [ -n "$source_file" ]; then
        if [ "$created" = musl ]; then
            rm -f "$source_file" build/tests/musl/review_unassigned
        else
            cp "$WORK/source" "$source_file"
            touch "$source_file"
        fi
        if [ "$created" = root ]; then
            rm -f build/tests/review_unassigned build/tests/review_unassigned.d
        fi
        created=
        source_file=
    fi
}
trap restore 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
fail() { echo "host-tools-proof: $*; logs: $WORK" >&2; exit 1; }
job() {
    "$REPO/build/tests/system/deadline" 120 "$@" &
    active=$!
    status=0
    wait "$active" || status=$?
    active=
    return "$status"
}
run_test() {
    phase=$1
    log="$WORK/$case_name.$phase"
    case "$test_name" in
        *.c)
            deps=$(sed -n 's/.*host-deps: *//p' "$test_name" | tr '\n' ' ')
            extra=$(sed -n 's/.*host-cflags: *//p' "$test_name" | tr '\n' ' ')
            # shellcheck disable=SC2086
            job "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
                -DRACH2_HOSTTEST -g -O1 -ffreestanding -fsanitize=address,undefined \
                -fno-sanitize-recover=all -I rach2/sys -I rach2/sys/lib \
                -I rach2/sys/proc -I utils -I krffs -I tests/host/compat \
                $extra -o "$WORK/test" "$test_name" $deps > "$log.compile" 2>&1 ||
                fail "$case_name did not compile"
            job "$WORK/test" > "$log.run" 2>&1
            ;;
        *) job sh "$test_name" $test_arg > "$log.run" 2>&1 ;;
    esac
}
[ -x build/tests/system/deadline ] || fail 'build the deadline tool first'
cases='ring temporary gap-growth gap-slack utility-links dns-links kernel-link kernel-strip ownership truncate timestamps host-djb host-mix kernel-djb kernel-mix probe-root probe-musl'
if [ "$#" -eq 0 ]; then
    # shellcheck disable=SC2086
    set -- $cases
fi
for case_name in "$@"; do
    test_arg=
    case "$case_name" in
        ring) file=krffs/krffs.c; expected='oversized root rejected' ;;
        temporary) file=tools/verify/check.sh; expected='write followed the planted symlink' ;;
        gap-growth) file=krffs/krffs_fuse_operations.c; expected='write gap is zero' ;;
        gap-slack) file=krffs/krffs_fuse_operations.c; expected='slack write gap is zero' ;;
        utility-links) file=utils/Makefile; expected='libc.a did not relink'; test_arg=libraries ;;
        dns-links) file=tests/Makefile; expected='libc.a did not relink'; test_arg=libraries ;;
        kernel-link) file=rach2/Makefile; expected='linker script edit did not reach'; test_arg=kernel ;;
        kernel-strip) file=rach2/Makefile; expected='the kernel a loader reads carries debug sections'; test_arg=kernel ;;
        ownership) file=krffs/krffs_fuse_operations.c; expected='group-only chown preserves uid' ;;
        truncate) file=krffs/krffs_allocator.c; expected='capacity growth updates eof' ;;
        timestamps) file=krffs/krffs_fuse_operations.c; expected='explicit timestamps retained' ;;
        host-djb) file=krffs/krffs_utilities.c; expected='hash vector failed' ;;
        host-mix) file=krffs/stageimage.krffs.c; expected='hash vector failed' ;;
        kernel-djb|kernel-mix) file=rach2/sys/fs/krffs.c; expected='kernel hash vector' ;;
        probe-root) file=tests/Makefile; expected='unassigned program: review_unassigned' ;;
        probe-musl) file=tests/musl/review_unassigned.c; expected='unassigned program: musl/review_unassigned' ;;
        *) fail "unknown mutation: $case_name" ;;
    esac
    case "$case_name" in
        temporary) test_name=tests/system/verify_temp_test.sh ;;
        *-links|kernel-link|kernel-strip) test_name=tests/system/build_inputs_test.sh ;;
        host-djb|host-mix) test_name=tests/host/krffs_hash_hosttest.c ;;
        kernel-djb|kernel-mix) test_name=tests/host/krffs_hosttest.c ;;
        probe-*) test_name=tools/verify/probecheck.sh ;;
        *) test_name=tests/host/krffs_fuse_hosttest.c ;;
    esac
    run_test baseline || fail "$case_name baseline failed"
    if [ "$case_name" = probe-musl ]; then
        test ! -e "$file" || fail "$file already exists"
        created=musl
    else
        cp "$file" "$WORK/source"
    fi
    if [ "$case_name" = probe-root ]; then
        test ! -e build/tests/review_unassigned || fail 'unassigned root artifact already exists'
        created=root
    fi
    source_file=$file
    python3 - "$case_name" "$file" <<'PY'
import sys
from pathlib import Path
kind, file = sys.argv[1:]
p = Path(file)
s = p.read_text() if p.exists() else ''
def replace(old, new):
    global s
    if s.count(old) != 1:
        raise SystemExit('mutation source did not match: ' + kind)
    s = s.replace(old, new)
if kind == 'ring':
    replace('if (!valid_node_ring(&file_system))',
            'if (file_system.node->magic != KRFFS_File_System_Magic)')
elif kind == 'temporary':
    start = s.index('VDIR=')
    end = s.index('LIBC=', start)
    s = s[:start] + 'VDIR=/tmp/rach2_verify\nmkdir -p "$VDIR"\n' + s[end:]
elif kind in ('gap-growth', 'gap-slack'):
    start = s.index('int krffs_write(')
    end = s.index('int krffs_fsync(', start)
    part = s[start:end]
    token = 'krffs_write_into_node(node, buffer, size, offset, old_data_size);'
    first = part.index(token)
    position = first if kind == 'gap-slack' else part.index(token, first + 1)
    part = part[:position] + part[position:].replace(
        token, 'krffs_write_into_node(node, buffer, size, offset, (uint64_t)offset);', 1)
    s = s[:start] + part + s[end:]
elif kind == 'utility-links':
    replace('$(TARGETS) $(SBIN_TARGETS): $(CONFIG) $(CRT1) $(CRTI) \\\n    $(PREFIX)/lib/libc.a $(LIBGCC) $(CRTN)',
            '$(TARGETS) $(SBIN_TARGETS): $(CONFIG)')
elif kind == 'dns-links':
    replace('$(BUILD)/dnstest: dnstest.c ../utils/resolv.c ../utils/netdb.h \\\n    $(MUSL_PRE) $(MUSL_PREFIX)/lib/libc.a $(MUSL_LIBGCC) \\\n    $(MUSL_PREFIX)/lib/crtn.o',
            '$(BUILD)/dnstest: dnstest.c ../utils/resolv.c ../utils/netdb.h')
elif kind == 'kernel-link':
    replace('$(KERNEL_BIN) : $(ASM_OBJECTS) $(C_OBJECTS) $(LINKER_SCRIPT).ld',
            '$(KERNEL_BIN) : $(ASM_OBJECTS) $(C_OBJECTS)')
elif kind == 'kernel-strip':
    replace('$(CROSS_OBJCOPY) --strip-debug "$$tmp.debug" "$$tmp"',
            'cp "$$tmp.debug" "$$tmp"')
elif kind == 'ownership':
    replace('if (uid != (uid_t)-1)', 'if (1)')
    replace('if (gid != (gid_t)-1)', 'if (1)')
elif kind == 'truncate':
    replace('        node->data_size = size;\n        krffs_sync_mapping(node, sizeof(*node));\n        return node;',
            '        return node;')
elif kind == 'timestamps':
    start = s.index('            if (!time || time[0].tv_nsec == UTIME_NOW)')
    end = s.index('\n\n            krffs_sync_mapping', start)
    s = s[:start] + '            node->access_time = current_node_time;\n            node->modification_time = current_node_time;' + s[end:]
elif kind == 'host-djb':
    replace('hash = ((hash << 5) + hash) ^ character;',
            'hash = ((hash << 5) + hash) + character;')
elif kind == 'kernel-djb':
    replace('hash = ((hash << 5) + hash) ^ (uint64_t)c;',
            'hash = ((hash << 5) + hash) + (uint64_t)c;')
elif kind in ('host-mix', 'kernel-mix'):
    replace('h *= 1099511628211ULL;', 'h *= 1099511628213ULL;')
elif kind == 'probe-root':
    s += '\nPROGS += review_unassigned\n$(BUILD)/review_unassigned: hello.c\n\t$(CC) $(CFLAGS) -o $@ $<\n'
elif kind == 'probe-musl':
    s = 'int main(void) { return 0; }\n'
p.write_text(s)
PY
    touch "$source_file"
    case "$case_name" in
        probe-root)
            job make -C tests ../build/tests/review_unassigned > "$WORK/$case_name.build" 2>&1 ||
                fail 'unassigned root probe did not build' ;;
        probe-musl)
            job make musl-programs > "$WORK/$case_name.build" 2>&1 ||
                fail 'unassigned musl probe did not build' ;;
    esac
    result=0
    run_test mutated || result=$?
    [ "$result" -gt 0 ] && [ "$result" -lt 124 ] || fail "$case_name mutation was not caught (exit $result)"
    grep -Fq "$expected" "$WORK/$case_name.mutated.run" || fail "$case_name failed for another reason"
    restore
    if [ "$case_name" = probe-musl ]; then
        job make musl-programs > "$WORK/$case_name.rebuilt" 2>&1 || fail "musl restoration did not build"
    elif [ "$case_name" = probe-root ]; then
        job make -C tests > "$WORK/$case_name.rebuilt" 2>&1 || fail "probe restoration did not build"
    fi
    run_test restored || fail "$case_name restored test failed"
    echo "host-tools-proof: passed: $case_name (baseline, caught mutation, rebuilt restoration)"
done
echo "host-tools-proof: logs: $WORK"
