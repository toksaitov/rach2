#!/usr/bin/env sh

#
# render_man.sh: render the ports' manual pages for the system's man.
#
# Usage: render_man.sh MANDIR
#
# MANDIR is a tree's /usr/share/man. The ports install their pages as troff
# sources under manN, and the system's man reads only plain text, so each
# page of sections 1, 5, 7 and 8 is rendered here with the build host's
# mandoc, as man/render.sh renders this project's pages: to ASCII, without
# the backspaces that mark bold and underline, gzipped, to
# MANDIR/gnu/catN/NAME.N. A suffix after the section, as OpenSSL's `1ossl`,
# is dropped, and of two pages that would have one name the first keeps it.
# mandoc runs in MANDIR, where a page's `.so` request names the page it
# stands for. A page mandoc renders to nothing is left out.
#

set -e

MANDIR="$1"
if [ -z "$MANDIR" ] || [ ! -d "$MANDIR" ]; then
    echo "usage: render_man.sh MANDIR" >&2
    exit 2
fi
bs=$(printf '\b')
count=0
for src in "$MANDIR"/man[1578]/*; do
    [ -f "$src" ] || continue
    dir=${src%/*}
    section=${dir##*/man}
    base=${src##*/}
    base=${base%.gz}
    name=${base%.*}
    out="$MANDIR/gnu/cat$section/$name.$section"
    [ -e "$out" ] && continue
    mkdir -p "$MANDIR/gnu/cat$section"
    (cd "$MANDIR" && mandoc -T ascii "man$section/${src##*/}" 2>/dev/null) |
        sed "s/.$bs//g" > "$out.txt" || :
    if [ -s "$out.txt" ]; then
        gzip -9 -n < "$out.txt" > "$out"
        count=$((count + 1))
    fi
    rm -f "$out.txt"
done
echo "gnu: man: $count pages rendered"
