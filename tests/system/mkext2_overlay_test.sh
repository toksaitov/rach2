#!/bin/sh
# The single-quoted dollars below are the makefile's and the stand-ins', not
# expansions.
# shellcheck disable=SC2016
#
# mkext2_overlay_test.sh: what an overlay adds to a volume of
# tools/image/mkext2.sh has the same modes whatever the umask it was made
# under, and a failing mke2fs or debugfs leaves its diagnostic on stderr.
#
# A copy of mkext2.sh runs in a private tree with the built stager, the set-id
# guard's source, a makefile that builds the guard by mk/host.mk's rule, as
# mkext2.sh asks make for it, and an allowlist of the test's own, over a
# manifest that stages /etc/motd, /usr and /tmp, mode 1777. The overlay copies a
# build tree into the staged one, as vendor/gnu/install_root.sh copies the GNU
# layer's, and writes /tmp/scratch itself. The build tree has a program in
# /usr/libexec, which the allowlist declares a file the overlay installs, so the
# guard requires the directories above it to be writable by root alone, as it
# requires them of the GNU root's /usr/libexec/sshd-session; a data file; and a
# link to a file outside it, mode 0666. The volume is made under umask 022 and
# again under umask 002, the build tree each time under the same umask; 002
# leaves the tree's directories and data file writable by their group, as a
# build in an account of Ubuntu's does. Each volume must pass mkext2.sh's
# checks, the guard's among them; the guard's listing of the two, each node's
# path, mode, owner and size, must be the same, with /usr/libexec 0755, /tmp
# 1777 as the manifest asks, and the data file and /tmp/scratch 0644; and the
# file the link names must keep its mode.
#
# Two more volumes are made from the manifest alone, each with a stand-in
# first on the path for mke2fs or for debugfs that writes a line and exits
# with status 1. mkext2.sh must fail, write its own message and the line,
# and leave no image under the target's name.
#
# MKEXT2 is the mkext2.sh the rows run, the tree's by default.
#
# Usage: sh tests/system/mkext2_overlay_test.sh [MKEXT2]
#
set -u

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
[ "$#" -le 1 ] || {
    echo 'mkext2-overlay-test: usage: mkext2_overlay_test.sh [MKEXT2]' >&2
    exit 2
}
mkext2=${1:-$repo/tools/image/mkext2.sh}
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" mkext2-overlay-test "$repo/tests/system/mkext2_overlay_test.sh" "$@"
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-mkext2-overlay.XXXXXX") || exit 1
# mkext2.sh finds its tree with `cd` and `pwd`, which fold a doubled slash.
work=$(CDPATH='' cd -- "$work" && pwd)

mkdir -p "$work/tools/image" "$work/tools/setup" "$work/tools/inspect" "$work/build/krffs" \
    "$work/mk"
cp "$mkext2" "$work/tools/image/mkext2.sh"
if [ ! -x "$repo/build/krffs/stageimage.krffs" ]; then
    echo 'mkext2-overlay-test: no build/krffs/stageimage.krffs; run make' >&2
    exit 1
fi
cp "$repo/build/krffs/stageimage.krffs" "$work/build/krffs/"
cp "$repo/tools/setup/e2fsprogs_path.sh" "$work/tools/setup/"
cp "$repo/tools/inspect/setidcheck.c" "$work/tools/inspect/"
cp "$repo/mk/host.mk" "$work/mk/"
printf '%s\n' 'include mk/host.mk' \
    'build/tools/inspect/setidcheck: tools/inspect/setidcheck.c' \
    '	@mkdir -p $(dir $@)' '	$(HOST_LINK)' > "$work/Makefile"

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
# fail ROW: report that ROW failed and count it.
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

# fails TOOL: a directory of mke2fs, debugfs and e2fsck that run the
# programs mkext2.sh would find, but for TOOL, which writes a line to stderr
# and exits with status 1.
fails() {
    rm -rf "$work/fail-$1"
    mkdir "$work/fail-$1"
    for t in mke2fs debugfs e2fsck; do
        if [ "$t" = "$1" ]; then
            printf '#!/bin/sh\necho "injected failure in %s" >&2\nexit 1\n' "$t"
        else
            printf '#!/bin/sh\nexec "%s" "$@"\n' \
                "$(sh -c '. "$1" && command -v "$2"' sh "$repo/tools/setup/e2fsprogs_path.sh" "$t")"
        fi > "$work/fail-$1/$t"
        chmod 0755 "$work/fail-$1/$t"
    done
}

# refused TOOL TEXT: whether mkext2.sh, with TOOL failing, fails, writes
# TEXT and TOOL's line, and leaves no image; when it does not, the last
# lines it wrote are shown.
refused() {
    fails "$1"
    rm -f "$work/fail.ext2"
    if (cd "$work" && PATH="$work/fail-$1:$PATH" sh tools/image/mkext2.sh \
            "$work/overlay-test.manifest" "$work/fail.ext2" 16) > "$work/fail-$1.log" 2>&1 ||
        ! grep -qF "$2" "$work/fail-$1.log" ||
        ! grep -qF "injected failure in $1" "$work/fail-$1.log" || [ -e "$work/fail.ext2" ]; then
        tail -n 3 "$work/fail-$1.log" | sed 's/^/mkext2-overlay-test:   /'
        return 1
    fi
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

# Run every row on the copy of mkext2.sh.
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
    refused mke2fs 'mkext2: mke2fs failed with status 1' ||
        fail 'a failing mke2fs leaves its diagnostic and no image'
    refused debugfs 'mkext2: debugfs refused a command (status 1):' ||
        fail 'a failing debugfs leaves its diagnostic and no image'
}

rows
if [ "$failures" -ne 0 ]; then
    echo "mkext2-overlay-test: result: fail: $failures"
    exit 1
fi
echo 'mkext2-overlay-test: result: pass: an overlay adds the same modes under umask 022 and 002, and a failing mke2fs or debugfs leaves its diagnostic'
