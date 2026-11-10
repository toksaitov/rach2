#!/bin/sh
#
# tooling_job.sh: the job that builds the GNU layer and its root image in
# etude's tooling machine, which tooling_image.sh runs there.
#
# The job runner of `etude provision tooling` (tools/etude/tooling/runner.sh)
# runs it as root in the job's working directory, which holds two inputs:
# tree.tar.gz, the tree the host staged, and job.env, the shell assignments of
# RACH2_ARCH, GNU_AGENTS, GNU_LINK, GNU_I386, RACH2_PATH_MAP, MUSL_CRYPT_SRC,
# IMAGE (the image's file name), SIZE (its size in MiB), JOBS (make's jobs),
# DRY_RUN and IMAGE_TIME, the host's commit date, which the staged tree, not
# a checkout, does not have and mkext2.sh gives the image's nodes.
# ETUDE_DISK1 is the layer's build disk, an ext4 volume labelled rach2-gnu
# that persists between jobs; a disk on which blkid finds no signature, as a
# new one, is formatted, and a disk with any other volume is refused. The
# volume's file `machine` has the machine's architecture, as `uname -m` gives
# it, since the build host's programs that the build tree keeps run on that
# architecture alone; a disk of another machine is refused.
# ETUDE_DISK2, which a dry run does not have, is the image's disk, of the
# image's size and reading as zeros.
#
# The job mounts the build disk at /gnu and replaces the tree in /gnu/rach2
# with the host's, keeping the layer's build tree, build/vendor/gnu or
# build/x86_64/vendor/gnu, and downloads in vendor/gnu/downloads and
# build/cache. As the account rach2, uid 1000, it runs make for the layer's
# targets in that tree, and
# tools/image/mkext2.sh with install_root.sh as mk/gnu-image.mk's rule for a
# Linux host does. It copies the image to ETUDE_DISK2, sparse, and writes its
# SHA-256 to the file `digest`. Under DRY_RUN make runs with -n, and no image
# is made. The volume is mounted without delayed allocation, so a stamp that
# a stopped machine left on the disk follows the files its step wrote.
#
# The disk keeps its step store in /gnu/store, bind-mounted at
# /var/cache/rach2-store because GNU outputs keep absolute build paths. The
# builder receives that path as RACH2_STORE. SpiderMonkey builds before the
# other layers, with its separate pool limited to two jobs per 3 GiB after
# leaving a GiB for the machine's other processes.
#
# The build disk also has a swap file of 8 GiB, which the job takes while it
# runs. Each of gcc's largest files takes nearly 2 GiB to compile, and make
# runs JOBS of them at once beside the layer's other steps, more than the
# machine's memory holds; without swap the kernel kills a compiler and the
# build fails.
#
# make's output and the scripts' go to a log of the job's own in /gnu/log,
# which keeps the last 20, and to the machine's second serial port, ttyS1 on
# amd64 and ttyAMA1 on arm64, each line after the prefix `gnu-vm: `. That
# port is etude's channel, which passes over lines it does not know
# and keeps them in channel.log in the job's directory, where tooling_image.sh
# reads them while the job runs; the console, the first port, is not used,
# since its getty hangs up the port once it starts. The standard output has
# the outcome, and the end of the log after a failure.
#
# Usage: sh tooling_job.sh
#

set -u

MNT=/gnu
TREE=$MNT/rach2
CHAN=/dev/ttyS1
[ ! -c /dev/ttyAMA1 ] || CHAN=/dev/ttyAMA1
BUILDER=rach2
BUILDER_ID=1000
LABEL=rach2-gnu
SWAP=$MNT/swap

fail()
{
    echo "gnu: job: $*" >&2
    exit 1
}

# builder COMMAND...: run COMMAND as the build's account, with an
# environment of its own. In a machine that is not x86, QEMU_LD_PREFIX gives
# QEMU's user-mode emulator the toolchain's sysroot, under which an i386 or
# x86-64 program's loader and libraries are found first, as an x86-64 host's
# own are when the build runs a program it has just linked, such as bzip2's
# test of itself.
builder()
{
    setpriv --reuid=$BUILDER_ID --regid=$BUILDER_ID --clear-groups \
        env -i HOME=$MNT/home LANG=C.UTF-8 TMPDIR=$MNT/tmp RACH2_STORE=/var/cache/rach2-store \
        ${LD_PREFIX:+QEMU_LD_PREFIX="$LD_PREFIX"} \
        PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin "$@"
}

# logged COMMAND...: run COMMAND as the build's account, its output in the
# log and on the channel; the status is the command's.
logged()
{
    { builder "$@" 2>&1; echo $? > /run/gnu-job-status; } |
        tee -a "$LOG" | sed -u 's/^/gnu-vm: /' > "$CHAN"
    return "$(cat /run/gnu-job-status)"
}

# mount_disk: format the build disk when it is empty, check it, mount it,
# and grow its volume to the disk.
mount_disk()
{
    type=$(blkid -p -o value -s TYPE "$DISK" 2>/dev/null)
    if [ -z "$type" ]; then
        if blkid -p "$DISK" > /dev/null 2>&1; then
            fail "the build disk has a signature blkid cannot name; it is not the layer's"
        fi
        mkfs.ext4 -q -L $LABEL -m 0 -E lazy_itable_init=1,lazy_journal_init=1,nodiscard "$DISK" ||
            fail 'mkfs.ext4 failed on the build disk'
        echo "gnu: job: formatted the build disk"
    elif [ "$type" != ext4 ] || [ "$(blkid -p -o value -s LABEL "$DISK")" != $LABEL ]; then
        fail "the build disk has a $type volume, not the layer's ext4 volume $LABEL"
    fi
    e2fsck -p "$DISK" > /dev/null 2>&1
    [ $? -le 2 ] || fail 'e2fsck found faults in the build disk that it does not repair'
    mkdir -p $MNT
    mount -o noinit_itable,nodelalloc "$DISK" $MNT || fail 'the build disk does not mount'
    resize2fs "$DISK" > /dev/null 2>&1 || fail 'resize2fs cannot grow the volume to the disk'
    [ -s $MNT/machine ] || uname -m > $MNT/machine
    built=$(cat $MNT/machine)
    if [ "$built" != "$(uname -m)" ]; then
        umount $MNT
        fail "the build disk was built on $built, and this machine is $(uname -m); remove the disk"
    fi
}

# sync_tree: as the build's account, replace the tree with the host's,
# keeping the layer's build tree and its downloads, and the times the host
# gave each file.
sync_tree()
{
    cd $TREE || exit 1
    keep=./build$SUFFIX/vendor/gnu
    find . -mindepth 1 \( -path "$keep" -o -path ./vendor/gnu/downloads -o -path ./build/cache \) -prune -o \
        ! -type d -exec rm -f {} + || exit 1
    find . -mindepth 1 -type d -empty ! -path "$keep" ! -path "$keep/*" \
        ! -path ./vendor/gnu/downloads ! -path './vendor/gnu/downloads/*' \
        ! -path ./build/cache ! -path './build/cache/*' -delete || exit 1
    tar -x -z -f "$WORK/tree.tar.gz" --warning=no-unknown-keyword || exit 1
    echo "gnu: job: the tree has $(find . -path "$keep" -prune -o -type f -print | wc -l) files"
}

# build_layer: as the build's account, run make for the layer's targets.
build_layer()
{
    cd $TREE || exit 1
    js_jobs=$(((JOBS - 1) * 2 / 3))
    [ "$js_jobs" -gt 0 ] || js_jobs=1
    # mach has its own pool, with compilers of up to 1.5 GiB each.
    # shellcheck disable=SC2086 # DRY_RUN is empty or -n
    make --no-print-directory -j"$JOBS" $DRY_RUN ARCH="$RACH2_ARCH" \
        DRY_RUN="$DRY_RUN" GNU_JOBS="$js_jobs" gnu-js || exit 1
    set --
    if [ "${GNU_TEST_PRODUCTS:-}" = 1 ]; then
        # The product fixtures build against ported musl and e2fsprogs.
        # Both Debian cross toolchains are installed in the tooling recipe.
        sh tools/setup/elfwrap.sh i686 "$TREE/build/tooling-elf" || exit 1
        sh tools/setup/elfwrap.sh x86_64 "$TREE/build/tooling-elf" || exit 1
        PATH=$TREE/build/tooling-elf:$PATH
        export PATH
        set -- gnu-sanitizers gnu-host-products gnu-fstools
        [ "$IMAGE" != gnu-host-checks.tar ] || set -- "$@" gnu-host-checks
    fi
    [ "${GNU_I386:-}" != 1 ] || set -- "$@" gnu-i386
    [ "$RACH2_ARCH" != x86_64 ] || set -- "$@" gnu-kvm
    # shellcheck disable=SC2086 # DRY_RUN is empty or -n
    exec make --no-print-directory -j"$JOBS" $DRY_RUN ARCH="$RACH2_ARCH" GNU_AGENTS="$GNU_AGENTS" \
        DRY_RUN="$DRY_RUN" GNU_LINK="$GNU_LINK" \
        gnu-ports gnu-native gnu-gdb gnu-js gnu-desktop gnu-proton gnu-webgl gnu-venus gnu-va gnu-games gnu-agents "$@"
}

# build_image: as the build's account, make the image as mk/gnu-image.mk's
# rule for a Linux host makes it, with the KRFFS stager built for this host;
# for gnu-host-checks.tar, the archive of the host checks' directory, as the
# same file makes it, followed by zeros to the disk's size.
build_image()
{
    cd $TREE || exit 1
    if [ "$IMAGE" = gnu-host-checks.tar ]; then
        dir=$(readlink -f "build$SUFFIX/vendor/gnu/host-checks") || exit 1
        tar --format=ustar -C "$(dirname "$dir")" -cf "$MNT/out/$IMAGE" "$(basename "$dir")" || exit 1
        [ "$(stat -c %s "$MNT/out/$IMAGE")" -le $((SIZE * 1048576)) ] || {
            echo "gnu: job: the host checks' archive is larger than $SIZE MiB" >&2
            exit 1
        }
        exec truncate -s "${SIZE}M" "$MNT/out/$IMAGE"
    fi
    make --no-print-directory -C krffs MUSL_CRYPT="../$MUSL_CRYPT_SRC" ../build/krffs/stageimage.krffs ||
        exit 1
    [ -z "$RACH2_PATH_MAP" ] || export RACH2_PATH_MAP
    RACH2_ARCH=$RACH2_ARCH GNU_AGENTS="$GNU_AGENTS" GNU_TEST_PRODUCTS=${GNU_TEST_PRODUCTS:-} \
        GNU_I386=${GNU_I386:-} SOURCE_DATE_EPOCH=${IMAGE_TIME:-1} exec sh tools/image/mkext2.sh \
        --overlay vendor/gnu/install_root.sh "${MANIFEST:-krffs/manifests/os-real-gnu.manifest}" "$MNT/out/$IMAGE" "$SIZE"
}

WORK=$(pwd)
[ -f job.env ] || fail 'the job has no job.env'
. ./job.env
SUFFIX=
[ "$RACH2_ARCH" != x86_64 ] || SUFFIX=/x86_64
LD_PREFIX=
case "$(uname -m)" in
    x86_64 | i?86) ;;
    *) LD_PREFIX=$TREE/build$SUFFIX/vendor/gnu/sysroot ;;
esac
[ -z "$DRY_RUN" ] || DRY_RUN=-n
case ${1:-} in
    --tree) sync_tree; exit ;;
    --layer) build_layer ;;
    --image) build_image ;;
esac

: > digest
DISK=${ETUDE_DISK1:?the job has no build disk}
[ -n "$DRY_RUN" ] || [ -b "${ETUDE_DISK2:-}" ] || fail 'the job has no disk for the image'
mount_disk
getent passwd $BUILDER > /dev/null ||
    useradd -u $BUILDER_ID -U -M -d $MNT/home -s /bin/sh $BUILDER || fail 'useradd failed'
mkdir -p $MNT/home $TREE $MNT/log $MNT/tmp $MNT/out || fail 'the directories of the build disk'
chown $BUILDER_ID:$BUILDER_ID $MNT/home $TREE $MNT/log $MNT/tmp $MNT/out || fail 'chown failed'
mkdir -p $MNT/store /var/cache/rach2-store || fail 'the step store directories'
chown $BUILDER_ID:$BUILDER_ID $MNT/store || fail 'the step store owner'
mount --bind $MNT/store /var/cache/rach2-store || fail 'the step store does not mount'
find $MNT/tmp $MNT/out -mindepth 1 -delete
if [ ! -f $SWAP ]; then
    { fallocate -l 8G $SWAP.new && chmod 600 $SWAP.new && mkswap $SWAP.new > /dev/null &&
        mv $SWAP.new $SWAP; } || fail 'the swap file cannot be made'
fi
swapon $SWAP || fail 'the swap file cannot be taken'
LOG=$MNT/log/$(date -u +%Y%m%dT%H%M%SZ)-$RACH2_ARCH.log
find $MNT/log -name "*.log" | sort -r | tail -n +20 | xargs rm -f
echo "gnu: job: $RACH2_ARCH, agents: ${GNU_AGENTS:-none}, link: ${GNU_LINK:-shared}, jobs: $JOBS${DRY_RUN:+, dry run}" |
    tee "$LOG" | sed -u 's/^/gnu-vm: /' > "$CHAN"

status=0
logged sh "$0" --tree && logged sh "$0" --layer || status=$?
if [ $status -eq 0 ] && [ -z "$DRY_RUN" ]; then
    logged sh "$0" --image || status=$?
fi
if [ $status -eq 0 ] && [ -z "$DRY_RUN" ]; then
    img=$MNT/out/$IMAGE
    if [ "$(stat -c %s "$img")" != "$(blockdev --getsize64 "$ETUDE_DISK2")" ]; then
        echo "gnu: job: the image's disk is not the image's size" >&2
        status=1
    elif ! sha256sum "$img" | cut -d ' ' -f 1 > digest ||
        ! dd if="$img" of="$ETUDE_DISK2" bs=4096 conv=sparse,fsync status=none; then
        echo "gnu: job: the image cannot be copied to its disk" >&2
        status=1
    else
        echo "gnu: job: made $IMAGE, sha256 $(cat digest)"
    fi
    rm -f "$img"
fi
if [ $status -ne 0 ]; then
    echo "gnu: job: failed with status $status; the end of $LOG on the build disk:" >&2
    tail -n 40 "$LOG" >&2
fi
find $MNT/tmp -mindepth 1 -delete
swapoff $SWAP
sync
umount /var/cache/rach2-store || { echo "gnu: job: the step store does not unmount" >&2; status=1; }
fstrim $MNT > /dev/null 2>&1
umount $MNT || { echo "gnu: job: the build disk does not unmount" >&2; status=1; }
exit $status
