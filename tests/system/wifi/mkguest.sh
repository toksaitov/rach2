#!/bin/sh
#
# mkguest.sh: make the Linux AP guest of the Wi-Fi suite, a Linux kernel and
# an initramfs, from the pinned Alpine packages of packages.txt.
#
# Usage: mkguest.sh OUTDIR
#
# Writes OUTDIR/vmlinuz, linux-lts's kernel, and OUTDIR/initrd, an
# uncompressed newc archive: the packages' files with their modes, the
# modules modules.txt names with those they need, decompressed, and the files
# of root/ and certs/ over them. A built hwsimrelay is staged as
# /usr/sbin/hwsimrelay when HWSIMRELAY names it. Make runs it through the
# target `wifi-guest`, with MKCPIO and SHA256 naming the host programs it
# built and HWSIMRELAY the relay that WIFI_HWSIMRELAY names.
#
# Each package is downloaded once into the cache, RACH2_WIFI_CACHE or
# build/cache/wifi-guest, from RACH2_ALPINE_MIRROR or Alpine's CDN, and
# checked against its digest on every build; a file with another digest is
# removed and refused. The outputs are written beside their names and
# renamed into place, so a failed build leaves the last good guest.

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../../.." && pwd)"
CACHE="${RACH2_WIFI_CACHE:-$REPO/build/cache/wifi-guest}"
MIRROR="${RACH2_ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}"
MKCPIO="${MKCPIO:-$REPO/build/tools/wifi/mkcpio}"
SHA256="${SHA256:-$REPO/build/tools/verify/sha256}"
RELEASE=v3.24

if [ $# -ne 1 ]; then
    echo "usage: mkguest.sh OUTDIR" >&2
    exit 2
fi
mkdir -p "$1"
OUT="$(cd "$1" && pwd)"
# absolute PATH: PATH from the root, for a program run from another directory.
absolute()
{
    case $1 in
        /*) echo "$1" ;;
        *) echo "$PWD/$1" ;;
    esac
}
MKCPIO="$(absolute "$MKCPIO")"
SHA256="$(absolute "$SHA256")"

work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$REPO/build/tmp" "$CACHE"
work="$(mktemp -d "$REPO/build/tmp/rach2-wifiguest.XXXXXX")"
root="$work/root"
mkdir "$root"

# download URL FILE: write URL to FILE, trying up to three times.
download()
{
    tries=1
    while ! curl -sSfL -o "$2" "$1"; do
        [ "$tries" -lt 3 ] || return 1
        tries=$((tries + 1))
        echo "mkguest: retrying: $1" >&2
        sleep 10
    done
}

# fetch REPOSITORY NAME VERSION SHA256: print the checked file's path,
# downloading it if the cache lacks it.
fetch()
{
    file="$CACHE/$2-$3.apk"
    if [ ! -f "$file" ]; then
        url="$MIRROR/$RELEASE/$1/x86_64/$2-$3.apk"
        echo "mkguest: downloading $2-$3.apk" >&2
        if ! download "$url" "$file.part"; then
            rm -f "$file.part"
            echo "mkguest: $2: download failed: $url" >&2
            return 1
        fi
        mv "$file.part" "$file"
    fi
    got="$("$SHA256" "$file" | awk '{print $1}')"
    if [ "$got" != "$4" ]; then
        rm -f "$file"
        echo "mkguest: $2: refused: sha256 $got, pinned $4" >&2
        return 1
    fi
    echo "$file"
}

# unpack FILE DIR MEMBER...: extract the MEMBERs of an .apk, three gzip
# members holding one tar stream, into DIR.
unpack()
{
    file="$1"
    dest="$2"
    shift 2
    mkdir -p "$dest"
    if ! tar -xzf "$file" -C "$dest" "$@" 2> "$work/tar.err"; then
        cat "$work/tar.err" >&2
        echo "mkguest: ${file##*/}: cannot extract" >&2
        return 1
    fi
}

# The packages but the kernel go to mkcpio as tar streams, which it copies
# into the archive without the host's file system, whose names may ignore
# case: iptables has both libxt_MARK.so and libxt_mark.so.
mkdir "$work/packages"
: > "$work/list"
grep -v '^#' "$DIR/packages.txt" | while read -r repo name version digest; do
    [ -n "$name" ] || continue
    file="$(fetch "$repo" "$name" "$version" "$digest")"
    if [ "$name" = linux-lts ]; then
        echo "$file $version" > "$work/linux"
    else
        gzip -dc "$file" > "$work/packages/$name.tar"
        printf 't\t../packages/%s.tar\n' "$name" >> "$work/list"
    fi
done

# The kernel, and the modules: each named one after those modules.dep says
# it needs, which are listed so that the last loads first. Package 6.18.60-r0
# installs its modules under 6.18.60-0-lts.
read -r linux version < "$work/linux"
kver="${version%-r*}-${version##*-r}-lts"
mods="$work/kernel/lib/modules/$kver"
unpack "$linux" "$work/kernel" boot/vmlinuz-lts "lib/modules/$kver/modules.dep"
grep -v '^#' "$DIR/modules.txt" | awk -v dep="$mods/modules.dep" '
    BEGIN {
        while ((getline line < dep) > 0) {
            split(line, f, ": *")
            path = f[1]
            name = path
            sub(/.*\//, "", name)
            sub(/\.ko(\.gz|\.xz|\.zst)?$/, "", name)
            gsub(/-/, "_", name)
            deps[name] = f[2]
            file[name] = path
        }
    }
    NF {
        want = $1
        gsub(/-/, "_", want)
        if (!(want in file)) {
            print "mkguest: module " $1 ": not in modules.dep" > "/dev/stderr"
            exit 1
        }
        n = split(deps[want], d, " ")
        for (i = n; i > 0; i--)
            emit(d[i])
        emit(file[want])
    }
    function emit(p) {
        if (!(p in done)) {
            done[p] = 1
            print p
        }
    }' > "$work/order"
# shellcheck disable=SC2046 # one member a line, and no path has a space
unpack "$linux" "$work/kernel" $(sed "s|^|lib/modules/$kver/|" "$work/order")
mkdir -p "$root/lib/modules" "$root/etc/wifiap"
: > "$root/etc/wifiap/modules"
while read -r path; do
    plain="${path%.gz}"
    mkdir -p "$root/lib/modules/$(dirname "$plain")"
    gzip -dc "$mods/$path" > "$root/lib/modules/$plain"
    case $plain in
        */mac80211_hwsim.ko) echo "$plain" > "$root/etc/wifiap/hwsim" ;;
        *) echo "$plain" >> "$root/etc/wifiap/modules" ;;
    esac
done < "$work/order"
if [ ! -s "$root/etc/wifiap/hwsim" ]; then
    echo "mkguest: modules.txt does not name mac80211_hwsim" >&2
    exit 1
fi

cp -R "$DIR/root/." "$root/"
mkdir -p "$root/etc/wifiap/certs"
cp "$DIR"/certs/*.pem "$DIR"/certs/*.key "$root/etc/wifiap/certs/"
if [ -n "${HWSIMRELAY:-}" ]; then
    mkdir -p "$root/usr/sbin"
    cp "$HWSIMRELAY" "$root/usr/sbin/hwsimrelay"
fi

# Then the files made here, over the packages': the modes come from where a
# file is, since a host such as MSYS2 has no executable bit for a program it
# did not make, and a file's source is relative to the root, where mkcpio
# runs, since a native program on Windows cannot open MSYS2's absolute paths
# written in a file.
(cd "$root" && find .) | LC_ALL=C sort | while read -r p; do
    p="${p#./}"
    [ "$p" != . ] || continue
    if [ -L "$root/$p" ]; then
        printf 'l\t777\t%s\t%s\n' "$p" "$(readlink "$root/$p")"
    elif [ -d "$root/$p" ]; then
        printf 'd\t755\t%s\n' "$p"
    else
        case $p in
            init | bin/* | sbin/* | usr/bin/* | usr/sbin/*) mode=755 ;;
            etc/wifiap/certs/*.key) mode=600 ;;
            *) mode=644 ;;
        esac
        printf 'f\t%s\t%s\t%s\n' "$mode" "$p" "$p"
    fi
done >> "$work/list"

(cd "$root" && "$MKCPIO" "$OUT/initrd.part" "$work/list")
cp "$work/kernel/boot/vmlinuz-lts" "$OUT/vmlinuz.part"
mv "$OUT/vmlinuz.part" "$OUT/vmlinuz"
mv "$OUT/initrd.part" "$OUT/initrd"
echo "mkguest: result: $OUT/vmlinuz and $OUT/initrd, Linux $kver"
