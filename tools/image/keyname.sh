#!/usr/bin/env sh
#
# keyname.sh: stop the build of a recovery artifact whose name does not say
# whether it has a recovery key.
#
# Make reads `krffs/recovery_authorized_keys` when it starts, and that decides
# whether the recovery image, its ISO, the install stick, the sealed disk and
# the GRUB disk are built under their plain names or under `-with-key` ones;
# the two disks have the recovery image on their recovery partitions. A key
# made after that decision, as `make recovery-key rach2-install-stick` makes
# one in a single command, would be staged into an artifact whose name says it
# has no key. A person reads the name before writing a stick, so the build
# stops here instead. This check also keeps `make release` from building an
# artifact with a key made in the same command.
#
# Usage: keyname.sh PATH
#
# The exit status is 0 when the name of PATH matches the tree, 1 when it does
# not, and 2 for a usage error.
set -u
[ -n "${1:-}" ] || { echo "usage: keyname.sh PATH" >&2; exit 2; }
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
name="$(basename "$1")"
case "$name" in
    *-with-key.*) says_key=yes ;;
    *)            says_key=no ;;
esac
if [ -f "$REPO/krffs/recovery_authorized_keys" ]; then
    has_key=yes
else
    has_key=no
fi
[ "$says_key" = "$has_key" ] && exit 0
if [ "$has_key" = yes ]; then
    echo "keyname: $name would carry this tree's key under a name that says it does not;" >&2
    echo "keyname: the key arrived after make chose the names, so run the same command again" >&2
else
    echo "keyname: $name is named for a key this tree has not got;" >&2
    echo "keyname: run 'make recovery-key' first, or remove the artifact and build again" >&2
fi
exit 1
