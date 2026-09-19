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
# page has been rendered. An alias whose target is not a page is skipped and
# counted here; `mancheck` is what refuses it, so a tree whose pages are still
# being written builds.
set -u
aliases=$1
out=$2
skipped=0
while read -r name section target; do
    case "$name" in ''|'#'*) continue ;; esac
    src="$out/cat$section/$target.$section"
    if [ ! -f "$src" ]; then
        skipped=$((skipped + 1))
        continue
    fi
    cp "$src" "$out/cat$section/$name.$section"
    cp "$out/html/$target.$section.html" "$out/html/$name.$section.html"
done < "$aliases"
[ "$skipped" -eq 0 ] || echo "render_aliases: $skipped aliases name a page not rendered yet; mancheck refuses them" >&2
exit 0
