#!/bin/bash
# go.sh TAG...: boot ./kernel-NAME for each TAG NAME-N with rach2.txt, and
# Debian's kernel for linux-N with linux.txt; load.c and rcat.c (80-94 and
# 80-100) are built beside it into tools.ext2.
cd "$(dirname "$0")"
[ -f tools.ext2 ] || {
    gcc -O2 -static -pthread -o load load.c && gcc -O2 -static -o rcat rcat.c &&
    truncate -s 64M tools.ext2 && mke2fs -q -F -t ext2 tools.ext2
    debugfs -w -R 'write load load' tools.ext2; debugfs -w -R 'write rcat rcat' tools.ext2
}
for t in "$@"; do
    case $t in
    linux*) python3 vm.py linux none linux.txt $t.out ;;
    *) python3 vm.py rach2 ./kernel-${t%%-*} rach2.txt $t.out ;;
    esac
    echo "go: $t: exit $?"
done
echo "go: done"
