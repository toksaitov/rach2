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

row_glibc() {
    same "getconf" "$(getconf GNU_LIBC_VERSION)" "glibc 2.44"
    has "the loader" "$(/lib/ld-linux.so.2 --version 2>&1 | head -n 1)" "2.44"
    has "the cache" "$(ldconfig -p | head -n 1)" "libs found in cache"
    same "a locale of the archive" "$(LC_ALL=en_US.UTF-8 locale charmap)" "UTF-8"
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

row_coreutils() {
    has "the version" "$(cat --version | head -n 1)" "(GNU coreutils) 9.12"
    same "seq, sort and head" "$(seq 1 10 | sort -rn | head -n 1)" "10"
    same "sha256sum" "$(printf abc | sha256sum)" "$ABC  -"
    same "date" "$(date -u -d @0 +%Y-%m-%d)" "1970-01-01"
}

row_bash() {
    # shellcheck disable=SC2016 # the dollars are the inner bash's
    same "an associative array" "$(bash -c 'declare -A m=([k]=v); echo "${m[k]}"')" "v"
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

# An autoconf package built on the machine as a user builds one: GNU sed's
# configure, which takes the build's name from the root's site file, since
# config.guess knows no system called Rach2, then make, and the sed it made.
row_autotools() {
    runs "the tarball" tar -xJf /tmp/gnu/sed.tar.xz -C "$WORK"
    cd "$WORK"/sed-* || {
        fail "no sed directory"
        return
    }
    output "configure" ./configure
    has "the site file" "$OUT" "loading site script"
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
# browser prints the DOM of a local page whose script writes into it, with
# the system bus found and the session bus of a login shell's.
row_chromium() {
    local bus
    cat > "$WORK/page.html" << 'EOF'
<!DOCTYPE html>
<html><body><p id="out">not run</p>
<script>document.getElementById("out").textContent = "script ran: " + 6 * 7;</script>
</body></html>
EOF
    same "the libraries not found" "$(ldd /usr/lib/chromium/chromium-headless-shell | grep -c 'not found')" "0"
    bus=$(bash -lc 'printf %s "$DBUS_SESSION_BUS_ADDRESS"')
    OUT=$(HOME=$WORK DBUS_SESSION_BUS_ADDRESS=$bus chromium-headless-shell --no-sandbox --headless \
        --dump-dom --user-data-dir="$WORK/profile" "file://$WORK/page.html" 2> "$WORK/chromium.err") ||
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
