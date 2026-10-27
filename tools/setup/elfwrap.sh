#!/bin/sh
#
# tools/setup/elfwrap.sh: make the bare elf cross toolchain the tree calls
# from a Debian or Ubuntu cross toolchain that targets Linux.
#
# Usage: sh tools/setup/elfwrap.sh ARCH DIR
#
# ARCH is i686 or x86_64. The tools of /usr/bin/ARCH-linux-gnu-* are linked
# into DIR as ARCH-elf-*, and DIR/ARCH-elf-gcc is written as a script that
# runs ARCH-linux-gnu-gcc with the defaults of a Linux target turned off:
# position-independent code, the stack protector, control-flow protection,
# stack-clash probes, unwind tables, fortified sources and the Linux target
# macros. The kernel and the guest programs are written for a bare elf GCC,
# and a static tcc built position-independent faults at address 0x1c. The
# caller's own flags come after the wrapper's, so they may turn any of them
# back on. DIR is made when it is missing; files of the same names in it are
# replaced. The wrapper records the checksums of the Linux compiler's driver
# and of its cc1. An existing wrapper with the same bytes is kept, and a new
# one takes the date of the newer of those two files, not of its writing.
# mk/fingerprint.mk reads the wrapper's size and date, so a run again, or in
# a fresh machine, rebuilds nothing, and a changed compiler rebuilds all.
# RACH2_ELFWRAP_FROM names the directory of the Linux tools, /usr/bin by
# default. Exit 0 when the toolchain is in place, 1 when the Linux one is
# missing or DIR cannot be written, 2 on a usage error.
#
set -u

[ "$#" -eq 2 ] || { echo 'usage: elfwrap.sh i686|x86_64 DIR' >&2; exit 2; }
arch=$1 dir=$2
case "$arch" in
    i686|x86_64) ;;
    *) echo "elfwrap: unknown architecture: $arch" >&2; exit 2 ;;
esac
from=${RACH2_ELFWRAP_FROM:-/usr/bin}/$arch-linux-gnu
if [ ! -x "$from-gcc" ]; then
    echo "elfwrap: $from-gcc is missing; install the $arch-linux-gnu cross compiler first" >&2
    exit 1
fi
mkdir -p "$dir" || exit 1
for t in ld ar objcopy strip nm objdump readelf as ranlib size strings c++filt; do
    if [ ! -x "$from-$t" ]; then
        echo "elfwrap: $from-$t is missing; install the $arch-linux-gnu binutils first" >&2
        exit 1
    fi
    ln -sf "$from-$t" "$dir/$arch-elf-$t" || exit 1
done
if ! cc1=$("$from-gcc" -print-prog-name=cc1) || [ ! -f "$cc1" ] ||
    ! identity=$(cksum "$from-gcc" "$cc1"); then
    echo "elfwrap: cannot read the checksums of $from-gcc and its cc1" >&2
    exit 1
fi
tmp=$dir/.$arch-elf-gcc.$$
trap 'rm -f "$tmp"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$tmp" <<EOF || exit 1
#!/bin/sh
# $arch-elf-gcc: $arch-linux-gnu-gcc with a Linux target's defaults turned
# off, written by tools/setup/elfwrap.sh of Rach2 for this compiler:
$(printf '%s\n' "$identity" | sed 's/^/#   /')
exec $from-gcc \\
    -fno-pie -no-pie -fno-stack-protector -fcf-protection=none \\
    -fno-stack-clash-protection -fno-asynchronous-unwind-tables \\
    -fno-unwind-tables -U_FORTIFY_SOURCE -U__linux__ -U__gnu_linux__ -Ulinux \\
    "\$@"
EOF
if [ -f "$dir/$arch-elf-gcc" ] && cmp -s "$tmp" "$dir/$arch-elf-gcc"; then
    chmod 755 "$dir/$arch-elf-gcc" || exit 1
    echo "elfwrap: result: $arch-elf tools in $dir, the wrapper unchanged"
else
    newer=$from-gcc
    [ ! "$cc1" -nt "$newer" ] || newer=$cc1
    chmod 755 "$tmp" && touch -r "$newer" "$tmp" && mv -f "$tmp" "$dir/$arch-elf-gcc" || exit 1
    echo "elfwrap: result: $arch-elf tools in $dir"
fi
