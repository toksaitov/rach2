#!/bin/bash

#
# gnu_rows.sh: the rows of `tests/run gnu` that run on the machine.
#
# Usage: gnu_rows.sh NAME
#
# The gnu suite puts this script on the machine it boots, the GNU system's
# ext2 root, and runs it there as root under /bin/bash, once for each row,
# each run bounded on the host and in a directory of the row's own. A row
# has a port, or a tool built beside the ports, do its main job as a person
# would ask for it, and compares what it printed or wrote with what it must
# be. A program a row runs as its job, and whose output a crash could follow,
# must also end with status 0: the programs built by make and by GCC, GDB,
# the SpiderMonkey shell and the browser. It prints `GNUROW OK NAME` when
# every comparison holds, and otherwise `GNUROW FAIL NAME: ` with the first
# that did not. The path has /usr/bin first, so each name finds the GNU
# program, and bash runs no BusyBox applet for a name.
#

set -u
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export HOME=/root LC_ALL=C

ROW=${1:-}
WORK=/tmp/gnu/$ROW
FAILED=
OUT=
ABC=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad

# fail WHAT: record WHAT unless an earlier comparison already failed.
fail() {
    [ -n "$FAILED" ] || FAILED=$1
}

# same WHAT GOT WANT: require GOT to be WANT.
same() {
    [ "$2" = "$3" ] || fail "$1: got [$2], want [$3]"
}

# has WHAT GOT WANT: require GOT to contain WANT.
has() {
    case "$2" in
        *"$3"*) ;;
        *) fail "$1: [$(printf '%s' "$2" | tail -n 3 | tr '\n' ' ')] has no [$3]" ;;
    esac
}

# runs WHAT COMMAND...: require COMMAND to exit with status 0.
runs() {
    local what=$1
    shift
    "$@" > "$WORK/out" 2>&1 || fail "$what: status $?: $(tail -n 3 "$WORK/out" | tr '\n' ' ')"
}

# output WHAT COMMAND...: keep what COMMAND prints on both streams in OUT,
# and require it to exit with status 0, so output a crash follows fails.
output() {
    local what=$1
    shift
    OUT=$("$@" 2>&1) || fail "$what: status $?: $(printf '%s' "$OUT" | tail -n 3 | tr '\n' ' ')"
}

# The names the path finds: GNU make and GCC as `make` and `cc`, the GNU
# tools before anything of /bin, /bin/bash and /bin/sh as bash, which finds
# GNU's grep by the path, each program under its name in /bin and in /usr/bin,
# as Debian's merged /usr has it, and Debian's account and group of nobody.
row_names() {
    same "make" "$(type -P make)" "/usr/bin/make"
    same "cc" "$(type -P cc)" "/usr/bin/cc"
    same "grep" "$(type -P grep)" "/usr/bin/grep"
    same "/bin/bash" "$(readlink /bin/bash)" "../usr/bin/bash"
    has "/bin/sh" "$(/bin/sh --version 2>&1 | head -n 1)" "GNU bash"
    has "grep in /bin/sh" "$(/bin/sh -c 'grep --version' 2>&1 | head -n 1)" "GNU grep"
    same "/bin/true" "$(readlink /bin/true)" "../usr/bin/true"
    same "/usr/sbin/ldconfig" "$(readlink /usr/sbin/ldconfig)" "../../sbin/ldconfig"
    same "nobody" "$(getent passwd nobody | cut -d: -f3,4) $(getent group nogroup | cut -d: -f3)" \
        "65534:65534 65534"
}

# glibc, its databases of service, protocol and network names, which
# Debian's netbase gives, and the zone the clock is kept in.
row_glibc() {
    same "getconf" "$(getconf GNU_LIBC_VERSION)" "glibc 2.44"
    has "the loader" "$(/lib/ld-linux.so.2 --version 2>&1 | head -n 1)" "2.44"
    has "the cache" "$(ldconfig -p | head -n 1)" "libs found in cache"
    same "a locale of the archive" "$(LC_ALL=en_US.UTF-8 locale charmap)" "UTF-8"
    same "a service by its name" "$(getent services https | tr -s ' ')" "https 443/tcp"
    same "a protocol by its name" "$(getent protocols tcp | tr -s ' ')" "tcp 6 TCP"
    same "a port by its name in Perl" "$(perl -e 'print scalar getservbyname("ssh", "tcp")')" "22"
    same "the zone" "$(readlink /etc/localtime) $(cat /etc/timezone)" \
        "../usr/share/zoneinfo/Etc/UTC Etc/UTC"
}

row_zlib() {
    same "a round trip" "$(printf 'zlib row' | openssl zlib | openssl zlib -d)" "zlib row"
    has "curl's zlib" "$(curl -V | head -n 1)" "zlib/1.3.2"
}

row_openssl() {
    has "the version" "$(openssl version)" "OpenSSL 4.0.2"
    has "SHA-256" "$(printf abc | openssl dgst -sha256)" "$ABC"
    same "the bundle" "$(grep -c 'BEGIN CERTIFICATE' /etc/ssl/cert.pem)" "121"
    awk '/BEGIN CERT/ { p = 1 } p { print } /END CERT/ { exit }' \
        /etc/ssl/certs/ca-certificates.crt > "$WORK/root.pem"
    same "a root of the bundle in the default store" "$(openssl verify "$WORK/root.pem" 2>&1)" \
        "$WORK/root.pem: OK"
}

# The clients against the machine's own server, with a key made here; a
# session on a terminal of the server's is in the login records `who` reads.
row_openssh() {
    local opts=(-i "$WORK/id" -o BatchMode=yes -o StrictHostKeyChecking=no -o LogLevel=ERROR
                -o "UserKnownHostsFile=$WORK/known" -o ConnectTimeout=120)

    has "the version" "$(ssh -V 2>&1)" "OpenSSH_10.5p1"
    runs "ssh-keygen" ssh-keygen -q -t ed25519 -N '' -f "$WORK/id"
    cat "$WORK/id.pub" >> /root/.ssh/authorized_keys
    same "ssh" "$(ssh "${opts[@]}" root@127.0.0.1 echo ssh row 2>&1)" "ssh row"
    # shellcheck disable=SC2016 # the dollar is the remote shell's
    same "a command's path" "$(ssh "${opts[@]}" root@127.0.0.1 'echo $PATH' 2>&1)" \
        "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin"
    has "who in a session with a terminal" "$(ssh -tt "${opts[@]}" root@127.0.0.1 who 2>&1)" "root     pts/"
    printf 'scp row\n' > "$WORK/scp.in"
    runs "scp" scp "${opts[@]}" "$WORK/scp.in" "root@127.0.0.1:$WORK/scp.out"
    same "the file scp wrote" "$(cat "$WORK/scp.out" 2>&1)" "scp row"
    printf 'get %s %s\n' "$WORK/scp.out" "$WORK/sftp.out" > "$WORK/sftp.batch"
    runs "sftp" sftp "${opts[@]}" -b "$WORK/sftp.batch" root@127.0.0.1
    same "the file sftp fetched" "$(cat "$WORK/sftp.out" 2>&1)" "scp row"
}

row_ncurses() {
    has "tput" "$(tput -T vt100 cup 5 10 | od -An -c | tr -s ' ')" "033 [ 6 ; 1 1 H"
    has "infocmp" "$(infocmp -1 vt100 | sed -n 2p)" "vt100|vt100-am"
}

row_less() {
    printf 'less row\n' > "$WORK/less.txt"
    same "a file through less" "$(less "$WORK/less.txt")" "less row"
    has "the version" "$(less --version | head -n 1)" "less 710"
}

row_vim() {
    printf 'hello world\n' > "$WORK/vim.txt"
    runs "an edit in ex mode" vim -Nu NONE -i NONE -es -c '%s/world/Rach2/' -c wq "$WORK/vim.txt"
    same "the edited file" "$(cat "$WORK/vim.txt")" "hello Rach2"
}

row_perl() {
    # shellcheck disable=SC2016 # the dollars are Perl's
    same "a hash and a sort" "$(perl -e 'my %h = map { $_ => $_ * $_ } 1 .. 5;
        print join(",", map { $h{$_} } sort { $a <=> $b } keys %h)')" "1,4,9,16,25"
    same "Digest::SHA" "$(perl -MDigest::SHA=sha256_hex -e 'print sha256_hex("abc")')" "$ABC"
    # shellcheck disable=SC2016 # the dollars are Perl's
    same "IO::Compress::Gzip" "$(perl -MIO::Compress::Gzip=gzip -MIO::Uncompress::Gunzip=gunzip \
        -e 'my $in = "perl row"; my ($z, $out); gzip(\$in => \$z) or die;
            gunzip(\$z => \$out) or die; print $out')" "perl row"
    # shellcheck disable=SC2016 # the dollars are Perl's
    same "threads" "$(perl -Mthreads -Mthreads::shared -e 'my $n :shared = 0;
        $_->join for map { threads->create(sub { lock $n; $n += 10 }) } 1 .. 4; print $n')" "40"
}

# A file URL, and HTTPS from openssl's test server on this machine, whose
# certificate curl is given; curl tries again while the server starts.
row_curl() {
    local server

    same "a file URL" "$(curl -sS file:///etc/hostname)" "rach2"
    runs "a certificate" openssl req -x509 -newkey ed25519 -keyout "$WORK/key.pem" \
        -out "$WORK/cert.pem" -days 1 -nodes -subj /CN=localhost -addext subjectAltName=DNS:localhost
    openssl s_server -quiet -accept 127.0.0.1:4433 -key "$WORK/key.pem" -cert "$WORK/cert.pem" \
        -www > "$WORK/server.log" 2>&1 &
    server=$!
    has "HTTPS" "$(curl -sS --retry 120 --retry-connrefused --retry-delay 1 \
        --cacert "$WORK/cert.pem" https://localhost:4433/ 2>&1)" "TLSv1.3"
    kill "$server"
    wait "$server" 2> /dev/null
}

row_git() {
    local git=(git -c user.name=rach2 -c user.email=root@rach2)

    runs "git init" "${git[@]}" init -q "$WORK/repo"
    printf 'git row\n' > "$WORK/repo/file"
    runs "git add" "${git[@]}" -C "$WORK/repo" add file
    runs "git commit" "${git[@]}" -C "$WORK/repo" commit -qm first
    same "git log" "$(git -C "$WORK/repo" log --format=%s)" "first"
    runs "git clone" "${git[@]}" clone -q "$WORK/repo" "$WORK/clone"
    same "the clone's file" "$(cat "$WORK/clone/file" 2>&1)" "git row"
    same "git grep -P" "$(git -C "$WORK/repo" grep -oP 'g\w+(?= row)')" "file:git"
}

# coreutils: its version, a pipeline, a digest, dates in the zones of the
# database, and the boot time `who -b` takes from init's record in utmp.
row_coreutils() {
    has "the version" "$(cat --version | head -n 1)" "(GNU coreutils) 9.12"
    same "seq, sort and head" "$(seq 1 10 | sort -rn | head -n 1)" "10"
    same "sha256sum" "$(printf abc | sha256sum)" "$ABC  -"
    same "date" "$(date -u -d @0 +%Y-%m-%d)" "1970-01-01"
    same "a zone of the database" "$(TZ=Europe/Berlin date -d @0 +%H:%M:%Z)" "01:00:CET"
    same "a zone's old name" "$(TZ=US/Eastern date -d @0 +%H:%Z)" "19:EST"
    same "who -b" "$(LC_ALL=C who -b | sed 's/^ *system boot *//')" \
        "$(LC_ALL=C date -d @"$(awk '/^btime/ {print $2}' /proc/stat)" '+%b %e %H:%M')"
}

# bash with the answers its configure could not find in a cross build:
# the status of a pipeline's last command under lastpipe, printf's %a, the
# real-time signals, and its directory as a user who may not search it.
row_bash() {
    # shellcheck disable=SC2016 # the dollars are the inner bash's
    same "an associative array" "$(bash -c 'declare -A m=([k]=v); echo "${m[k]}"')" "v"
    # shellcheck disable=SC2016 # the dollars are the inner bash's
    same "lastpipe's status" "$(bash -c 'shopt -s lastpipe; true | false; echo $?')" "1"
    same "printf %a" "$(bash -c 'printf "%a" 1')" "0x1p+0"
    same "a real-time signal's name" "$(bash -c 'kill -l 40')" "RTMIN+6"
    mkdir -m 700 "$WORK/closed"
    same "pwd where the user may not search" "$(cd "$WORK/closed" && su test -c pwd 2>&1)" "$WORK/closed"
    # shellcheck disable=SC2016 # the dollars are the inner bash's
    same "the version" "$(/bin/bash -c 'echo "${BASH_VERSINFO[0]}.${BASH_VERSINFO[1]}"')" "5.3"
    # shellcheck disable=SC2016 # the dollars are the inner bash's
    same "a here-document longer than a pipe holds" "$(timeout 20 bash -c 'doc=$(head -c 5000 /dev/zero | tr "\0" a)
cat << EOF | wc -c
$doc
EOF')" "5001"
}

row_grep() {
    same "an extended pattern" "$(printf 'a1\nb22\nc333\n' | grep -E -c '[0-9]{2}')" "2"
    same "a Perl pattern" "$(printf 'a1\nb22\nc333\n' | grep -oP '(?<=b)\d+')" "22"
    runs "a match from a pipe into /dev/null, as configure asks" \
        bash -c 'printf "a\nb\n" | grep -E a > /dev/null'
    has "the version" "$(grep --version | head -n 1)" "3.12"
}

row_sed() {
    same "a substitution" "$(echo hello | sed -E 's/(l+)/[\1]/')" "he[ll]o"
    has "the version" "$(sed --version | head -n 1)" "4.10"
}

row_gawk() {
    same "strftime" "$(gawk 'BEGIN { print strftime("%Y", 0, 1), 6 * 7 }')" "1970 42"
    has "the version" "$(gawk --version | head -n 1)" "GNU Awk 5.4.1"
}

row_findutils() {
    same "find" "$(find /usr/share/terminfo -name vt100 -printf '%f\n' | head -n 1)" "vt100"
    same "xargs" "$(printf 'a b c' | xargs -n 1 | wc -l)" "3"
    # shellcheck disable=SC2185 # --version names no path to walk
    has "the version" "$(find --version | head -n 1)" "(GNU findutils) 4.11.0"
}

row_diffutils() {
    printf 'one\ntwo\nthree\n' > "$WORK/a"
    printf 'one\n2\nthree\n' > "$WORK/b"
    diff -u "$WORK/a" "$WORK/b" > "$WORK/ab.diff"
    same "diff's status for two files that differ" "$?" "1"
    has "the difference" "$(cat "$WORK/ab.diff")" "+2"
    runs "cmp" cmp "$WORK/a" "$WORK/a"
    has "the version" "$(diff --version | head -n 1)" "(GNU diffutils) 3.12"
}

row_patch() {
    printf 'one\ntwo\nthree\n' > "$WORK/a"
    printf -- '--- a\n+++ b\n@@ -1,3 +1,3 @@\n one\n-two\n+2\n three\n' > "$WORK/ab.diff"
    runs "patch" patch -s -o "$WORK/c" "$WORK/a" "$WORK/ab.diff"
    same "the patched file" "$(tr '\n' ' ' < "$WORK/c")" "one 2 three "
    has "the version" "$(patch --version | head -n 1)" "GNU patch 2.8"
}

row_tar() {
    mkdir -p "$WORK/dir" "$WORK/x"
    printf 'tar row\n' > "$WORK/dir/file"
    runs "tar -c" tar -czf "$WORK/t.tgz" -C "$WORK" dir
    same "tar -t" "$(tar -tzf "$WORK/t.tgz" | sort | tr '\n' ' ')" "dir/ dir/file "
    runs "tar -x" tar -xzf "$WORK/t.tgz" -C "$WORK/x"
    same "the extracted file" "$(cat "$WORK/x/dir/file" 2>&1)" "tar row"
    has "the version" "$(tar --version | head -n 1)" "(GNU tar) 1.35"
}

row_gzip() {
    seq 1 1000 > "$WORK/in"
    same "a round trip" "$(gzip -c "$WORK/in" | gzip -dc | cksum)" "$(cksum < "$WORK/in")"
    has "the version" "$(gzip --version | head -n 1)" "1.15"
}

row_xz() {
    seq 1 1000 > "$WORK/in"
    same "a round trip" "$(xz -c "$WORK/in" | xz -dc | cksum)" "$(cksum < "$WORK/in")"
    has "the version" "$(xz --version | head -n 1)" "5.8.4"
}

# The interfaces, addresses, routes and neighbours, through the netlink
# route socket: a link asked for by its name, with its counters, a route
# looked up, the rules and the queueing disciplines. Then the
# sockets through the socket diagnostics, which ss asks first and complains
# of when it cannot, a listener among them, and the counts of ss -s.
row_iproute2() {
    has "ip addr" "$(ip -4 addr show 2>&1)" "inet 10.0.2.15/24"
    has "ip route" "$(ip route show 2>&1)" "default via 10.0.2.2"
    has "ip link" "$(ip -o link show 2>&1)" "eth0: <"
    has "lo" "$(ip -o addr show lo 2>&1)" "inet 127.0.0.1/8 scope host lo"
    has "ip link of one interface by its name" "$(ip link show eth0 2>&1)" "link/ether"
    has "ip -s link's counters" "$(ip -s link show eth0 2>&1)" "RX:"
    has "ip neigh" "$(ip neigh show 2>&1)" "10.0.2.2 dev eth0 lladdr"
    has "ip route get" "$(ip route get 10.0.2.2 2>&1)" "10.0.2.2 dev eth0 src 10.0.2.15"
    has "ip rule" "$(ip rule 2>&1)" "32766:	from all lookup main"
    has "tc qdisc" "$(tc qdisc 2>&1)" "qdisc noqueue 0: dev eth0 root"
    has "ip netconf" "$(ip netconf 2>&1)" "inet all forwarding off rp_filter off"
    perl -MIO::Socket::INET -e 'my $s = IO::Socket::INET->new(LocalPort => 5999, Listen => 1) or die; sleep 90' &
    local pid=$! i=0
    until grep -q ':176F ' /proc/net/tcp || [ $i -ge 600 ]; do
        sleep 0.1
        i=$((i + 1))
    done
    has "the listener in /proc/net/tcp" "$(cat /proc/net/tcp)" ":176F "
    has "ss -tln" "$(ss -tln 2>&1)" "0.0.0.0:5999"
    same "what ss -a says on stderr" "$(ss -a 2>&1 >/dev/null)" ""
    has "ss -s" "$(ss -s 2>&1)" "TCP:   "
    same "what ss -s says on stderr" "$(ss -s 2>&1 >/dev/null)" ""
    kill "$pid"
    has "lspci, from /sys" "$(lspci 2>&1)" "00:00.0 Class 0600: 8086:"
}

# loopctl attaches a file named relative to the current directory, which it
# resolves, since the kernel takes an absolute path, and detaches it; the
# file is an ext2 volume that util-linux's mkfs made through mkfs.ext2.
row_loopctl() {
    local dev
    truncate -s 1M "$WORK/loop.img"
    runs "mkfs -t ext2" mkfs -t ext2 -q -F "$WORK/loop.img"
    has "the volume mkfs made" "$(file "$WORK/loop.img")" "ext2 filesystem"
    runs "loopctl attach of a relative name" loopctl attach loop.img
    dev=$(loopctl | awk -v f="$WORK/loop.img" '$NF == f { sub(/^loop/, "", $1); print $1 }')
    has "the attachment" "$(loopctl)" " $WORK/loop.img"
    runs "loopctl detach" loopctl detach "${dev:-none}"
}

# util-linux's losetup attaches an image to the first free loop device and lists
# it, mkfs makes a file system on the device, blockdev reads what the device
# says of itself, and mount -o loop mounts the image by itself, learning its
# type from the device, and gives the device back at the unmount.
row_losetup() {
    local dev
    truncate -s 4M "$WORK/lo.img"
    dev=$(losetup -f --show "$WORK/lo.img" 2>&1)
    has "losetup -f --show" "$dev" "/dev/loop"
    has "losetup -a" "$(losetup -a 2>&1)" "$WORK/lo.img"
    runs "mkfs.ext2 on the device" mkfs.ext2 -q "$dev"
    same "blockdev" "$(blockdev --getro --getra --getbsz --getss "$dev" 2>&1 | tr '\n' ' ')" \
        "0 256 4096 512 "
    runs "losetup -d" losetup -d "$dev"
    mkdir -p "$WORK/mnt"
    runs "mount -o loop" mount -o loop "$WORK/lo.img" "$WORK/mnt"
    same "the mount's type" "$(findmnt -no FSTYPE "$WORK/mnt")" "ext2"
    runs "umount" umount "$WORK/mnt"
    same "the device given back" "$(losetup -a 2>&1)" ""
}

# e2fsprogs: tune2fs labels a volume in a file and e2label reads the label,
# resize2fs grows the volume and dumpe2fs lists its new size, and fsck checks
# it through fsck.ext2.
row_e2fsprogs() {
    truncate -s 8M "$WORK/v.ext2"
    runs "mke2fs" mke2fs -q -F "$WORK/v.ext2"
    runs "tune2fs -L" tune2fs -L rowvol "$WORK/v.ext2"
    same "e2label" "$(e2label "$WORK/v.ext2" 2>&1)" "rowvol"
    truncate -s 16M "$WORK/v.ext2"
    runs "resize2fs" resize2fs "$WORK/v.ext2"
    same "dumpe2fs -h" "$(dumpe2fs -h "$WORK/v.ext2" 2>/dev/null |
        awk -F: '/^Block count/ { gsub(/ /, "", $2); print $2 }')" "4096"
    runs "fsck through fsck.ext2" fsck -t ext2 -n "$WORK/v.ext2"
}

# file names a program, a text and a gzip file from the magic database, and
# looks into the gzip file with -z.
row_file() {
    printf 'plain text\n' > "$WORK/t.txt"
    gzip -c "$WORK/t.txt" > "$WORK/t.gz"
    output "file" file -L /bin/bash "$WORK/t.txt" "$WORK/t.gz"
    has "the program" "$OUT" "ELF 32-bit LSB"
    has "the text" "$OUT" "t.txt: ASCII text"
    has "the gzip file" "$OUT" "gzip compressed data"
    output "file -z" file -bz "$WORK/t.gz"
    has "the text inside" "$OUT" "ASCII text"
    output "file --mime-type" file -bL --mime-type /bin/bash
    has "the program's type" "$OUT" "application/x-"
}

# pkg-config answers for the root's libraries with the flags a build needs.
row_pkgconf() {
    same "a library's flags" "$(pkg-config --libs zlib)" "-lz"
    has "the version" "$(pkg-config --version)" "2.5.1"
}

# procps-ng's programs read the processes and the memory from /proc: ps and
# pgrep take the options BusyBox's lack, and the rest print what they must.
row_procps() {
    same "ps -p" "$(ps -p 1 -o pid=,comm= | tr -s ' ')" " 1 init"
    has "ps -u" "$(ps -u root -o comm=)" "sshd"
    same "pgrep -u" "$(pgrep -u root -x sshd | wc -l)" "1"
    has "pgrep -c" "$(pgrep -c -x sshd)" "1"
    has "uptime -p" "$(uptime -p)" "up "
    has "free" "$(free -k)" "Mem:"
    same "vmstat" "$(vmstat 1 2 | wc -l)" "4"
    has "w" "$(w)" "load average:"
    same "kill -l" "$(kill -l 15)" "TERM"
    same "sysctl" "$(sysctl -n kernel.hostname)" "$(cat /proc/sys/kernel/hostname)"
    has "pmap" "$(pmap $$ | tail -n 1)" "total"
}

# util-linux's programs: the disks and mounts from /sys and /proc and the root
# /etc/fstab names, a lock taken and waited for, the limits and the I/O class of
# a process, the login history and the record of the boot that init writes, the
# formatting and terminal tools jq's tests and scripts use, and the namespaces
# unshare makes, nsenter enters and lsns lists.
row_utillinux() {
    local pid
    has "lsblk" "$(lsblk -dno NAME,TYPE)" "disk"
    same "findmnt" "$(findmnt -no FSTYPE /)" "ext2"
    has "findmnt --verify of /etc/fstab" "$(findmnt --verify 2>&1)" "Success, no errors or warnings detected"
    flock "$WORK/lock" sleep 5 &
    pid=$!
    sleep 1
    flock -w 1 "$WORK/lock" true
    same "flock -w on a held lock" "$?" "1"
    wait "$pid"
    runs "flock on a free lock" flock -w 1 "$WORK/lock" true
    has "prlimit" "$(prlimit --pid $$ --nofile)" "NOFILE"
    same "ionice" "$(ionice -c 2 -n 5 ionice)" "best-effort: prio 5"
    has "last" "$(last -n 1 root)" "root"
    has "the boot's record in utmp" "$(utmpdump /var/run/utmp 2>/dev/null)" "[reboot"
    has "last -x" "$(last -x reboot)" "system boot"
    same "column -t" "$(printf 'a b\nccc d\n' | column -t)" "$(printf 'a    b\nccc  d')"
    script -q -e -c 'exit 3' /dev/null
    same "script -e's status" "$?" "3"
    same "uuidgen" "$(uuidgen | grep -cE '^[0-9a-f]{8}-([0-9a-f]{4}-){3}[0-9a-f]{12}$')" "1"
    has "dmesg" "$(dmesg | head -n 1)" "]"
    same "newgrp's mode" "$(stat -c '%a %U' /usr/bin/newgrp)" "4755 root"
    same "unshare -r as the test account" "$(su test -c 'unshare -r id -u')" "0"
    same "unshare -pf --mount-proc" "$(unshare -pf --mount-proc sh -c 'echo $$; ps -e -o pid= | wc -l')" \
        "$(printf '1\n3')"
    same "unshare -u" "$(unshare -u sh -c 'hostname inner; hostname'; hostname)" \
        "$(printf 'inner\n%s' "$(hostname)")"
    same "nsenter -t 1 -u" "$(unshare -u sh -c 'hostname inner; nsenter -t 1 -u hostname')" "$(hostname)"
    has "lsns" "$(lsns -n -o TYPE)" "user"
}

# zstd compresses and decompresses, in its own format and in gzip's, and
# tar calls it for --zstd.
row_zstd() {
    seq 1 1000 > "$WORK/in"
    same "a round trip" "$(zstd -c "$WORK/in" | zstd -dc | cksum)" "$(cksum < "$WORK/in")"
    same "gzip's format" "$(zstd -c --format=gzip "$WORK/in" | gzip -dc | cksum)" "$(cksum < "$WORK/in")"
    mkdir "$WORK/dir"
    cp "$WORK/in" "$WORK/dir/in"
    runs "tar --zstd" tar --zstd -cf "$WORK/t.tar.zst" -C "$WORK" dir
    same "tar --zstd -t" "$(tar --zstd -tf "$WORK/t.tar.zst" | sort | tr '\n' ' ')" "dir/ dir/in "
    has "the version" "$(zstd --version)" "v1.5.7"
}

# rsync copies a tree, then only what changed, and deletes what the source
# no longer has; and over SSH to the machine's own server.
row_rsync() {
    local rsh="ssh -i $WORK/id -o BatchMode=yes -o StrictHostKeyChecking=no -o LogLevel=ERROR -o UserKnownHostsFile=$WORK/known"
    mkdir -p "$WORK/src/d"
    seq 1 1000 > "$WORK/src/d/a"
    printf 'b\n' > "$WORK/src/b"
    runs "rsync -a" rsync -a "$WORK/src/" "$WORK/dst/"
    runs "the copy" diff -r "$WORK/src" "$WORK/dst"
    printf 'c\n' > "$WORK/src/c"
    rm "$WORK/src/b"
    output "rsync -a --delete" rsync -ai --delete "$WORK/src/" "$WORK/dst/"
    same "what changed" "$(printf '%s\n' "$OUT" | awk '$2 != "./" { print $2 }' | sort | tr '\n' ' ')" \
        "b c "
    runs "ssh-keygen" ssh-keygen -q -t ed25519 -N '' -f "$WORK/id"
    cat "$WORK/id.pub" >> /root/.ssh/authorized_keys
    runs "rsync over ssh" rsync -a -e "$rsh" "$WORK/src/" "root@127.0.0.1:$WORK/remote/"
    runs "the copy over ssh" diff -r "$WORK/src" "$WORK/remote"
    has "the version" "$(rsync --version | head -n 1)" "version 3.5.1"
}

# nano edits a file on a terminal script gives it: the text typed, then
# ^X, which with -t saves the file without asking.
row_nano() {
    printf 'hello nano\030' | script -q -c "nano -t $WORK/n.txt" /dev/null > /dev/null
    same "the file nano saved" "$(cat "$WORK/n.txt" 2>&1)" "hello nano"
    has "the syntax files /etc/nanorc includes" "$(grep -c '^include' /etc/nanorc)" "1"
    has "the version" "$(nano --version | head -n 1)" "9.2"
}

# makeinfo turns a Texinfo file into an Info file, and info reads a node of
# it and a node of a manual the root has.
row_texinfo() {
    printf '\\input texinfo\n@setfilename t.info\n@settitle T\n@node Top\n@top T\n\nTexinfo row.\n@bye\n' \
        > "$WORK/t.texi"
    runs "makeinfo" makeinfo -o "$WORK/t.info" "$WORK/t.texi"
    has "info" "$(info -o - -f "$WORK/t.info" 2>&1)" "Texinfo row."
    has "a manual of the root" "$(info -o - sed 'Invoking sed' 2>&1 | head -n 5)" "sed"
    has "the version" "$(info --version | head -n 1)" "7.3"
}

# strace traces a program's calls, and a child's with -f.
row_strace() {
    output "strace" strace -f -e trace=write,execve -o "$WORK/log" \
        bash -c 'echo strace row; /usr/bin/true; echo done'
    has "the write" "$(tr -s ' ' < "$WORK/log")" 'write(1, "strace row\n", 11) = 11'
    has "the child's execve" "$(cat "$WORK/log")" 'execve("/usr/bin/true"'
    has "strace -c" "$(strace -c -o /dev/stdout true 2>&1)" "total"
}

# lsof lists a process's open file, the process that has a file open, in a
# scan of every process, epoll descriptors among them, and the server's
# listening socket.
row_lsof() {
    local pid
    : > "$WORK/held"
    sleep 30 3< "$WORK/held" &
    pid=$!
    sleep 1
    has "lsof -p" "$(lsof -p "$pid" 2>&1)" "$WORK/held"
    has "lsof FILE" "$(lsof "$WORK/held" 2>&1)" "sleep"
    has "lsof -i" "$(lsof -nP -i TCP:22 -s TCP:LISTEN 2>&1)" "sshd"
    kill "$pid"
}

# tmux runs a command in a detached session, and the pane shows what it
# printed; a window's login shell starts in the directory the window names,
# which /etc/profile leaves as it is.
row_tmux() {
    runs "tmux new-session" tmux new-session -d -s row -x 80 -y 24 'echo tmux row; sleep 30'
    sleep 1
    has "the pane" "$(tmux capture-pane -p -t row)" "tmux row"
    runs "tmux new-window" tmux new-window -t row -n here -c "$WORK"
    sleep 1
    tmux send-keys -t row:here pwd Enter
    sleep 1
    has "a window's directory" "$(tmux capture-pane -p -t row:here)" "$WORK"
    runs "tmux kill-server" tmux kill-server
}

# cron runs a user's crontab: crontab, set-id to its group, installs the
# test account's into the spool, and the daemon init started runs its job
# at the next minute.
row_cron() {
    local i=0
    same "the daemon" "$(pgrep -x crond | wc -l)" "1"
    rm -f /tmp/cron-row.out
    echo '* * * * * echo cron row > /tmp/cron-row.out' | su test -c 'crontab -'
    same "the crontab" "$(su test -c 'crontab -l')" "* * * * * echo cron row > /tmp/cron-row.out"
    same "its owner" "$(stat -c '%U %G' /var/spool/cron/crontabs/test)" "test crontab"
    until [ -s /tmp/cron-row.out ] || [ $i -ge 70 ]; do
        sleep 1
        i=$((i + 1))
    done
    same "the job's output" "$(cat /tmp/cron-row.out 2>&1)" "cron row"
    su test -c 'crontab -r'
}

# zip makes an archive of a tree, and unzip lists it and extracts it.
row_zip() {
    mkdir -p "$WORK/z/d"
    seq 1 1000 > "$WORK/z/d/a"
    (cd "$WORK" && zip -qr z.zip z) || fail "zip: status $?"
    has "unzip -l" "$(unzip -l "$WORK/z.zip")" "z/d/a"
    runs "unzip" unzip -q "$WORK/z.zip" -d "$WORK/unz"
    runs "the extracted tree" diff -r "$WORK/z" "$WORK/unz/z"
    has "zipinfo" "$(zipinfo -1 "$WORK/z.zip")" "z/d/a"
    has "the version" "$(zip -v | head -n 2)" "Zip 3.0"
}

# htop draws a screen of the processes on a terminal and ends after it, as
# -n asks, with the load averages in its header.
row_htop() {
    has "the version" "$(htop --version)" "htop 3.5.3"
    output "a screen" script -qec "TERM=xterm-256color htop -n 1" /dev/null < /dev/null
    has "the screen" "$OUT" "Load average"
}

# dosfstools makes a FAT volume in a file, checks it, and names it again.
row_dosfstools() {
    runs "a file" dd if=/dev/zero of="$WORK/v.img" bs=1M count=16 status=none
    runs "mkfs.vfat" mkfs.vfat -n ROWVOL "$WORK/v.img"
    output "fsck.vfat" fsck.vfat -n "$WORK/v.img"
    has "the check" "$OUT" "files"
    runs "fatlabel" fatlabel "$WORK/v.img" RENAMED
    same "the label" "$(fatlabel "$WORK/v.img")" "RENAMED"
}

# shadow's tools add an account with a home, change its comment, shell and
# group, and remove it with its home.
row_shadow() {
    runs "groupadd" groupadd -g 1777 rowgroup
    runs "useradd" useradd -m -u 1777 -g rowgroup -s /bin/bash rowuser
    has "the account" "$(getent passwd rowuser)" "rowuser:x:1777:1777::/home/rowuser:/bin/bash"
    runs "usermod" usermod -c "Row User" -aG test rowuser
    has "the comment" "$(getent passwd rowuser)" ":Row User:"
    has "the group" "$(id -nG rowuser)" "test"
    runs "chsh" chsh -s /bin/sh rowuser
    has "the shell" "$(getent passwd rowuser)" ":/bin/sh"
    runs "pwck" pwck -r -q
    runs "userdel" userdel -r rowuser
    runs "groupdel" groupdel rowgroup
    same "the account gone" "$(getent passwd rowuser)" ""
    same "the home gone" "$(ls -d /home/rowuser 2>/dev/null)" ""
}

# Python with the modules of its standard library that link the root's
# libraries, ctypes calling into the C library, sqlite3, a pool of worker
# processes, which share POSIX semaphores, a virtual environment, and byte
# code checked against a hash of its source, which the image's file times
# leave valid.
row_python() {
    same "the version" "$(python3 -c 'import sys; print(sys.version_info[:2])')" "(3, 14)"
    output "the modules" python3 -c 'import zlib, bz2, lzma, ssl, hashlib, ctypes, sqlite3, readline, curses, uuid, json, pyexpat, decimal; print("imported")'
    same "the modules" "$OUT" "imported"
    same "hashlib" "$(python3 -c 'import hashlib; print(hashlib.sha256(b"abc").hexdigest())')" "$ABC"
    same "ctypes" "$(python3 -c 'import ctypes, os; print(ctypes.CDLL(None).getpid() == os.getpid())')" "True"
    same "sqlite3" "$(python3 -c 'import sqlite3; c = sqlite3.connect(":memory:"); c.execute("create table t(x)"); c.executemany("insert into t values (?)", [(1,), (41,)]); print(c.execute("select sum(x) from t").fetchone()[0])')" "42"
    same "a pool of processes" "$(python3 -c 'from multiprocessing import Pool
if __name__ == "__main__":
    with Pool(2) as p:
        print(sum(p.map(abs, [-1, -2, 3])))')" "6"
    has "ssl" "$(python3 -c 'import ssl; print(ssl.OPENSSL_VERSION)')" "OpenSSL 4.0.2"
    runs "venv" python3 -m venv --without-pip "$WORK/venv"
    same "the venv's python" "$("$WORK/venv/bin/python" -c 'import sys; print(sys.prefix)')" "$WORK/venv"
    same "the byte code's flags" "$(python3 -c 'import importlib.util as u, traceback; print(open(u.cache_from_source(traceback.__file__), "rb").read(5)[4])')" "3"
}

# GnuPG makes a key, signs with it and verifies the signature, and encrypts
# and decrypts with a passphrase, with its agent started for the home it
# is given and the passphrase given on the command line.
row_gnupg() {
    local g="--homedir $WORK/g --batch --pinentry-mode loopback"
    mkdir -m 700 "$WORK/g"
    has "the version" "$(gpg --version | head -n 1)" "gpg (GnuPG) 2.5.24"
    same "the system configuration directory" "$(gpgconf --list-dirs sysconfdir)" "/etc/gnupg"
    # shellcheck disable=SC2086 # g is a list of options
    runs "a key" gpg $g --passphrase '' --quick-gen-key 'Row <row@example.org>' ed25519 sign never
    printf 'signed row\n' > "$WORK/msg"
    # shellcheck disable=SC2086
    runs "a signature" gpg $g --passphrase '' -o "$WORK/msg.asc" --clearsign "$WORK/msg"
    # shellcheck disable=SC2086
    has "its check" "$(gpg $g --verify "$WORK/msg.asc" 2>&1)" "Good signature from \"Row <row@example.org>\""
    # shellcheck disable=SC2086
    runs "encryption" gpg $g --passphrase row -o "$WORK/msg.gpg" -c "$WORK/msg"
    # shellcheck disable=SC2086
    same "decryption" "$(gpg $g --passphrase row -d "$WORK/msg.gpg" 2>/dev/null)" "signed row"
    gpgconf --homedir "$WORK/g" --kill all
}

# An autoconf package built on the machine as a user builds one: GNU sed's
# configure in a login shell, for a prefix of the user's own, which takes the
# build's name from the root's site file that /etc/profile names, since
# config.guess knows no system called Rach2, then make, and the sed it made.
row_autotools() {
    runs "the tarball" tar -xJf /tmp/gnu/sed.tar.xz -C "$WORK"
    cd "$WORK"/sed-* || {
        fail "no sed directory"
        return
    }
    output "configure" bash -lc "./configure --prefix=$WORK/inst"
    has "the site file, for a prefix of the user's own" "$OUT" "loading site script"
    runs "make" make -j2
    output "the sed made" ./sed/sed --version
    has "its version" "$OUT" "(GNU sed)"
    same "a substitution" "$(echo hello | ./sed/sed 's/l/L/g')" "heLLo"
}

# GNU make over a project of two objects and a pattern rule, run twice.
row_make() {
    mkdir -p "$WORK/p"
    printf 'int twenty(void) { return 20; }\n' > "$WORK/p/a.c"
    printf '#include <stdio.h>\nint twenty(void);\nint main(void) { printf("%%d\\n", twenty() + 22); return 0; }\n' \
        > "$WORK/p/main.c"
    # shellcheck disable=SC2016 # the dollars are make's
    printf 'prog: main.o a.o\n\t$(CC) -o $@ main.o a.o\n%%.o: %%.c\n\t$(CC) -O2 -c -o $@ $<\n' \
        > "$WORK/p/Makefile"
    runs "make" make -C "$WORK/p" -j2
    output "the program" "$WORK/p/prog"
    same "the program" "$OUT" "42"
    has "a second make" "$(make -C "$WORK/p" 2>&1)" "is up to date"
}

# GCC and G++ compile and link a C and a C++ program, and both run; `cc`
# is GCC.
row_gcc() {
    printf '#include <stdio.h>\nint main(void) { printf("hello from gcc\\n"); return 0; }\n' \
        > "$WORK/hello.c"
    cat > "$WORK/hello.cc" << 'EOF'
#include <iostream>
#include <vector>

int main()
{
    std::vector<int> v{1, 2, 3};
    int s = 0;
    for (int x : v)
        s += x;
    std::cout << "hello from g++ " << s << std::endl;
    return 0;
}
EOF
    runs "gcc" gcc -O2 -o "$WORK/hello" "$WORK/hello.c"
    output "the C program" "$WORK/hello"
    same "the C program" "$OUT" "hello from gcc"
    runs "cc" cc -o "$WORK/hello-cc" "$WORK/hello.c"
    output "the program cc linked" "$WORK/hello-cc"
    same "the program cc linked" "$OUT" "hello from gcc"
    has "cc's version" "$(cc --version | head -n 1)" "16.2.0"
    runs "g++" g++ -O2 -o "$WORK/hello++" "$WORK/hello.cc"
    output "the C++ program" "$WORK/hello++"
    same "the C++ program" "$OUT" "hello from g++ 6"
    has "the C++ program's libraries" "$(ldd "$WORK/hello++")" "libstdc++.so.6"
}

# GDB runs a program, stops at a breakpoint, steps, prints a variable and
# a backtrace, and lets it run to its end; it was built with its text user
# interface.
row_gdb() {
    cat > "$WORK/g.c" << 'EOF'
int f(int x)
{
    int y = x * 2;
    return y + 1;
}

int main(void)
{
    return f(20) == 41 ? 0 : 1;
}
EOF
    runs "gcc -g" gcc -g -O0 -o g g.c
    output "gdb" gdb -nx -batch -ex 'break f' -ex run -ex next -ex 'print y' -ex bt -ex continue ./g
    has "the breakpoint" "$OUT" "Breakpoint 1, f (x=20)"
    has "the step" "$OUT" "return y + 1;"
    has "the variable" "$OUT" "\$1 = 40"
    has "the backtrace" "$OUT" "in main () at g.c:"
    has "the end" "$OUT" "exited normally"
    output "gdb's catch syscall" gdb -nx -batch -ex 'catch syscall exit_group' -ex run -ex continue ./g
    has "a catchpoint of a call by its name" "$OUT" "Catchpoint 1 (call to syscall exit_group)"
    has "gdb's text user interface" "$(gdb --configuration)" "--enable-tui"
}

# The script the shell's build ran: a loop the JIT compiles, and the shell's
# own report that it ran in JIT code and in Ion.
row_js() {
    cat > "$WORK/jit.js" << 'EOF'
function sum(n) {
    let s = 0;
    for (let i = 0; i < n; i++)
        s += i;
    return [s, inJit(), inIon()];
}
let r;
for (let k = 0; k < 5000; k++)
    r = sum(1000);
print("sum(1000) = " + r[0]);
print("inJit() = " + r[1] + ", inIon() = " + r[2]);
EOF
    output "js" js "$WORK/jit.js"
    has "the sum" "$OUT" "sum(1000) = 499500"
    has "the JIT" "$OUT" "inJit() = true, inIon() = true"
}

# D-Bus's system bus, which init starts as its own user: it answers a call
# from root and names that user as its owner, and the machine has the
# identity the bus reads, 32 hex digits. A login shell has a session bus of
# its user's, which /etc/profile starts, and the bus answers there.
row_dbus() {
    output "dbus-send" dbus-send --system --print-reply --dest=org.freedesktop.DBus \
        /org/freedesktop/DBus org.freedesktop.DBus.GetConnectionUnixUser string:org.freedesktop.DBus
    has "the bus's user" "$OUT" "uint32 $(id -u messagebus)"
    same "the machine's identity" "$(grep -cxE '[0-9a-f]{32}' /etc/machine-id)" "1"
    output "the session bus" bash -lc 'dbus-send --session --print-reply \
        --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.GetId'
    has "the session bus's answer" "$OUT" 'string "'
}

# The browser's libraries are found through the loader's cache, and the
# browser, run by the test account from its login shell with its sandbox,
# prints the DOM of a local page whose script writes into it, with the
# system bus found and the session bus of the login shell. The browser
# refuses to start when it has no sandbox it can use, so the DOM shows that
# its namespaces, its chroot and its seccomp filters took.
row_chromium() {
    cat > "$WORK/page.html" << 'EOF'
<!DOCTYPE html>
<html><body><p id="out">not run</p>
<script>document.getElementById("out").textContent = "script ran: " + 6 * 7;</script>
</body></html>
EOF
    same "the libraries not found" "$(ldd /usr/lib/chromium/chromium-headless-shell | grep -c 'not found')" "0"
    mkdir "$WORK/profile"
    chown test "$WORK/profile"
    OUT=$(su test -c "bash -lc 'chromium-headless-shell --headless --dump-dom \
        --user-data-dir=$WORK/profile file://$WORK/page.html'" 2> "$WORK/chromium.err") ||
        fail "chromium-headless-shell: status $?: $(tail -n 3 "$WORK/chromium.err" | tr '\n' ' ')"
    has "the dumped DOM" "$OUT" '<p id="out">script ran: 42</p>'
    same "the browser's complaints of D-Bus" "$(grep -c 'dbus/' "$WORK/chromium.err")" "0"
}

if ! declare -F "row_$ROW" > /dev/null; then
    echo "GNUROW FAIL $ROW: no such row"
    exit 2
fi
rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK" || exit 2
"row_$ROW"
if [ -n "$FAILED" ]; then
    echo "GNUROW FAIL $ROW: $FAILED"
    exit 1
fi
echo "GNUROW OK $ROW"
