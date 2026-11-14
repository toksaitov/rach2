#!/bin/sh
# ref244.sh OUT CONTROLS RUNS: in the Debian reference guest, run Proton's
# d3d9:device CONTROLS times on Debian's glibc, then bind Rach2's x86_64
# glibc 2.44 over Debian's and run it RUNS times (task 84-264u). Run it
# from the tree with RACH2_QEMU_ACCEL=kvm once `make ARCH=x86_64` has made the
# GNU root and build/tests/graphics/d3d/bundle.tar.
set -eu
tree=$(cd "$(dirname "$0")/../../.." && pwd)
out=$1 controls=$2 runs=$3
[ ! -e "$out" ] || { echo "$out exists" >&2; exit 1; }
mkdir -p "$out"
root=$tree/build/x86_64/vendor/gnu/root
libs="libBrokenLocale.so.1 libanl.so.1 libc.so.6 libc_malloc_debug.so.0 libdl.so.2 libm.so.6 libmvec.so.1 libnss_compat.so.2 libnss_dns.so.2 libnss_files.so.2 libpthread.so.0 libresolv.so.2 librt.so.1 libthread_db.so.1 libutil.so.1"
stage=$out/stage
mkdir -p "$stage"
for l in $libs; do cp -L "$root/lib/$l" "$stage/"; done
cp -L "$root/lib64/ld-linux-x86-64.so.2" "$stage/"
tar -cf "$out/glibc244.tar" -C "$stage" .
rm -rf "$stage"
proton=$(sh "$tree/vendor/gnu/fetch.sh" --path ge-proton)
row() { # N
    echo "python3 /var/tmp/d3d/run.py -a lavapipe -R wine 'proton/d3d9:device' /tmp/out/$1; echo \$? > /tmp/out/$1.status; echo ROWEXIT $1 \$(cat /tmp/out/$1.status);"
}
rows1="" rows2="" n=0
i=0; while [ $i -lt "$controls" ]; do n=$((n + 1)); rows1="$rows1 $(row $n)"; i=$((i + 1)); done
i=0; while [ $i -lt "$runs" ]; do n=$((n + 1)); rows2="$rows2 $(row $n)"; i=$((i + 1)); done
bind='import ctypes, os, sys
libc = ctypes.CDLL(None, use_errno=True)
for n in sorted(os.listdir("/var/tmp/g244")):
    dst = os.path.realpath("/usr/lib/x86_64-linux-gnu/" + n)
    if not os.path.exists(dst):
        print("g244: no", dst); continue
    if libc.mount(("/var/tmp/g244/" + n).encode(), dst.encode(), None, 4096, None) != 0:
        print("g244: mount failed", dst, ctypes.get_errno()); sys.exit(1)
    print("g244: bound", dst)'
sh "$tree/tests/graphics/debian-ref/debian-ref.sh" prepare -n
sh "$tree/tests/graphics/debian-ref/debian-ref.sh" run -p virtio-vga -c 8 -m 16384 \
    -g conformance -d -r "$out/record" \
    -i "$tree/build/tests/graphics/d3d/bundle.tar" -i "$proton" -i "$out/glibc244.tar" -o "$out/out.tar.gz" \
    "mkdir -p /var/tmp/d3d /var/tmp/proton /tmp/out /var/tmp/g244 &&
     tar -xf /tmp/bundle.tar -C /var/tmp/d3d && rm /tmp/bundle.tar &&
     tar -xzf /tmp/$(basename "$proton") -C /var/tmp/proton --strip-components=1 &&
     rm /tmp/$(basename "$proton") && export D3D_PROTON=/var/tmp/proton &&
     tar -xf /tmp/glibc244.tar -C /var/tmp/g244 &&
     /lib64/ld-linux-x86-64.so.2 --version | head -1 &&
     $rows1
     sudo python3 -c '$bind' &&
     /lib64/ld-linux-x86-64.so.2 --version | head -1 &&
     $rows2
     tar -czf /tmp/out.tar.gz -C /tmp/out ."
