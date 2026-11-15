#!/bin/sh
# host-scan.sh: run gem_readwrite's read-write subtest on a Linux host in a
# user-namespace chroot of the x86-64 GNU root, with a preloaded scan.
#
# Usage: unshare -rm sh host-scan.sh ROOT IGT DESKTOP SCRATCH [cache]
#
# ROOT is build/x86_64/vendor/gnu/root, IGT and DESKTOP the store's igt and
# debian/desktop trees, SCRATCH an empty directory under build/tmp that
# holds stackscan.so and iconvtrace.so, built with gcc -shared -fPIC. The
# chroot is an overlay of IGT over ROOT, with DESKTOP's libraries less
# glibc's on the library path. "cache" first writes gconv-modules.cache
# with the root's own iconvconfig. PRELOAD names the scan, by default
# /tmp/stackscan.so. The output goes to SCRATCH/out.
set -e
R=$1 I=$2 D=$3 S=$4
up() {
    mount -t overlay overlay -o "lowerdir=$I:$R,upperdir=$S/up,workdir=$S/work" "$S/mnt"
    mount --bind "$D" "$S/mnt/deb"
    mount --rbind /proc "$S/mnt/proc"
    mount --rbind /dev "$S/mnt/dev"
}
down() {
    umount -l "$S/mnt/proc"
    umount -l "$S/mnt/dev"
    umount "$S/mnt/deb"
    umount "$S/mnt"
}
rm -rf "$S/up" "$S/work"
mkdir -p "$S/mnt" "$S/up/tmp" "$S/up/proc" "$S/up/dev" "$S/up/deb" "$S/up/deblib" "$S/work"
cp "$S/stackscan.so" "$S/iconvtrace.so" "$S/up/tmp/"
for f in "$D"/usr/lib/x86_64-linux-gnu/*; do
    b=${f##*/}
    case $b in
    ld-linux* | libc.so* | libm.so* | libmvec* | libpthread* | libdl.so* | librt.so* | \
        libresolv* | libanl* | libutil* | libBrokenLocale* | libnss_* | libthread_db* | \
        libc_malloc* | libmemusage* | libpcprofile* | gconv) continue ;;
    esac
    ln -s "/deb/usr/lib/x86_64-linux-gnu/$b" "$S/up/deblib/$b"
done
up
if [ "$5" = cache ]; then
    chroot "$S/mnt" /usr/sbin/iconvconfig -o /tmp/gconv-modules.cache
    down
    mkdir -p "$S/up/usr/lib/gconv"
    mv "$S/up/tmp/gconv-modules.cache" "$S/up/usr/lib/gconv/"
    up
fi
chroot "$S/mnt" /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin LANG=C.UTF-8 \
    LD_LIBRARY_PATH=/deblib:/usr/libexec/igt-gpu-tools/lib LD_PRELOAD="${PRELOAD:-/tmp/stackscan.so}" \
    /usr/libexec/igt-gpu-tools/gem_readwrite --run-subtest read-write >"$S/out" 2>&1 || true
down
