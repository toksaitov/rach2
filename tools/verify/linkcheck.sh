#!/usr/bin/env sh

#
# linkcheck.sh: every Markdown link in the in-house documents names a file
# that is there, and every anchor names a heading that is in it.
#
# A document that points at a file a commit moved is worse than one that says
# nothing: a reader follows it and learns that the tree cannot be trusted.
# URLs are left alone, since this runs with no network, and so is an anchor of
# the form `L120`, which names a line and not a heading. `dev-log/` is
# excluded: those records are history, and a path inside one is written as the
# repository sees it rather than as a link from that directory.
#
# Usage: linkcheck.sh [FILE...]   (default: the in-house documents)
#

set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO" || exit 1

if [ "$#" -gt 0 ]; then
    files="$*"
else
    files=$(find . -name '*.md' \
        -not -path './vendor/*' -not -path './build/*' -not -path './dev-log/*' \
        -not -path './ports/doom/src/*' -not -path './.git/*' |
        sed 's|^\./||' | sort)
fi

out=$(mktemp "${TMPDIR:-/tmp}/linkcheck.XXXXXX") || exit 1
trap 'rm -f "$out"' 0
count=0
for f in $files; do
    count=$((count + 1))
    if [ ! -r "$f" ]; then
        echo "linkcheck: $f: cannot read" >> "$out"
        continue
    fi
    dir=$(dirname "$f")
    # A link inside a code span is an example, not a link: `[text](target)`
    # in this file's own description of the form would otherwise be one.
    # shellcheck disable=SC2016 # the backticks are Markdown, not a command
    sed 's/`[^`]*`//g' "$f" | grep -o '\[[^][]*\]([^()]*)' | sed 's/.*(\(.*\))/\1/' |
    while read -r target; do
        case "$target" in
            http://*|https://*|mailto:*|'#'*|'') continue ;;
        esac
        anchor=""
        case "$target" in *'#'*) anchor=${target#*#}; target=${target%%#*} ;; esac
        [ -n "$target" ] || continue
        path="$dir/$target"
        if [ ! -e "$path" ]; then
            echo "linkcheck: $f: no such file: $target" >> "$out"
            continue
        fi
        case "$anchor" in ''|L[0-9]*) continue ;; esac
        case "$path" in *.md) ;; *) continue ;; esac
        # A heading makes an anchor of its text: lower case, spaces hyphens,
        # and everything that is not a letter, a digit, a hyphen or an
        # underscore dropped.
        if ! grep '^#' "$path" | sed 's/^#* *//' | tr '[:upper:]' '[:lower:]' |
             tr ' ' '-' | sed 's/[^a-z0-9_-]//g' | grep -qx -- "$anchor"; then
            echo "linkcheck: $f: $target has no heading '$anchor'" >> "$out"
        fi
    done
done

if [ -s "$out" ]; then
    cat "$out"
    echo "linkcheck: $(wc -l < "$out" | tr -d ' ') broken link(s)"
    exit 1
fi
echo "linkcheck: ok: every link in $count document(s) names a file that is there"
