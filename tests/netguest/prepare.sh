#!/bin/sh
#
# prepare.sh: make a gateway guest of the netguest suite, FreeBSD with pf or
# Debian with nftables, from its project's own image.
#
#   prepare.sh freebsd|debian OUT
#
# It runs on a Linux host with QEMU, qemu-img, xorriso, curl, xz and flock,
# and sha512sum for Debian, under KVM where /dev/kvm opens; ACCEL=tcg or
# ACCEL=kvm chooses. OUT is the prepared image, a compressed qcow2 with no
# backing file, written after OUT.record, the guest's `NETGUEST RECORD`
# lines and the host's, so that an OUT that exists has its record.
#
# The system's image is FreeBSD 15.1's BASIC-CLOUDINIT image, which
# freebsd/image pins by URL and SHA-256 and which is fetched into the cache
# and unpacked for the one boot, or the Debian 13 cloud image that
# tests/graphics/debian-ref/image pins, which debian-ref.sh fetches and
# checks into its own cache. The script boots an overlay of it once, with a
# seed, an ISO 9660 volume labelled cidata that holds the system's
# meta-data and a user-data that is a shell script: the lines that write
# the system's netguest.sh, the guest's boot script, to /root/netguest.sh,
# then the system's setup.sh. nuageinit on FreeBSD and cloud-init on Debian
# run it at the end of that boot; it installs the boot script for every
# later boot, turns nuageinit or cloud-init off and powers the guest off.
# FreeBSD reads the seed from a CD drive and Debian from a virtio disk. The
# FreeBSD boot has QEMU's user network closed to the outside
# (`restrict=on`), since its setup needs no package and the image would
# otherwise upgrade itself on its first boot. Debian's has it open, for the
# nftables package the cloud image lacks, which comes from the snapshot
# that debian-ref's image names.
#
# The guest's serial console goes to a file, which the script reads until
# it holds `NETGUEST PREPARED` or `NETGUEST FAILED reason`, or QEMU ends.
# The preparation has no time limit; its caller's bound ends a boot that
# hangs. After PREPARED the guest powers itself off. The cache,
# build/cache/netguest of this tree or NETGUEST_CACHE, keeps the downloads,
# SYSTEM.qcow2 with SYSTEM.key, the digest of what it was made from, and
# SYSTEM.console.log, the console of the last preparation, kept after a
# failure too; a cached image whose key is the inputs' digest is linked or
# copied to OUT without a boot. The key covers this script, the system's
# files, its image's pins and, for Debian, debian-ref.sh, and is taken once,
# before the boot. A download enters the cache only once its digest checks.
# One preparation runs on a cache at a time, under flock. The last line is
# `netguest-prepare: result: prepared OUT` or `netguest-prepare: result:
# fail: REASON`, with status 0 or 1; a usage error is status 2.
#
# A prepared guest takes its parameters and reports as its netguest.sh
# says: one line in the first sector of its second virtio disk,
# `rules=N opt=none|basic state=1|0 fwd=PORT filter=1|0`, and on its serial
# console `NETGUEST READY inner=IF outer=IF addr=A.B.C.D rules=N opt=O
# state=S filter=F` or `NETGUEST FAILED reason`.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../.." && pwd)
cache=${NETGUEST_CACHE:-$tree/build/cache/netguest}
debian_ref=$tree/tests/graphics/debian-ref
# The processors and memory of the preparing boot.
cpus=2 mib=2048

usage()
{
    echo "usage: prepare.sh freebsd|debian OUT" >&2
    exit 2
}

die()
{
    echo "netguest-prepare: result: fail: $*"
    exit 1
}

# pin FILE NAME: print the value FILE gives NAME.
pin()
{
    awk -v n="$2" '$1 == n { print $2 }' "$1"
}

# inputs_key: print the digest of what the system's prepared image is made
# from: this script, its recipe, its image's pins, the script that fetches
# Debian's image, and the words of its boot.
inputs_key()
{
    {
        cat "$here/prepare.sh" "$here/$system/setup.sh" "$here/$system/netguest.sh" \
            "$here/$system/meta-data"
        case $system in
            freebsd) cat "$here/freebsd/image" ;;
            debian) cat "$debian_ref/image" "$debian_ref/debian-ref.sh" ;;
        esac
        echo "cpus $cpus mib $mib"
    } | sha256sum | cut -d ' ' -f 1
}

# The preparing boot's process and scratch directory.
qemu='' work=''
# shellcheck disable=SC2317 # the trap calls it
cleanup()
{
    [ -z "$qemu" ] || kill "$qemu" 2> /dev/null || :
    [ -z "$qemu" ] || wait "$qemu" 2> /dev/null || :
    if [ -n "$work" ]; then
        [ ! -f "$work/console.log" ] || cp "$work/console.log" "$cache/$system.console.log"
        rm -rf "$work"
    fi
    rm -f "$out.part.$$" "$out.record.part.$$" "$cache/$system.qcow2.part.$$" \
        "$cache/$system.record.part.$$"
}

# base_image: set `base` to the system's image, a qcow2, and `named` to
# the line of the record that names it.
base_image()
{
    case $system in
        freebsd)
            url=$(pin "$here/freebsd/image" url)
            sum=$(pin "$here/freebsd/image" sha256)
            packed=$cache/$(basename "$url")
            if [ -f "$packed" ] && ! echo "$sum  $packed" | sha256sum -c --quiet - 2> /dev/null; then
                echo "netguest-prepare: $packed has another SHA-256; fetching it again"
                rm -f "$packed"
            fi
            if [ ! -f "$packed" ]; then
                echo "netguest-prepare: fetching $url"
                curl -sSfL -o "$packed.part.$$" "$url" || {
                    rm -f "$packed.part.$$"
                    die "cannot fetch $url"
                }
                echo "$sum  $packed.part.$$" | sha256sum -c --quiet - || {
                    rm -f "$packed.part.$$"
                    die "$url: SHA-256 differs"
                }
                mv "$packed.part.$$" "$packed"
            fi
            base=$work/base.qcow2
            xz -dc "$packed" > "$base" || die "cannot unpack $packed"
            named="image $(basename "$url") sha256 $sum"
            ;;
        debian)
            base=$(sh "$debian_ref/debian-ref.sh" fetch |
                sed -n 's/^debian-ref: result: \(.*\) checks$/\1/p')
            [ -f "$base" ] || die "debian-ref.sh fetch named no checked image"
            named="image $(basename "$base") sha512 $(pin "$debian_ref/image" sha512)"
            ;;
    esac
}

# boot: prepare the system in a boot of an overlay of its image, and leave
# the result in the cache as SYSTEM.qcow2 with its key and record.
boot()
{
    work=$(mktemp -d "$cache/prepare-$system.XXXXXX") || die "no scratch directory in $cache"
    mkdir -p "$work/seed"
    base_image

    snapshot=$(pin "$debian_ref/image" snapshot)
    {
        echo '#!/bin/sh'
        echo "cat > /root/netguest.sh << 'NETGUEST_EOF'"
        cat "$here/$system/netguest.sh"
        echo NETGUEST_EOF
        sed -e 1d -e "s|@SNAPSHOT@|$snapshot|g" "$here/$system/setup.sh"
    } > "$work/seed/user-data"
    cp "$here/$system/meta-data" "$work/seed/meta-data"
    xorriso -as mkisofs -quiet -o "$work/seed.iso" -V cidata -J -r "$work/seed" ||
        die "the seed was not made"
    qemu-img create -q -f qcow2 -F qcow2 -b "$base" "$work/disk.qcow2" ||
        die "the overlay of $base was not made"

    if [ "${ACCEL:-}" = kvm ] || { [ -z "${ACCEL:-}" ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; }; then
        accel=kvm cpu=host
    else
        accel=tcg cpu=max
    fi
    console=$work/console.log
    : > "$console"
    set -- -nodefaults -no-reboot -display none -machine q35 \
        -accel "$accel" -cpu "$cpu" -smp "$cpus" -m "$mib" \
        -drive "file=$work/disk.qcow2,if=virtio,format=qcow2" \
        -device virtio-net-pci,netdev=net0 -device virtio-rng-pci -device VGA \
        -serial "file:$console" -monitor none
    case $system in
        freebsd)
            set -- "$@" -netdev user,id=net0,restrict=on \
                -drive "file=$work/seed.iso,if=none,id=seed,format=raw,readonly=on" \
                -device ide-cd,drive=seed,bus=ide.1
            ;;
        debian)
            set -- "$@" -netdev user,id=net0 \
                -drive "file=$work/seed.iso,if=virtio,format=raw,readonly=on"
            ;;
    esac
    echo "netguest-prepare: preparing $system under $accel; console in $console"
    qemu-system-x86_64 "$@" < /dev/null &
    qemu=$!

    # Read the console until the setup reports or QEMU ends.
    while kill -0 "$qemu" 2> /dev/null; do
        ! grep -a -q -E 'NETGUEST (PREPARED|FAILED)' "$console" || break
        sleep 2
    done
    grep -a 'NETGUEST ' "$console" | tr -d '\r' | sed 's/^.*NETGUEST /netguest-prepare: guest: /'
    if grep -a -q 'NETGUEST FAILED' "$console"; then
        die "$system: the setup reported $(grep -a 'NETGUEST FAILED' "$console" | tr -d '\r' |
            sed 's/^.*NETGUEST //' | tail -n 1)"
    fi
    if ! grep -a -q 'NETGUEST PREPARED' "$console"; then
        tr -d '\r' < "$console" | grep -a . | tail -n 20 | sed 's/^/netguest-prepare: console: /'
        die "$system: QEMU ended before the setup reported"
    fi
    wait "$qemu" || die "$system: QEMU exited with an error"
    qemu=''

    rm -f "$cache/$system.qcow2" "$cache/$system.key"
    part=$cache/$system.qcow2.part.$$
    qemu-img convert -c -O qcow2 "$work/disk.qcow2" "$part" ||
        die "$system: the prepared image was not written"
    info=$(qemu-img info "$part") || die "$system: qemu-img cannot read the prepared image"
    if echo "$info" | grep -q '^backing file'; then
        die "$system: the prepared image has a backing file"
    fi
    chmod a-w "$part"
    {
        grep -a 'NETGUEST RECORD ' "$console" | tr -d '\r' | sed 's/^.*NETGUEST RECORD //'
        echo "$named"
        echo "accelerator $accel"
        echo "qemu $(qemu-system-x86_64 --version | head -n 1)"
        echo "bytes $(wc -c < "$part")"
    } > "$cache/$system.record.part.$$" || die "$system: the record was not written"
    mv "$cache/$system.record.part.$$" "$cache/$system.record"
    mv "$part" "$cache/$system.qcow2"
    echo "$key" > "$cache/$system.key"
}

[ $# -eq 2 ] || usage
system=$1 out=$2
case $system in
    freebsd | debian) ;;
    *) usage ;;
esac
tools='qemu-system-x86_64 qemu-img xorriso sha256sum curl xz flock'
[ "$system" = freebsd ] || tools="$tools sha512sum"
for tool in $tools; do
    command -v "$tool" > /dev/null || die "no $tool on this host"
done
! grep -q -x NETGUEST_EOF "$here/$system/netguest.sh" ||
    die "$system/netguest.sh has the line that ends it on the seed"
mkdir -p "$cache"
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
# One preparation of a cache at a time.
exec 9>> "$cache/prepare.lock"
flock 9 || die "cannot lock $cache/prepare.lock"
key=$(inputs_key)
if [ ! -f "$cache/$system.qcow2" ] || [ ! -f "$cache/$system.record" ] ||
    [ "$(cat "$cache/$system.key" 2> /dev/null)" != "$key" ]; then
    boot
fi
mkdir -p "$(dirname "$out")"
# The record first, then the image, each whole before it takes its name; a
# link where the file system allows it, since the image is a gigabyte or
# more.
cp "$cache/$system.record" "$out.record.part.$$" || die "the record was not copied beside $out"
mv -f "$out.record.part.$$" "$out.record" || die "the record was not kept beside $out"
ln "$cache/$system.qcow2" "$out.part.$$" 2> /dev/null || cp "$cache/$system.qcow2" "$out.part.$$" ||
    die "the image was not copied to $out"
mv -f "$out.part.$$" "$out"
echo "netguest-prepare: result: prepared $out"
