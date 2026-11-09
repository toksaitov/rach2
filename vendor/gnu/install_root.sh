#!/usr/bin/env sh

#
# install_root.sh: install the GNU system over a staged root.
#
# Usage: install_root.sh TREE
#
# tools/image/mkext2.sh runs this over the tree it staged from
# krffs/manifests/os-real-gnu.manifest, before it formats the volume, so that
# the image is the os-real system with the GNU system installed over it, laid
# out as a Debian system is. From the GNU layer's build tree for the
# architecture RACH2_ARCH names (vendor/gnu/arch.sh) it copies in:
#
#   root      glibc's runtime, with its loader at /lib/ld-linux.so.2 on i686
#             and /lib64/ld-linux-x86-64.so.2 on x86-64, and the ports under
#             /usr, with their files in /etc
#   native    GCC and binutils
#   gdb       GDB and gdbserver
#   js        the SpiderMonkey shell
#   debian    the desktop closure, including chromium-headless-shell
#   proton    GE-Proton under /opt/proton, on x86-64
#   webgl     the pinned conformance suite under /usr/share/webgl
#   igt       IGT's tests under /usr/libexec/igt-gpu-tools and its runner,
#             when GNU_IGT is 1, on x86-64
#   agents    the packages of the agents layer that GNU_AGENTS lists, each as
#             fetch_agents.sh staged it, on x86-64 only
#
# Then it makes /usr/local and its directories, links
# /usr/bin/chromium-headless-shell, in place of Debian's script, to the
# browser, and links /bin/bash, /bin/sh, /bin/apropos, /bin/whatis,
# /usr/bin/cc, /usr/bin/vi, /bin/mkfs.ext2, /sbin/fsck.ext2, /sbin/e2label,
# /etc/mtab and /run, links each BusyBox applet that no file of the path
# provides, gives each program of /bin and /usr/bin its name in the other, and
# of /sbin and /usr/sbin, puts the CA bundle in /etc/ssl, puts /etc/services,
# /etc/protocols, /etc/networks, /etc/localtime and /etc/timezone in place,
# compiles the console's terminfo entry, rach2.ti, checks that ncurses's
# database has term's xterm-256color, renders the ports' manual pages with
# render_man.sh, gives every node its owner's write permission, takes the
# set-id bit off OpenSSH's ssh-keysign, sets the modes of the ports that are
# narrower than the store keeps, gives /etc/shadow a locked line for each
# system account of /etc/passwd, and has the root's own ldconfig write
# /etc/ld.so.cache from the /etc/ld.so.conf the manifest staged. A file the
# tree already has is refused rather than replaced, so that what the manifest
# staged is what the image has; the two files replaced and the one added to
# are named where they are, and /bin/sh, which the manifest does not stage, is
# a name of bash's file.
#

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC2034 # arch.sh reads it
REPO="$(cd "$DIR/../.." && pwd)"
. "$DIR/arch.sh"
OUT="$GNU_OUT"
DEBIAN="$OUT/work/debian/desktop"

# Libraries and programs already supplied by the GNU ports or image manifest
# are kept out of Debian's search directory. Distinct sonames (OpenSSL 3,
# FFmpeg 7 and Python 3.13) stay beside the ports' newer ABI versions.
SKIP="libc6 libgcc-s1 libstdc++6 libatomic1 libgomp1 zlib1g liblzma5 libpcre2-8-0
libmount1 libblkid1 libuuid1 libsmartcols1 libfdisk1 libffi8 libsqlite3-0 libbz2-1.0
libzstd1 libcrypt1 libcap2 libcap2-bin libevent-2.1-7t64 libgcrypt20 libgpg-error0
libreadline8t64 libncurses6 libncursesw6 libbrotli1 gcc-14-base debconf
lsb-base sysvinit-utils base-files dpkg debianutils init-system-helpers systemd
systemd-sysv dbus-user-session gawk mount procps tar tzdata netbase python3
python3-minimal libpython3-stdlib readline-common libpam-modules
libpam-modules-bin libpam-runtime"

TREE="$1"
if [ -z "$TREE" ] || [ ! -d "$TREE" ]; then
    echo "usage: install_root.sh TREE" >&2
    exit 2
fi
for part in root native gdb js debian/desktop proton webgl; do
    if [ ! -d "$OUT/$part" ]; then
        echo "gnu: install: no $OUT/$part; run make gnu-ports gnu-native gnu-gdb gnu-js gnu-desktop gnu-proton gnu-webgl" >&2
        exit 1
    fi
done
if [ ! -f "$DEBIAN/closure.txt" ]; then
    echo "gnu: install: no $DEBIAN/closure.txt; run make gnu-desktop" >&2
    exit 1
fi
if [ "${GNU_IGT:-}" = 1 ] && [ ! -d "$OUT/igt" ]; then
    echo "gnu: install: no $OUT/igt; run make ARCH=x86_64 gnu-igt" >&2
    exit 1
fi
for name in ${GNU_AGENTS:-}; do
    if [ "$RACH2_ARCH" != x86_64 ]; then
        echo "gnu: install: the agents layer's packages are for x86-64 only" >&2
        exit 1
    fi
    if [ ! -d "$OUT/agents/$name" ]; then
        echo "gnu: install: no $OUT/agents/$name; run make ARCH=x86_64 GNU_AGENTS=$name gnu-agents" >&2
        exit 1
    fi
done

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

# writable: give the owner write permission on every node of the tree. A
# part that comes from the host's store of step outputs is read-only, and the
# volume must not depend on where the layer was built, so every node is
# root's to write, whatever mode the build gave it, and the few narrower
# modes are set below.
writable()
{
    chmod -R u+w "$TREE"
}

# install_part PART: copy PART of the build tree over the tree. The Info
# directory index lists only the manuals of the part that wrote it, so none
# is kept, as build_port.sh keeps none.
install_part()
{
    nodes "$OUT/$1" > "$WORK/new"
    fresh "$WORK/new"
    cp -a "$OUT/$1/." "$TREE/"
    writable
    rm -f "$TREE/usr/share/info/dir"
    echo "gnu: install: $1: $(wc -l < "$WORK/new" | tr -d ' ') files"
}

install_part root
if [ "${GNU_TEST_PRODUCTS:-}" = 1 ]; then
    # The manifest assigns this disk-backed workspace to the test account.
    mkdir -p "$TREE/host-work"
    chmod 0700 "$TREE/host-work"
    mkdir "$TREE/host-products"
    cp -a "$OUT/host-products/." "$TREE/host-products/"
    chmod u+w "$TREE/host-products"
    cp "$REPO/build/tests/host-suite/scripts-assets.tar" "$TREE/host-products/scripts-assets.tar"
    mkdir -p "$TREE/host-products/diskpolicy/$RACH2_ARCH"
    cp "$ARCH_BUILD/rach2/kernel" \
        "$TREE/host-products/diskpolicy/$RACH2_ARCH/readwrite.kernel"
    cp "$ARCH_BUILD/rach2-metal/kernel" \
        "$TREE/host-products/diskpolicy/$RACH2_ARCH/readonly.kernel"
    # Install only the isolated helper tools, without changing root/.
    for tool in mkfs.exfat fsck.exfat; do
        test -x "$OUT/fstools/usr/sbin/$tool"
        cp "$OUT/fstools/usr/sbin/$tool" "$TREE/usr/sbin/$tool"
    done
    # Independent filesystem commands are required prepared inputs.
    for tool in mkfs.exfat fsck.exfat fsck.fat; do
        test -x "$TREE/usr/sbin/$tool"
    done
    test -x "$TREE/host-products/fstool"
    if [ "$RACH2_ARCH" = x86_64 ]; then
        # The store's directory mode must permit the nested fixture copy.
        chmod u+w "$TREE/host-products"
        mkdir "$TREE/host-products/host-checks"
        cp -a "$OUT/host-checks/." "$TREE/host-products/host-checks/"
    fi
    writable
fi
# Build the framed WebSocket fixture against the same TLS libraries as the
# guest tools. This happens in the GNU build environment on every host.
make --no-print-directory -C "$REPO" -f tests/cases/host/products.mk \
    CC="$OUT/cross/bin/$GNU_TARGET-gcc --sysroot=$OUT/root" \
    PREFIX="$TREE" "$TREE/etude-wsclient"
# The runtime's libstdc++.so.6 is a copy of the cross compiler's library,
# and the native compiler has the same library of the same GCC with the links
# and archives a link against it needs, so its copy replaces the runtime's.
rm -f "$TREE/usr/lib/libstdc++.so.6"
install_part native
install_part gdb
install_part js
if [ "${GNU_TEST_PRODUCTS:-}" = 1 ]; then
    cp -a "$OUT/sanitizers/root/." "$TREE/"
    writable
fi

# install_debian LAYER: copy a named closure, refusing replaced root files.
install_debian()
{
    meta="$OUT/work/debian/$1"
    stage="$OUT/debian/$1"
    # Each package's list is the listing of its unpack, where /bin, /lib
    # and /sbin were links into /usr, as trixie has them; here
    # they are directories of the musl system, so those paths are taken under
    # /usr, where the unpack put their files. The archive is written whole
    # before it is read, so a failed write stops the install. Its list
    # has each directory's whole contents right after it, sorted with the slash
    # below every other byte, since tar gives a directory its mode once it has
    # left it, and a mode from the store has no write permission.
    : > "$WORK/debian"
    while read -r name _; do
        skipped "$name" && continue
        sed -e 's|^\./bin/|./usr/bin/|' -e 's|^\./lib/|./usr/lib/|' \
            -e 's|^\./sbin/|./usr/sbin/|' -e 's|/$||' "$meta/files/$name" >> "$WORK/debian"
    done < "$meta/closure.txt"
    grep -v '^\.$' "$WORK/debian" | LC_ALL=C sort -u > "$WORK/debian.list"
    nodes "$stage" > "$WORK/debian.have"
    LC_ALL=C comm -12 "$WORK/debian.list" "$WORK/debian.have" > "$WORK/new"
    fresh "$WORK/new"
    tr / '\001' < "$WORK/debian.list" | LC_ALL=C sort | tr '\001' / > "$WORK/debian.order"
    tar -cf "$WORK/debian.tar" -C "$stage" --no-recursion -T "$WORK/debian.order"
    tar -xpf "$WORK/debian.tar" -C "$TREE"
    writable
    # Debian's executable Python scripts use its matching dist-packages.
    python3 - "$TREE" "$WORK/debian.list" <<'PYTHON'
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
for relative in pathlib.Path(sys.argv[2]).read_text().splitlines():
    path = root / relative.removeprefix('./')
    if path.is_symlink() or not path.is_file() or not path.stat().st_mode & 0o111:
        continue
    with path.open('rb') as stream:
        if stream.read(2) != b'#!':
            continue
        first = b'#!' + stream.readline()
    versioned = re.sub(rb'^#![ \t]*/usr/bin/(?:env[ \t]+)?python3(?=[ \t\r\n]|$)',
                       b'#!/usr/bin/python3.13', first)
    if first != versioned:
        data = path.read_bytes()
        path.write_bytes(versioned + data[len(first):])
PYTHON
    echo "gnu: install: $1: $(wc -l < "$WORK/new" | tr -d ' ') files"

}

install_debian desktop

# Debian's /usr/bin/chromium-headless-shell is a shell script that runs the
# browser as its child, so a signal to the process a caller started reaches
# only the shell, and the browser is left running. A link starts the browser
# as that process; the browser finds its data beside /proc/self/exe, which
# names the file in /usr/lib/chromium.
if [ ! -x "$TREE/usr/lib/chromium/chromium-headless-shell" ] ||
    ! grep -q '^/usr/lib/chromium/chromium-headless-shell "\$@"$' \
        "$TREE/usr/bin/chromium-headless-shell"; then
    echo "gnu: install: refused: /usr/bin/chromium-headless-shell is not the launcher this script replaces" >&2
    exit 1
fi
rm "$TREE/usr/bin/chromium-headless-shell"
ln -s ../lib/chromium/chromium-headless-shell "$TREE/usr/bin/chromium-headless-shell"

install_part proton
install_part webgl
if [ "${GNU_IGT:-}" = 1 ]; then
    install_part igt
fi

# These alternatives are made by Debian's postinst, which is not run here.
for name in blas lapack; do
    libdir="$TREE/usr/lib/$GNU_MULTIARCH"
    if [ -f "$libdir/$name/lib$name.so.3" ]; then
        ln -s "$name/lib$name.so.3" "$libdir/lib$name.so.3"
    fi
done

for name in ${GNU_AGENTS:-}; do
    install_part "agents/$name"
done

# bash is /bin/bash, cc is GCC and vi is Vim, as on a Debian system. The
# links are relative, so they resolve wherever the volume is mounted. bash is
# /bin/sh too, a second name of its file, since the set-id guard requires the
# shell root runs to be a file: BusyBox's ash runs its own applet for a name
# before it searches the path, so a script, system(3) and a package's test
# suite would get BusyBox's sed where they name GNU's.
ln -s ../usr/bin/bash "$TREE/bin/bash"
ln "$TREE/usr/bin/bash" "$TREE/bin/sh"

# /usr/local, which the path searches first, for what a person installs
# beside the system's packages, laid out as Debian's base-files makes it.
for d in bin etc include lib sbin share src; do
    mkdir -p "$TREE/usr/local/$d"
done
chmod 0755 "$TREE/usr/local" "$TREE/usr/local"/*

# apropos and whatis are names of the system's man, which searches as each
# does when it is run by that name, as man-db's are.
ln -s man "$TREE/bin/apropos"
ln -s man "$TREE/bin/whatis"
ln -s gcc "$TREE/usr/bin/cc"
ln -s vim "$TREE/usr/bin/vi"

# mkfs.ext2 is a name of the manifest's mke2fs, the name util-linux's mkfs
# runs for `mkfs -t ext2` and Debian's e2fsprogs links; fsck.ext2 is one of
# e2fsck, which fsck runs for an ext2 volume; and e2label is one of tune2fs,
# which acts as e2label when it is run by that name. The ext3 and ext4 names
# are left out, since the kernel mounts neither's volumes.
ln -s mke2fs "$TREE/bin/mkfs.ext2"
ln -s e2fsck "$TREE/sbin/fsck.ext2"
ln -s tune2fs "$TREE/sbin/e2label"

# The mount table where glibc's getmntent and GNU df look for it, as a
# Debian system links it.
ln -s ../proc/self/mounts "$TREE/etc/mtab"

# /run, where D-Bus's programs look for the system bus's socket,
# /run/dbus/system_bus_socket: a link to /var/run, which init empties at
# boot as Linux keeps /run in memory. A port that installed into /run would
# turn the link into a link inside that directory.
if [ -e "$TREE/run" ] || [ -L "$TREE/run" ]; then
    echo "gnu: install: a port installed /run, where the link to /var/run goes" >&2
    exit 1
fi
ln -s var/run "$TREE/run"

# BusyBox's applets are not files: ash runs one for its name before it
# searches the path, and bash and every program that searches the path
# itself, as Git does for its editor, find only files. So each applet that
# no file in /usr/bin, /bin, /usr/sbin or /sbin provides, as ps, mount
# and which, is linked to /bin/busybox where BusyBox's own install puts it.
# The build host runs the BusyBox the manifest stages, which is static, for
# the list, from the architecture's build: the stager writes the tree's files
# without their modes, which mkext2.sh gives the volume afterwards.
"$REPO/build$ARCH_SUFFIX/vendor/busybox/busybox" --list-full > "$WORK/applets"
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
"$TREE$GNU_LOADER" --library-path "$TREE/lib:$TREE/usr/lib" \
    "$TREE/usr/bin/tic" -o "$TREE/usr/share/terminfo" "$DIR/rach2.ti"
# term gives its sessions TERM=xterm-256color, which ncurses's whole database
# has; a port that installed less would leave every program in term without
# its entry.
if [ ! -f "$TREE/usr/share/terminfo/x/xterm-256color" ]; then
    echo "gnu: install: refused: ncurses installed no xterm-256color, the terminal term names" >&2
    exit 1
fi

# Debian's cursor-theme alternative is normally made by a maintainer script.
mkdir -p "$TREE/usr/share/icons/default"
printf '[Icon Theme]\nInherits=Adwaita\n' > "$TREE/usr/share/icons/default/index.theme"
for pair in dnd-copy:copy dnd-none:no-drop; do
    ln -s "${pair#*:}" "$TREE/usr/share/icons/Adwaita/cursors/${pair%%:*}"
done

# Build the caches normally made by Debian's package maintainer scripts.
# Target programs run through the same loader as tic; no host libraries enter.
libpath="$TREE/lib:$TREE/usr/lib:$TREE/usr/lib/$GNU_MULTIARCH"
glib="$TREE/usr/lib/$GNU_MULTIARCH/glib-2.0/glib-compile-schemas"
"$TREE$GNU_LOADER" --library-path "$libpath" "$glib" \
    "$TREE/usr/share/glib-2.0/schemas"
pixbuf="$TREE/usr/lib/$GNU_MULTIARCH/gdk-pixbuf-2.0"
GDK_PIXBUF_MODULEDIR="$pixbuf/2.10.0/loaders" \
    "$TREE$GNU_LOADER" --library-path "$libpath" "$pixbuf/gdk-pixbuf-query-loaders" \
    > "$WORK/loaders.cache"
awk -v root="$TREE" '{
    while ((at = index($0, root)) != 0)
        $0 = substr($0, 1, at - 1) substr($0, at + length(root))
    print
}' "$WORK/loaders.cache" > "$pixbuf/2.10.0/loaders.cache"
"$TREE$GNU_LOADER" --library-path "$libpath" "$TREE/usr/bin/update-mime-database" \
    "$TREE/usr/share/mime"

# Debian's D-Bus activation helper has no privileged activation on Rach2.
if [ -f "$TREE/usr/lib/dbus-1.0/dbus-daemon-launch-helper" ]; then
    chmod 0755 "$TREE/usr/lib/dbus-1.0/dbus-daemon-launch-helper"
fi

# The ports' manual pages, rendered to the plain text the system's man
# reads, which it looks in after the project's own pages.
sh "$DIR/render_man.sh" "$TREE/usr/share/man"

# Screen's private session sockets need no set-id privilege.
chmod 0755 "$TREE/usr/bin/screen"

# ssh-keysign is set-id root so that host-based authentication can read the
# host keys. The system uses no host-based authentication, and a set-id file
# the manifest does not declare fails the set-id guard, so it has no bit.
# It and sudo's directories have the modes their ports install, which the
# store, readable by every account, does not keep.
chmod 0711 "$TREE/usr/libexec/ssh-keysign" "$TREE/var/lib/sudo"
chmod 0700 "$TREE/var/lib/sudo/lectured"

# The shadow file the manifest stages, krffs/shadow, has the lines of the
# accounts that log in, root's and test's, which every image shares. shadow's
# pwck reports an account of /etc/passwd that has no line, so the GNU root's
# system accounts, sshd, messagebus and nobody, get one with a locked
# password, as Debian's have.
locked=$(awk -F: 'NR == FNR { have[$1] = 1; next } !($1 in have) { print $1 ":!:::::::" }' \
    "$TREE/etc/shadow" "$TREE/etc/passwd")
[ -z "$locked" ] || printf '%s\n' "$locked" >>"$TREE/etc/shadow"

# ldconfig is linked statically, as glibc always links it, so the build host
# runs the root's own without its loader: -r reads the tree's ld.so.conf and
# writes the tree's cache, and -X leaves the links in the library
# directories as the packages made them.
"$TREE/sbin/ldconfig" -X -r "$TREE"
echo "gnu: install: ld.so.cache: $("$TREE/sbin/ldconfig" -r "$TREE" -p | sed -n 1p)"
