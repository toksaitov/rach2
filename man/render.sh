#!/usr/bin/env sh
#
# render.sh: one manual page from mdoc to the plain text the machine reads.
#
# Usage: render.sh SOURCE OUTDIR
#
# SOURCE is man/manN/NAME.N. The page is checked with `mandoc -T lint` at the
# warning level, since a page with a warning is a page the machine renders
# wrong, then rendered with `mandoc -T ascii` and its overstrikes removed: the
# renderer marks bold and underline with backspaces, which the console has no
# use for and BusyBox's `man` would hand to a `col` this system does not
# build. The text goes to OUTDIR/catN/NAME.N, which the manifests stage under
# /usr/share/man on the images, and an HTML rendering to OUTDIR/html/NAME.N.html
# for reading off the machine. The operating system named in the page's header
# and footer is Rach2, given here so every page says it once.
set -u
src=$1
out=$2
name=$(basename "$src")
section=${name##*.}
bs=$(printf '\b')

mandoc -T lint -W warning "$src" || {
    echo "render: $src: mandoc found a warning; the page is refused" >&2
    exit 1
}
mkdir -p "$out/cat$section" "$out/html"
# The renderer's status is checked on its own: in a pipeline only the last
# command's would be, and a page it failed on would be published as the
# empty or partial text it left.
mandoc -I os=Rach2 -T ascii "$src" > "$out/cat$section/$name.raw" || {
    rm -f "$out/cat$section/$name.raw"
    echo "render: $src: mandoc could not render the page" >&2
    exit 1
}
sed "s/.$bs//g" < "$out/cat$section/$name.raw" > "$out/cat$section/$name.new" &&
    rm -f "$out/cat$section/$name.raw" &&
    mv "$out/cat$section/$name.new" "$out/cat$section/$name" || exit 1
mandoc -I os=Rach2 -T html "$src" > "$out/html/$name.html.new" &&
    mv "$out/html/$name.html.new" "$out/html/$name.html" || exit 1
