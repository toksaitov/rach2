#!/usr/bin/env sh
#
# render_aliases.sh: the pages a second name reaches, as copies.
#
# Usage: render_aliases.sh ALIASES OUTDIR
#
# `man/aliases` has one `NAME SECTION TARGET` a line: `man NAME` on the
# machine finds the page TARGET of that section. BusyBox's `man` follows no
# links between preformatted pages, so each alias is a copy of the rendered
# target under its own name, in OUTDIR/catN and OUTDIR/html, made after every
# page has been rendered. An alias whose target has not been rendered is a
# fault, and the build stops on it.
set -u
aliases=$1
out=$2
status=0
while read -r name section target; do
    case "$name" in ''|'#'*) continue ;; esac
    src="$out/cat$section/$target.$section"
    if [ ! -f "$src" ]; then
        echo "render_aliases: $name($section): its target $target.$section is not rendered" >&2
        status=1
        continue
    fi
    cp "$src" "$out/cat$section/$name.$section"
    cp "$out/html/$target.$section.html" "$out/html/$name.$section.html"
done < "$aliases"
exit $status
