#!/bin/sh
#
# machine_logins.sh: copy the coding agents' logins of an account on the
# installed Lenovo to this host, or back, so that a replaced root, or a new
# machine, keeps them.
#
# Usage: machine_logins.sh pull|push DIR [USER [ADDRESS]]
#
# DIR holds the logins as a tree laid out from the machine's root: those of
# USER, root unless given, are under DIR/HOME, where HOME is USER's home on
# the machine without its leading slash, such as DIR/root or DIR/home/alice.
# `make machine LOGINS=DIR` stages the same tree's DIR/root in the machine's
# root image (mk/gnu-image.mk). ADDRESS is the machine's,
# RACH2_MACHINE_ADDRESS or 192.168.0.106; RACH2_MACHINE_MAC names the station
# address it must report before anything is copied, 28:d2:44:7e:e0:af by
# default, the Lenovo's. The script logs in as root by the development key,
# krffs/rach2_demo_ed25519.
#
# pull copies the files the agents keep their sign-ins in, the ones the
# manual pages name and tests/system/accept/agents_guest.sh reads,
# ~/.claude/.credentials.json, ~/.codex/auth.json,
# ~/.gemini/antigravity-cli/antigravity-oauth-token, ~/.config/muse/auth.json
# and ~/.config/gh/hosts.yml, and OpenCode's database,
# ~/.local/share/opencode/opencode.db, which holds its credentials and which
# sqlite3's .backup copies whole while OpenCode writes it. Each file and the
# directories above it keep their modes. The copy is made in a scratch
# directory on the machine's /dev/shm, sent as one tar archive and checked
# here by SHA-256 against the machine's; only a pull that has every file it
# found, and at least one, replaces the files of DIR, which are replaced
# one by one, and DIR's other files are left as they are. A file the machine
# lacks keeps DIR's copy, and the script names it.
#
# push copies every file of DIR/HOME into USER's home: a directory the home
# lacks is made with its mode in DIR, and a file is written beside its place
# and renamed over it, owned by USER, with its mode in DIR. Each file
# written is checked against DIR's by SHA-256. OpenCode's database is
# refused while an opencode process runs, since it keeps its writes in the
# database's journal, which the push removes with the database it replaces;
# `opencode service stop` ends its service.
#
# The files' contents are never printed: only their names. Exit 0 when
# every file was copied and checked, 1 when a step fails, 2 on a usage
# error.
set -u

usage()
{
    echo 'usage: machine_logins.sh pull|push DIR [USER [ADDRESS]]' >&2
    exit 2
}

[ "$#" -ge 2 ] && [ "$#" -le 4 ] || usage
command=$1
dir=$2
user=${3:-root}
address=${4:-${RACH2_MACHINE_ADDRESS:-192.168.0.106}}
mac=${RACH2_MACHINE_MAC:-28:d2:44:7e:e0:af}
case $command in pull | push) ;; *) usage ;; esac
case $user in
    "" | *[!a-z0-9_-]* | [!a-z_]*) echo "machine-logins: $user is not an account name" >&2; exit 2 ;;
esac
root=$(cd "$(dirname "$0")/../.." && pwd)
key=$root/krffs/rach2_demo_ed25519
chmod 600 "$key"

fail()
{
    echo "machine-logins: failed: $*" >&2
    exit 1
}

# The machine's half: run there by `sh -s -- COMMAND USER`, it prints USER's
# home for `home`; stages USER's login files in /dev/shm/rach2-logins for
# `pull` and prints `HASH  ./PATH` for each and `missing PATH` for each the
# home lacks; and for `push` writes the files of the archive
# /dev/shm/rach2-logins.tar into the home and prints `HASH  ./PATH` for each.
remote()
{
    cat << 'EOF'
set -u
FILES=".claude/.credentials.json .codex/auth.json
.gemini/antigravity-cli/antigravity-oauth-token .config/muse/auth.json
.config/gh/hosts.yml"
DB=.local/share/opencode/opencode.db
stage=/dev/shm/rach2-logins
fail() { echo "machine-logins: failed: $*" >&2; exit 1; }
line=$(awk -F: -v u="$2" '$1 == u { print; exit }' /etc/passwd)
[ -n "$line" ] || fail "the machine has no account $2"
uid=$(echo "$line" | cut -d: -f3)
gid=$(echo "$line" | cut -d: -f4)
home=$(echo "$line" | cut -d: -f6)
case "$home" in /*) ;; *) fail "$2's home, $home, is not a path" ;; esac
[ -d "$home" ] || fail "$2's home, $home, is not there"
cd "$home" || fail "$home cannot be entered"

# parents PATH: make the directories above PATH in the stage, with the
# home's modes.
parents()
{
    p=
    rest=${1%/*}
    while [ -n "$rest" ]; do
        c=${rest%%/*}
        if [ "$c" = "$rest" ]; then rest=; else rest=${rest#*/}; fi
        p=${p:+$p/}$c
        [ -d "$stage/$p" ] && continue
        mkdir "$stage/$p" && chmod "$(stat -c %a "$p")" "$stage/$p" || return 1
    done
}

case $1 in
home)
    echo "$home"
    ;;
pull)
    rm -rf "$stage" "$stage.tar" && mkdir -m 700 "$stage" || fail "$stage could not be made"
    for f in $FILES $DB; do
        if [ ! -f "$f" ]; then
            echo "missing $f"
            continue
        fi
        parents "$f" || fail "the directories above $f could not be made"
        if [ "$f" = "$DB" ]; then
            command -v sqlite3 > /dev/null || fail "the machine has no sqlite3 to copy $f with"
            sqlite3 "$f" ".backup '$stage/$f'" < /dev/null || fail "$f could not be copied"
            chmod "$(stat -c %a "$f")" "$stage/$f" || fail "$f's copy could not be given its mode"
        else
            cp -p "$f" "$stage/$f" || fail "$f could not be copied"
        fi
    done
    cd "$stage" && find . -type f | sort | while IFS= read -r f; do
        sha256sum "$f" || exit 1
    done || fail "the copies could not be hashed"
    ;;
push)
    rm -rf "$stage" && mkdir -m 700 "$stage" || fail "$stage could not be made"
    tar -xof "$stage.tar" -C "$stage" || fail "the archive could not be read"
    rm -f "$stage.tar"
    if [ -f "$stage/$DB" ]; then
        command -v pgrep > /dev/null || fail "the machine has no pgrep to look for opencode with"
        ! pgrep -x opencode > /dev/null ||
            fail "opencode runs on the machine; end it, and its service with opencode service stop"
        rm -f "$DB-wal" "$DB-shm" || fail "the old database's journal could not be removed"
    fi
    cd "$stage" || fail "$stage cannot be entered"
    find . -type d | sort | while IFS= read -r d; do
        d=${d#./}
        [ "$d" = . ] || [ -d "$home/$d" ] && continue
        { mkdir "$home/$d" && chmod "$(stat -c %a "$d")" "$home/$d" &&
            chown "$uid:$gid" "$home/$d"; } || exit 1
    done || fail "a directory could not be made in $home"
    find . -type f | sort | while IFS= read -r f; do
        t=$home/${f#./}
        { cp "$f" "$t.rach2-new" && chmod "$(stat -c %a "$f")" "$t.rach2-new" &&
            chown "$uid:$gid" "$t.rach2-new" && mv "$t.rach2-new" "$t"; } || exit 1
        h=$(sha256sum "$t" | cut -d' ' -f1)
        echo "$h  $f"
    done || fail "a file could not be written in $home"
    cd / && rm -rf "$stage"
    ;;
esac
EOF
}

# digest FILE: print FILE's SHA-256.
digest()
{
    if command -v sha256sum > /dev/null; then
        sha256sum "$1"
    else
        shasum -a 256 "$1"
    fi | cut -d' ' -f1
}

# check LIST TREE: every `HASH  ./PATH` line of LIST names a file of TREE
# with that SHA-256, and TREE has no other file. Prints the number checked.
check()
{
    n=0
    while read -r hash path; do
        [ "$hash" = missing ] && continue
        [ -f "$2/$path" ] && [ "$(digest "$2/$path")" = "$hash" ] ||
            fail "${path#./} differs from the machine's copy"
        n=$((n + 1))
    done < "$1"
    [ "$(cd "$2" && find . -type f | wc -l | tr -d ' ')" = "$n" ] ||
        fail "the copy has files the machine did not list"
    echo "$n"
}

work=
staged=
cleanup()
{
    [ -z "$staged" ] || machine 'rm -rf /dev/shm/rach2-logins /dev/shm/rach2-logins.tar' 2> /dev/null
    rm -rf ${work:+"$work"}
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$root/build/tmp" || exit 1
work=$(mktemp -d "$root/build/tmp/machine-logins.XXXXXX") || exit 1

# machine COMMAND: run COMMAND on the machine as root; each command ends
# after a minute without an answer from the server.
machine()
{
    ssh -i "$key" -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR \
        -o ServerAliveInterval=15 -o ServerAliveCountMax=4 \
        -o UserKnownHostsFile="$work/known" -o StrictHostKeyChecking=accept-new \
        "root@$address" "$@"
}

got=$(machine 'cat /sys/class/net/eth0/address' 2> "$work/err") || fail "$(tail -n 1 "$work/err")"
[ "$got" = "$mac" ] || fail "the machine reports station address $got, not $mac"
home=$(remote | machine "sh -s -- home $user") || fail "$user's home could not be read"
here=$dir/${home#/}

if [ "$command" = pull ]; then
    echo "machine-logins: pulling $user's logins from $address"
    staged=yes
    remote | machine "sh -s -- pull $user" > "$work/list" || fail "the machine could not copy them"
    machine 'tar -C /dev/shm/rach2-logins -cf - .' > "$work/pull.tar" ||
        fail "the copy did not arrive"
    mkdir "$work/new" && tar -xpf "$work/pull.tar" -C "$work/new" ||
        fail "the copy could not be unpacked"
    n=$(check "$work/list" "$work/new") || exit 1
    [ "$n" -gt 0 ] || fail "the machine has none of $user's login files; $here is as it was"
    mkdir -p "$here" && chmod 700 "$dir" || fail "$here could not be made"
    tar -xpf "$work/pull.tar" -C "$here" || fail "the copy could not be written to $here"
    sed -n 's|^[0-9a-f]*  \./|machine-logins: pulled: |p' "$work/list"
    sed -n 's|^missing |machine-logins: not on the machine: |p' "$work/list"
    echo "machine-logins: result: $n files of $user in $here"
else
    [ -d "$here" ] || fail "$here is not a directory"
    odd=$(cd "$here" && find . ! -type f ! -type d | head -n 1)
    [ -z "$odd" ] || fail "$here has $odd, which is neither a file nor a directory"
    ! (cd "$here" && find . | LC_ALL=C grep -q '[[:cntrl:]]') ||
        fail "$here has a name with a control character"
    echo "machine-logins: pushing $here to $user on $address"
    staged=yes
    (cd "$here" && COPYFILE_DISABLE=1 tar --no-xattrs -cf - .) |
        machine 'cat > /dev/shm/rach2-logins.tar' || fail "the archive did not arrive"
    remote | machine "sh -s -- push $user" > "$work/list" || fail "the machine could not write them"
    n=$(check "$work/list" "$here") || exit 1
    sed -n 's|^[0-9a-f]*  \./|machine-logins: pushed: |p' "$work/list"
    echo "machine-logins: result: $n files in $user's home, $home"
fi
