#!/bin/sh
# Build and run a host test over kernel functions copied unchanged out of
# their sources, which no host test can compile whole. The host suite runs
# each NAME as a check of its own.
#
# Usage: sh tests/host/extract_check.sh NAME
#
#   chown            tests/host/fixtures/chown_plan.c: the set-id rules of a
#                    mounted chown, over the functions fixtures/chown_plan.sh
#                    extracts from rach2/sys/syscall_fs.c
#   krffs_open       tests/host/fixtures/krffs_open.c: an open of mounted
#                    KRFFS during the flush that publishes a moved node, over
#                    the resolver of rach2/sys/fs/vfs.c and the open path of
#                    rach2/sys/syscall_fs.c
#   krffs_root_lock  tests/host/fixtures/krffs_root_lock.c: the KRFFS root's
#                    data operations under the mount's lock, over the
#                    operations and the lock of rach2/sys/fs/vfs.c
#   krffs_stat       tests/host/fixtures/krffs_stat.c: the inode number the
#                    KRFFS root gives to the stat calls, over the stat
#                    functions of rach2/sys/syscall_fs.c and vfs.c
#   pagecache        tests/host/pagecache_hosttest.c with
#                    PAGECACHE_SYNC_HOSTTEST: the page cache with the
#                    bounded wait of the sys_sync of rach2/sys/syscall_fs.c
#
# The KRFFS and page cache tests extract their functions with the program
# of tests/host/fixtures/extract.c. Every test is compiled under ASan and
# UBSan in a scratch directory under build/tests, which also has the
# extracted files and which the script removes on every exit, so the tree's
# sources are only read. The script runs the test and exits with its
# status.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

usage()
{
    echo 'extract-check: usage: extract_check.sh NAME' >&2
    exit 2
}

[ "$#" -eq 1 ] || usage
name=$1
case $name in
    chown|krffs_open|krffs_root_lock|krffs_stat|pagecache) ;;
    *) usage ;;
esac
mkdir -p build/tests
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "$repo/build/tests/extract-check.XXXXXX")

cc=${CC:-cc}
san='-fsanitize=address,undefined -fno-sanitize-recover=all'
s=rach2/sys
"$cc" -std=c99 -O1 -Wall -Wextra -Werror tests/host/fixtures/extract.c -o "$work/extract"
extract=$work/extract
# The flags of a fixture compiled over kernel headers. The extracted files
# are in $work, which every compile puts first on the include path.
kernel="-std=c99 -DRACH2_HOSTTEST -g -O1 -ffreestanding"
kernel="$kernel -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast"
kernel="$kernel -I $s -I $s/lib -I $s/proc"
# shellcheck disable=SC2086 # the flags and $extract are lists of words
case $name in
    chown)
        cp $s/syscall_fs.c $s/proc/process.h $s/fs/vfs.h $s/lib/kerrno.h \
            "$work/"
        sh tests/host/fixtures/chown_plan.sh "$work"
        "$cc" -std=c99 -g -O1 -Wall -Wextra -Werror $san -I "$work" \
            tests/host/fixtures/chown_plan.c -o "$work/$name"
        ;;
    krffs_open)
        $extract "$work/vfs_open.inc" -f $s/fs/vfs.c vfs_mount_gone \
            mount_base mount_rdonly vfs_ref_hold vfs_ref_put vfs_enter \
            vfs_leave ns_my_turn vfs_ns_lock vfs_ns_unlock in_groups \
            may_search toward_cwd mount_covering path_last mount_named_at \
            mount_named named_in_rest climbs node_put walk_lock walk_unlock \
            vfs_walk_unlock ref_move_node walk_move walk_root root_current \
            dotdot_stays walk_leaf walk_pages walk_splice walk_link \
            walk_last_comp walk_is_link walk_rest_len walk_name_root \
            walk_name_add walk_name_text walk_name_end 'struct walk_record' \
            walk_record_entry walk_record_clear walk_record_move walk_release walk_finish \
            resolve_path vfs_resolve_entry \
            vfs_resolve vfs_ref_release vfs_desc_open vfs_desc_close \
            vfs_getattr vfs_open_node vfs_release vfs_truncate vfs_create
        $extract "$work/syscall_open.inc" -f $s/syscall_fs.c \
            '#define O_|CRED_|S_IF' -f $s/syscall_internal.h '#define CRED_' \
            -f $s/syscall_fs.c name_is device_name open_device_name \
            open_link_target node_make open_create open_mounted_node \
            path_starts ofd_record_path open_install open_synth_node do_open
        "$cc" -std=c99 -g -O1 -Wall -Wextra -Werror $san -pthread \
            -c tests/host/fixtures/krffs_open_threads.c -o "$work/threads.o"
        "$cc" -I "$work" $kernel -Wall -Wextra -Werror $san -pthread \
            tests/host/fixtures/krffs_open.c "$work/threads.o" $s/fs/fd.c \
            tests/host/compat/vfs_name_none.c \
            $s/lib/kstring.c $s/lib/kctypes.c -o "$work/$name"
        ;;
    krffs_root_lock)
        $extract "$work/kfs_data.inc" -f $s/fs/vfs.c 'struct ns_turn' \
            mount_base ns_my_turn vfs_ns_lock vfs_ns_unlock kfs_read_locked \
            kfs_write_locked kfs_truncate_locked kfs_fsync_locked \
            kfs_readdir_locked kfs_read kfs_write kfs_truncate kfs_fsync \
            kfs_readdir
        "$cc" -std=c99 -g -O1 -Wall -Wextra -Werror $san -pthread \
            -c tests/host/fixtures/krffs_open_threads.c -o "$work/threads.o"
        "$cc" -I "$work" $kernel -Wall -Wextra -Werror $san -pthread \
            tests/host/fixtures/krffs_root_lock.c "$work/threads.o" \
            $s/lib/kstring.c $s/lib/kctypes.c -o "$work/$name"
        ;;
    krffs_stat)
        $extract "$work/krffs_stat_types.inc" \
            -f $s/arch/i686/uabi.h 'struct linux_stat' 'struct user_stat' \
            -f $s/fs/vfs.h 'struct vfs_stat' -f $s/arch/i686/uabi.h \
            '#define STAT_SET_INO' -f $s/syscall_fs.c 'struct kstat'
        $extract "$work/krffs_stat_funcs.inc" -f $s/syscall_fs.c \
            path_names_dir stat64_put stat64_node_times stat64_root_times \
            krffs_fill_stat64 stat_times_from_stat64 stat_node_times \
            stat_root_times do_stat64 old_stat_put do_old_stat \
            -f $s/fs/vfs.c kfs_getattr
        "$cc" -I "$work" $kernel $san tests/host/fixtures/krffs_stat.c \
            $s/lib/kstring.c $s/lib/kctypes.c -o "$work/$name"
        ;;
    pagecache)
        $extract "$work/syscall_sync.inc" -f $s/syscall_fs.c sys_sync
        "$cc" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
            -DPAGECACHE_SYNC_HOSTTEST -g -O1 -ffreestanding -Wno-empty-body \
            -Wall -Wextra -Werror $san -I "$work" -I $s -I $s/arch/x86 \
            -I $s/arch/i686 -I $s/lib -I $s/proc -I utils -I krffs \
            -I tests/host/compat tests/host/pagecache_hosttest.c \
            -o "$work/$name"
        ;;
esac
"$work/$name"
