#!/bin/sh
# sw_sync.sh: load Linux's sw_sync, the debugfs timeline of IGT's sync file
# tests, into the Debian reference guest for one boot, as root.
#
# Debian's kernel is built without it (CONFIG_SW_SYNC is not set), and the
# option is a boolean, so this builds the running kernel's own
# drivers/dma-buf/sw_sync.c and sync_debug.c, the code that option builds
# in, as one module against its headers, from linux-source of the kernel's
# own version, installed from the snapshot for this boot, and loads it. The
# module makes /sys/kernel/debug/sync/sw_sync and sync/info as the built-in
# code does. Nothing is installed in the prepared image, so that its key
# stays the one every worktree shares. The build's log is
# /tmp/sw_sync.log, whose end goes to standard error when a step fails; the
# status is 0 once sync/sw_sync exists.
set -eu

sw=/sys/kernel/debug/sync/sw_sync
[ ! -e "$sw" ] || exit 0
kernel=$(uname -r)
version=$(dpkg-query -W -f '${Version}' "linux-image-$kernel")
dir=$(mktemp -d /tmp/sw_sync.XXXXXX)
# cleanup: remove the build and, after a failure, show the log's end.
cleanup()
{
    status=$?
    rm -rf "$dir"
    [ "$status" = 0 ] || tail -n 30 /tmp/sw_sync.log >&2
}
trap cleanup EXIT
DEBIAN_FRONTEND=noninteractive apt-get -q -y install "linux-source-6.12=$version" >> /tmp/sw_sync.log 2>&1
tar -xf /usr/src/linux-source-6.12.tar.xz -C "$dir" --strip-components=3 \
    linux-source-6.12/drivers/dma-buf/sw_sync.c linux-source-6.12/drivers/dma-buf/sync_debug.c \
    linux-source-6.12/drivers/dma-buf/sync_debug.h linux-source-6.12/drivers/dma-buf/sync_trace.h \
    linux-source-6.12/drivers/dma-buf/sync_file.c
# sync_debug.c calls sync_file_get_name, which the kernel does not export;
# the module takes Linux's own from sync_file.c.
{
    echo '#include <linux/sync_file.h>'
    awk '/^char \*sync_file_get_name\(/ { on = 1 } on { print } on && /^}/ { exit }' "$dir/sync_file.c"
} > "$dir/sync_file_name.c"
grep -q '^}' "$dir/sync_file_name.c" || { echo "sw_sync: no sync_file_get_name in sync_file.c" >&2; exit 1; }
# The trace header names its directory from the kernel's include directory,
# which an outside build has not; it is the module's own. sync_debug.c
# takes late_initcall from linux/init.h, which defines it for built-in code
# alone; a module's comes from linux/module.h.
sed -i 's|^#define TRACE_INCLUDE_PATH .*|#define TRACE_INCLUDE_PATH .|' "$dir/sync_trace.h"
cat > "$dir/license.c" << 'EOF'
#include <linux/module.h>
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("drivers/dma-buf/sw_sync.c and sync_debug.c, as CONFIG_SW_SYNC builds them in");
EOF
cat > "$dir/Makefile" << 'EOF'
obj-m := sw_sync_debug.o
sw_sync_debug-y := sw_sync.o sync_debug.o sync_file_name.o license.o
CFLAGS_sw_sync.o := -I$(src)
CFLAGS_sync_debug.o := -include linux/module.h
EOF
make -C "/lib/modules/$kernel/build" M="$dir" modules >> /tmp/sw_sync.log 2>&1
insmod "$dir/sw_sync_debug.ko"
[ -e "$sw" ] || { echo "sw_sync: the module loaded and made no $sw" >&2; exit 1; }
echo "sw_sync: result: loaded from linux-source-6.12 $version"
