#!/bin/sh
# The single-quoted texts below are mkext2.sh's own lines, not expansions.
# shellcheck disable=SC2016
#
# mkext2_overlay_test.sh: what an overlay adds to a volume of
# tools/image/mkext2.sh has the same modes whatever the umask it was made
# under.
#
# A copy of mkext2.sh runs in a private tree with the built stager, the
# set-id guard's source and an allowlist of the test's own, over a manifest
# that stages /etc/motd, /usr and /tmp, mode 1777. The overlay copies a
# build tree into the staged one, as vendor/gnu/install_root.sh copies the
# GNU layer's, and writes /tmp/scratch itself. The build tree has a program
# in /usr/libexec, which the allowlist declares a file the overlay installs,
# so the guard requires the directories above it to be writable by root
# alone, as it requires them of the GNU root's /usr/libexec/sshd-session; a
# data file; and a link to a file outside it, mode 0666. The volume is made
# under umask 022 and again under umask 002, the build tree each time under
# the same umask; 002 leaves the tree's directories and data file writable
# by their group, as a build in an account of Ubuntu's does. Each volume must
# pass mkext2.sh's checks, the guard's among them; the guard's listing of
# the two, each node's path, mode, owner and size, must be the same, with
# /usr/libexec 0755, /tmp 1777 as the manifest asks, and the data file and
# /tmp/scratch 0644; and the file the link names must keep its mode.
#
# With --mutate each fault below is put into the copy of mkext2.sh and must
# fail its row; the rows run again on the copy as it is at the end.
#
# --texts puts each fault into the copy and runs no row.
#
# Usage: sh tests/system/mkext2_overlay_test.sh [--mutate | --texts]
#
set -u

case ${1:-} in
    '') mutate=0 ;;
    --mutate) mutate=1 ;;
    --texts) mutate=2 ;;
    *) echo 'mkext2-overlay-test: usage: mkext2_overlay_test.sh [--mutate | --texts]' >&2; exit 2 ;;
esac

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
if [ "${1:-}" != --texts ]; then
    . "$repo/tests/system/runlock.sh"
    runlock_hold "$repo" mkext2-overlay-test "$repo/tests/system/mkext2_overlay_test.sh" "$@"
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/mkext2-overlay.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
# mkext2.sh finds its tree with `cd` and `pwd`, which fold a doubled slash.
work=$(CDPATH='' cd -- "$work" && pwd)

mkdir -p "$work/saved" "$work/tools/image" "$work/tools/setup" "$work/tools/inspect" \
    "$work/build/krffs"
cp "$repo/tools/image/mkext2.sh" "$work/saved/mkext2.sh"
cp "$work/saved/mkext2.sh" "$work/tools/image/mkext2.sh"
if [ "$mutate" -ne 2 ]; then
    if [ ! -x "$repo/build/krffs/stageimage.krffs" ]; then
        echo 'mkext2-overlay-test: no build/krffs/stageimage.krffs; run make' >&2
        exit 1
    fi
    cp "$repo/build/krffs/stageimage.krffs" "$work/build/krffs/"
    cp "$repo/tools/setup/e2fsprogs_path.sh" "$work/tools/setup/"
    cp "$repo/tools/inspect/setidcheck.c" "$work/tools/inspect/"
fi

cat > "$work/tools/inspect/setid.allow" << 'EOF'
manifest overlay-test
trusted overlay 0755:0:0 /usr/libexec/probe build/layer/usr/libexec/probe
sentinel /etc/motd 0644:0:0
init none
EOF
cat > "$work/overlay-test.manifest" << 'EOF'
dir 0755 /etc
text 0644 /etc/motd hello\n
dir 0755 /usr
dir 1777 /tmp
EOF
cat > "$work/overlay.sh" << 'EOF'
#!/bin/sh
set -e
cp -a "$OVERLAY_LAYER/." "$1/"
printf 'scratch\n' > "$1/tmp/scratch"
EOF
chmod 0755 "$work/overlay.sh"

failures=0
fail() {
    echo "mkext2-overlay-test: failed: $1"
    failures=$((failures + 1))
}

# layer MASK: the build tree the overlay copies, made under MASK as a port's
# install makes it: directories by `mkdir -p`, the program with the mode
# install(1) gives it, and the data file by the shell. The link is made
# under umask 002 in both trees: Linux gives every link mode 0777, and macOS
# gives it the umask's, which mke2fs copies, so the link has one mode in
# both and is writable by its group on either system.
layer() {
    rm -rf "$work/build/layer"
    (
        umask "$1"
        mkdir -p "$work/build/layer/usr/libexec" "$work/build/layer/usr/share/probe"
        printf '#!/bin/sh\n' > "$work/build/layer/usr/libexec/probe"
        chmod 0755 "$work/build/layer/usr/libexec/probe"
        printf 'data\n' > "$work/build/layer/usr/share/probe/data"
        umask 002
        ln -s "$work/outside" "$work/build/layer/usr/share/probe/outside"
    )
}

# volume MASK NAME: make NAME.ext2 under MASK from a build tree made under
# it, and list its nodes without their inode numbers in NAME.nodes.
volume() {
    layer "$1"
    rm -f "$work/$2.ext2" "$work/$2.nodes"
    (
        umask "$1"
        cd "$work" &&
            OVERLAY_LAYER="$work/build/layer" sh tools/image/mkext2.sh \
                --overlay "$work/overlay.sh" "$work/overlay-test.manifest" "$work/$2.ext2" 16
    ) > "$work/$2.log" 2>&1 || return 1
    "$work/build/tools/inspect/setidcheck" --nodes --ext2 "$work/$2.ext2" |
        awk '{ print $1, $2, $3, $4, $5 }' > "$work/$2.nodes"
}

# mode NAME PATH: the mode, with its type, that NAME.nodes lists for PATH.
mode() {
    [ -f "$work/$1.nodes" ] || return 0
    awk -v p="$2" '$1 == p { print $2 }' "$work/$1.nodes"
}

# made MASK NAME: whether volume makes NAME under MASK; when it does not,
# the last lines mkext2.sh wrote are shown.
made() {
    volume "$1" "$2" && return 0
    tail -n 3 "$work/$2.log" | sed 's/^/mkext2-overlay-test:   /'
    return 1
}

rows() {
    : > "$work/outside"
    chmod 0666 "$work/outside"
    made 022 tight || fail 'the volume made under umask 022 passes the checks'
    made 002 loose || fail 'the volume made under umask 002 passes the checks'
    if [ ! -s "$work/tight.nodes" ] || [ ! -s "$work/loose.nodes" ]; then
        fail 'the two volumes list the same nodes, modes, owners and sizes'
    elif ! cmp -s "$work/tight.nodes" "$work/loose.nodes"; then
        diff "$work/tight.nodes" "$work/loose.nodes" | head -n 6 | sed 's/^/mkext2-overlay-test:   /'
        fail 'the two volumes list the same nodes, modes, owners and sizes'
    fi
    if [ "$(mode loose /usr/libexec)" != 40755 ] || [ "$(mode loose /usr/libexec/probe)" != 100755 ] ||
        [ "$(mode loose /usr/share/probe/data)" != 100644 ] ||
        [ "$(mode loose /tmp/scratch)" != 100644 ] || [ "$(mode loose /tmp)" != 41777 ]; then
        fail 'the added nodes are writable by root alone, and /tmp as the manifest asks'
    fi
    [ -n "$(find "$work/outside" -perm 0666)" ] ||
        fail 'the file a link names outside the tree keeps its mode'
}

# fault NAME OLD NEW EXPECT: the copy of mkext2.sh with its one line OLD
# replaced by NEW must fail the row named EXPECT.
fault() {
    OLD=$2 NEW=$3 awk '
        $0 == ENVIRON["OLD"] { print ENVIRON["NEW"]; n++; next }
        { print }
        END { exit n != 1 }' "$work/saved/mkext2.sh" > "$work/tools/image/mkext2.sh" || {
        echo "mkext2-overlay-test: fault $1: its line is not once in tools/image/mkext2.sh" >&2
        exit 1
    }
    [ "$mutate" -ne 2 ] || return 0
    before=$failures
    out=$(rows)
    failures=$before
    printf '%s\n' "$out"
    case $out in
        *"failed: $4"*) echo "mkext2-overlay-test: result: $1 fails its row" ;;
        *) echo "mkext2-overlay-test: fault passed: $1" >&2; failures=$((failures + 1)) ;;
    esac
}

if [ "$mutate" -ge 1 ]; then
    line='    find "$TREE" ! -type l \( -perm -020 -o -perm -002 \) -exec chmod go-w {} +'
    fault no-normalise "$line" '    :' 'the volume made under umask 002 passes the checks'
    fault links-followed "$line" \
        '    find "$TREE" \( -perm -020 -o -perm -002 \) -exec chmod go-w {} +' \
        'the file a link names outside the tree keeps its mode'
    cp "$work/saved/mkext2.sh" "$work/tools/image/mkext2.sh"
fi
if [ "$mutate" -eq 2 ]; then
    echo 'mkext2-overlay-test: texts: every fault is once in its source'
    exit 0
fi

rows
if [ "$failures" -ne 0 ]; then
    echo "mkext2-overlay-test: result: fail: $failures"
    exit 1
fi
echo 'mkext2-overlay-test: result: pass: an overlay adds the same modes under umask 022 and 002'
