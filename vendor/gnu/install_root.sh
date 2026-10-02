#!/usr/bin/env sh

#
# install_root.sh: install the GNU system over a staged root.
#
# Usage: install_root.sh TREE
#
# tools/image/mkext2.sh runs this over the tree it staged from
# krffs/manifests/os-real-gnu.manifest, before it formats the volume, so that
# the GNU system lies over the os-real system as a Debian i386 system has
# it. From build/vendor/gnu it copies in:
#
#   root      glibc's runtime, with its loader as /lib/ld-linux.so.2, and the
#             ports under /usr, with their files in /etc
#   native    GCC and binutils
#   gdb       GDB and gdbserver
#   js        the SpiderMonkey shell
#   chromium  chromium-headless-shell in /usr/lib/chromium and the libraries
#             it needs in /usr/lib/i386-linux-gnu, as Debian's packages have
#             them, from the packages fetch_chromium.sh unpacked
#
# Then it links /bin/bash, /bin/sh, /bin/apropos, /bin/whatis, /usr/bin/cc,
# /usr/bin/vi, /bin/mkfs.ext2, /etc/mtab and /run, writes autoconf's site file,
# links each BusyBox applet no file of the path answers for, gives each program
# of /bin and /usr/bin its name in the other, and of /sbin and /usr/sbin, puts
# the CA bundle in /etc/ssl, puts /etc/services, /etc/protocols, /etc/networks,
# /etc/localtime and /etc/timezone in place, compiles the console's terminfo
# entry, rach2.ti, renders the ports' manual pages with render_man.sh, takes the
# set-id bit off OpenSSH's ssh-keysign, and has the root's own ldconfig write
# /etc/ld.so.cache from the /etc/ld.so.conf the manifest staged. A file the tree
# already has is refused rather than replaced, so that what the manifest staged
# is what the image has; the one file replaced is named where it is, and
# /bin/sh, which the manifest does not stage, is a name of bash's file.
#

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
OUT="$REPO/build/vendor/gnu"
DEBIAN="$OUT/work/chromium"

# The packages of the browser's closure that are not installed: libc6,
# whose glibc 2.41 the root's 2.44 replaces; the libraries the root has its
# own builds of, of the same sonames and newer: GCC's in libgcc-s1,
# libstdc++6 and libatomic1, zlib1g, liblzma5, libpcre2-8-0, util-linux's
# libmount1, libblkid1 and libuuid1, libffi8, libsqlite3-0, libbz2-1.0 and
# libzstd1, with gcc-14-base, which only documents GCC's three; and the
# programs chromium-common's dependencies bring that a headless shell does
# not run, in debconf, lsb-base, sysvinit-utils, x11-utils and xdg-utils. A
# library of both would be found in Debian's directory first, which
# /etc/ld.so.conf lists, by the root's programs too, and util-linux's mount
# asks its libmount for a version Debian's lacks.
SKIP="libc6 libgcc-s1 libstdc++6 libatomic1 zlib1g liblzma5 libpcre2-8-0 libmount1 libblkid1
libuuid1 libffi8 libsqlite3-0 libbz2-1.0 libzstd1 gcc-14-base debconf lsb-base sysvinit-utils
x11-utils xdg-utils"

TREE="$1"
if [ -z "$TREE" ] || [ ! -d "$TREE" ]; then
    echo "usage: install_root.sh TREE" >&2
    exit 2
fi
for part in root native gdb js chromium; do
    if [ ! -d "$OUT/$part" ]; then
        echo "gnu: install: no $OUT/$part; run make gnu-ports gnu-native gnu-gdb gnu-js gnu-chromium" >&2
        exit 1
    fi
done
if [ ! -f "$DEBIAN/closure.txt" ]; then
    echo "gnu: install: no $DEBIAN/closure.txt; run make gnu-chromium" >&2
    exit 1
fi

WORK="$(mktemp -d "$TREE.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT INT TERM

# skipped NAME: whether the package NAME is one of SKIP.
skipped()
{
    for s in $SKIP; do
        [ "$s" = "$1" ] && return 0
    done
    return 1
}

# nodes DIR: every path under DIR that is not a directory, sorted.
nodes()
{
    (cd "$1" && find . ! -type d) | LC_ALL=C sort
}

# fresh LIST: fail when a path of the sorted LIST is already in the tree.
fresh()
{
    nodes "$TREE" > "$WORK/have"
    LC_ALL=C comm -12 "$WORK/have" "$1" > "$WORK/both"
    if [ -s "$WORK/both" ]; then
        echo "gnu: install: these would replace files the root already has:" >&2
        head -n 20 "$WORK/both" >&2
        exit 1
    fi
}

# install_part PART: copy build/vendor/gnu/PART over the tree. The Info
# directory index lists only the manuals of the part that wrote it, so none
# is kept, as build_port.sh keeps none.
install_part()
{
    nodes "$OUT/$1" > "$WORK/new"
    fresh "$WORK/new"
    cp -a "$OUT/$1/." "$TREE/"
    rm -f "$TREE/usr/share/info/dir"
    echo "gnu: install: $1: $(wc -l < "$WORK/new" | tr -d ' ') files"
}

install_part root
# The runtime's libstdc++.so.6 is a copy of the cross compiler's library,
# and the native compiler has the same library of GCC 16.2.0 with the links
# and archives a link against it needs, so its copy replaces the runtime's.
rm -f "$TREE/usr/lib/libstdc++.so.6"
install_part native
install_part gdb
install_part js

# The browser's packages. Each package's list is the listing of its unpack,
# where /bin, /lib and /sbin were links into /usr, as trixie has them; here
# they are directories of the musl system, so those paths are taken under
# /usr, where the unpack put their files. The archive is written whole
# before it is read, so that a failure to write it stops the install.
: > "$WORK/debian"
while read -r name _; do
    skipped "$name" && continue
    sed -e 's|^\./bin/|./usr/bin/|' -e 's|^\./lib/|./usr/lib/|' \
        -e 's|^\./sbin/|./usr/sbin/|' -e 's|/$||' "$DEBIAN/files/$name" >> "$WORK/debian"
done < "$DEBIAN/closure.txt"
grep -v '^\.$' "$WORK/debian" | LC_ALL=C sort -u > "$WORK/debian.list"
nodes "$OUT/chromium" > "$WORK/debian.have"
LC_ALL=C comm -12 "$WORK/debian.list" "$WORK/debian.have" > "$WORK/new"
fresh "$WORK/new"
tar -cf "$WORK/debian.tar" -C "$OUT/chromium" --no-recursion -T "$WORK/debian.list"
tar -xpf "$WORK/debian.tar" -C "$TREE"
echo "gnu: install: chromium: $(wc -l < "$WORK/new" | tr -d ' ') files"

# bash is /bin/bash, cc is GCC and vi is Vim, as on a Debian system. The
# links are relative, so they resolve wherever the volume is mounted. bash is
# /bin/sh too, a second name of its file, since the set-id guard requires the
# shell root runs to be a file: BusyBox's ash runs its own applet for a name
# before it searches the path, so a script, system(3) and a package's test
# suite would get BusyBox's sed where they name GNU's.
ln -s ../usr/bin/bash "$TREE/bin/bash"
ln "$TREE/usr/bin/bash" "$TREE/bin/sh"

# The system's man answers to apropos and whatis, as man-db's does.
ln -s man "$TREE/bin/apropos"
ln -s man "$TREE/bin/whatis"
ln -s gcc "$TREE/usr/bin/cc"
ln -s vim "$TREE/usr/bin/vi"

# The manifest's mke2fs answers to mkfs.ext2 too, the name util-linux's
# mkfs runs for `mkfs -t ext2` and Debian's e2fsprogs links. mkfs.ext3 and
# mkfs.ext4 are left out, since the kernel mounts neither's volumes.
ln -s mke2fs "$TREE/bin/mkfs.ext2"

# The mount table where glibc's getmntent and GNU df look for it, as a
# Debian system links it.
ln -s ../proc/self/mounts "$TREE/etc/mtab"

# /run, where D-Bus's programs look for the system bus's socket,
# /run/dbus/system_bus_socket: a link to /var/run, which init empties at
# boot as Linux keeps /run in memory.
ln -s var/run "$TREE/run"

# The site file autoconf's configure reads, for the default prefix and for
# /usr: config.guess knows no system by the kernel's name, Rach2, and stops
# every configure that has no --build, so the build is named the GNU/Linux
# system whose programs the kernel runs.
mkdir -p "$TREE/usr/local/share"
cat > "$TREE/usr/share/config.site" << 'EOF'
# config.site: answers for autoconf configure scripts on Rach2.
test -n "$build_alias" || build_alias=i686-pc-linux-gnu
EOF
ln -s ../../share/config.site "$TREE/usr/local/share/config.site"

# BusyBox's applets are not files: ash runs one for its name before it
# searches the path, and bash and every program that searches the path
# itself, as Git does for its editor, find only files. So each applet that
# no file in /usr/bin, /bin, /usr/sbin or /sbin answers for, as ps, mount
# and which, is linked to /bin/busybox where BusyBox's own install puts it.
# The build host runs the BusyBox the manifest stages, which is static, for
# the list, from the build: the stager writes the tree's files without their
# modes, which mkext2.sh gives the volume afterwards.
"$REPO/build/vendor/busybox/busybox" --list-full > "$WORK/applets"
applets=0
while IFS= read -r applet; do
    name=${applet##*/}
    for d in usr/bin bin usr/sbin sbin; do
        if [ -e "$TREE/$d/$name" ] || [ -L "$TREE/$d/$name" ]; then
            continue 2
        fi
    done
    case $applet in
        usr/*) target=../../bin/busybox ;;
        sbin/*) target=../bin/busybox ;;
        *) target=busybox ;;
    esac
    ln -s "$target" "$TREE/$applet"
    applets=$((applets + 1))
done < "$WORK/applets"
echo "gnu: install: busybox: $applets applets linked"

# Debian's /bin and /sbin are links into /usr, so every program has both
# names, and a script that names /bin/true or /usr/sbin/ldconfig finds it.
# Here the four are directories, so each program one of a pair lacks is
# linked from the other. A name both have keeps its two programs.
links=0
for pair in bin:usr/bin usr/bin:bin sbin:usr/sbin usr/sbin:sbin; do
    from=${pair%%:*}
    to=${pair#*:}
    case $to in
        usr/*) up=../.. ;;
        *) up=.. ;;
    esac
    for f in "$TREE/$from"/*; do
        name=${f##*/}
        if [ ! -e "$TREE/$to/$name" ] && [ ! -L "$TREE/$to/$name" ]; then
            ln -s "$up/$from/$name" "$TREE/$to/$name"
            links=$((links + 1))
        fi
    done
done
echo "gnu: install: /bin and /usr/bin, /sbin and /usr/sbin: $links names linked"

# The CA bundle, where OpenSSL looks by default, /etc/ssl/cert.pem, which
# curl and Git use for HTTPS, and under the name Debian gives it.
cp "$("$DIR/fetch.sh" --path cacert)" "$TREE/etc/ssl/certs/ca-certificates.crt"
chmod 0644 "$TREE/etc/ssl/certs/ca-certificates.crt"
ln -s certs/ca-certificates.crt "$TREE/etc/ssl/cert.pem"

# The service, protocol and network names glibc's getservbyname,
# getprotobyname and getnetbyname read, which Debian's netbase gives, from
# IANA's numbers in vendor/gnu/etc; and the zone the clock is kept in, UTC,
# as /etc/localtime and /etc/timezone name it on Debian.
for f in services protocols networks timezone localtime; do
    if [ -e "$TREE/etc/$f" ] || [ -L "$TREE/etc/$f" ]; then
        echo "gnu: install: refused: /etc/$f is already in the tree" >&2
        exit 1
    fi
done
for f in services protocols networks; do
    cp "$DIR/etc/$f" "$TREE/etc/$f"
    chmod 0644 "$TREE/etc/$f"
done
ln -s ../usr/share/zoneinfo/Etc/UTC "$TREE/etc/localtime"
printf 'Etc/UTC\n' > "$TREE/etc/timezone"

# The console's entry, for the TERM=rach2 the kernel gives its sessions. The
# build host runs the root's tic through the root's loader.
"$TREE/lib/ld-linux.so.2" --library-path "$TREE/lib:$TREE/usr/lib" \
    "$TREE/usr/bin/tic" -o "$TREE/usr/share/terminfo" "$DIR/rach2.ti"

# The ports' manual pages, rendered to the plain text the system's man
# reads, which it looks in after the project's own pages.
sh "$DIR/render_man.sh" "$TREE/usr/share/man"

# ssh-keysign is set-id root so that host-based authentication can read the
# host keys. The system uses no host-based authentication, and a set-id file
# the manifest does not declare fails the set-id guard, so it has no bit.
chmod u-s "$TREE/usr/libexec/ssh-keysign"

# ldconfig is linked statically, as glibc always links it, so the build host
# runs the root's own without its loader: -r reads the tree's ld.so.conf and
# writes the tree's cache, and -X leaves the links in the library
# directories as the packages made them.
"$TREE/sbin/ldconfig" -X -r "$TREE"
echo "gnu: install: ld.so.cache: $("$TREE/sbin/ldconfig" -r "$TREE" -p | sed -n 1p)"
