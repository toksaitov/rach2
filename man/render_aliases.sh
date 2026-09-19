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
# page has been rendered. The rendered text is gzipped under the plain name
# already, and the copy keeps it so: the three hundred applet aliases of the
# family pages would otherwise be seven megabytes. OUTDIR/aliases.made lists
# the files this run made;
# the next run removes what the previous one listed first, so an alias taken
# out of the list leaves no copy behind for an image to stage. An alias
# whose target is not a page, or a copy that cannot be written, fails the
# run: `mancheck` names the fault, and the build stops here so that no image
# stages an incomplete manual.
set -u
aliases=$1
out=$2
made=$out/aliases.made
if [ -f "$made" ]; then
    while read -r old; do
        rm -f "$old"
    done < "$made"
fi
: > "$made.new"
while read -r name section target; do
    case "$name" in ''|'#'*) continue ;; esac
    src="$out/cat$section/$target.$section"
    if [ ! -f "$src" ]; then
        echo "render_aliases: $name($section) names no rendered page $target" >&2
        exit 1
    fi
    text="$out/cat$section/$name.$section"
    html="$out/html/$name.$section.html"
    cp "$src" "$text.new" && mv "$text.new" "$text" || {
        echo "render_aliases: $text: cannot write the copy" >&2
        exit 1
    }
    cp "$out/html/$target.$section.html" "$html.new" && mv "$html.new" "$html" || {
        echo "render_aliases: $html: cannot write the copy" >&2
        exit 1
    }
    printf '%s\n%s\n' "$text" "$html" >> "$made.new"
done < "$aliases"
mv "$made.new" "$made"
