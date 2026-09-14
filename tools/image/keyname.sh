#!/usr/bin/env sh
#
# keyname.sh: refuse to stage a recovery artifact whose name does not say what
# it carries.
#
# Make reads `krffs/recovery_authorized_keys` when it starts, and that decides
# whether the recovery image, its ISO and the install stick are built under
# their plain names or under `-with-key` ones. A key that appears after that
# decision, which is what `make recovery-key rach2-install-stick` does in one
# command, would be staged into an artifact whose name says it answers nobody.
# The name is the guard a person reads before writing a stick, so the build
# stops here instead.
#
# Usage: keyname.sh PATH
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
