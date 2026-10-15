#!/usr/bin/env sh

#
# linkcheck.sh: check that every Markdown link in the in-house documents
# points at a file that exists, and every anchor at a heading in that file.
#
# A link to a file that a commit moved or deleted leads the reader nowhere.
# URLs are left alone, since this runs with no network, and so is an anchor of
# the form `L120`, which is a line and not a heading. A link inside a code
# span is an example of the syntax, so code spans are removed first. The
# in-house documents are the Markdown files that git tracks, so the extracted
# vendor trees and any helper's worktree under `.claude/` are left out.
# `dev-log/` is left out too: its records are history, and a path inside one
# is relative to the repository root rather than to the document.
#
# An anchor is a heading's text in lower case, with spaces turned to hyphens
# and everything that is not a letter, a digit, a hyphen or an underscore
# dropped.
#
# Usage: linkcheck.sh [FILE...]   (default: the in-house documents)
#

set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO" || exit 1

if [ "$#" -gt 0 ]; then
    files="$*"
else
    files=$(git ls-files -- '*.md' ':!:dev-log/*' | sort)
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
