#!/usr/bin/env sh
#
# together_test.sh: each maker that writes several files of the suites at
# once runs once when all its files are asked for together, under each make.
#
# tests/system/suites.mk gives such a maker a pattern rule whose targets have
# one stem, which make runs once for all of them; a rule with grouped targets
# (`&:`) is read by GNU Make 3.81, the make of macOS, as an ordinary rule
# with a target named `&`, and a parallel request of its files runs the
# maker once for each. The makers are these, by script and mode:
#
#     loopdev_disks.sh, krffsvol.sh volumes, krffsvol.sh disks,
#     usbdisk_sticks.sh, metal_sticks.sh, metal_installed_disks.sh,
#     parts_disks.sh arch and parts_disks.sh shared
#
# The rows run on a private copy of the root Makefile, mk/ and the suites'
# fragment, for i686 and x86-64, under each MAKE given, `make` by default:
#
#   * no-grouped-targets: no rule of the makefiles has grouped targets, and
#     the make database has no target named `&`.
#   * makers: one pattern rule runs each maker, and no explicit rule does.
#   * MAKER: a make -j8 of all the files of the maker's rules, with every
#     other prerequisite taken as made (-o), runs the maker once and no other
#     maker twice, and leaves each file of each maker that ran: a file of
#     another maker that a rule reads is not removed as intermediate when
#     make ends.
#
# In the copy each maker is a script that records its run, waits a second,
# so that a second run of it would overlap the first, and writes empty files
# in place of its own. TOGETHER_SUITES_MK names the fragment to copy, by
# default the tree's; tests/proofs/together.json runs the rows with copies
# that have faults in them.
#
# Usage:
#     sh tests/system/together_test.sh [MAKE...]
#
set -eu
unset ARCH MAKEFLAGS MFLAGS

REPO=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
# Each MAKE given as a path is made absolute, since the rows run elsewhere.
n=$#
while [ "$n" -gt 0 ]; do
    mk=$1
    shift
    case $mk in
        */*) mk=$(CDPATH='' cd -- "$(dirname -- "$mk")" && pwd)/${mk##*/} ;;
    esac
    set -- "$@" "$mk"
    n=$((n - 1))
done
. "$REPO/tests/system/runlock.sh"
runlock_hold "$REPO" together-test "$REPO/tests/system/together_test.sh" "$@"
cd "$REPO"
WORK=
trap '[ -z "$WORK" ] || rm -rf "$WORK"' 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rach2-together.XXXXXX")

[ "$#" -gt 0 ] || set -- make

MAKERS='loopdev_disks.sh
krffsvol.sh volumes
krffsvol.sh disks
usbdisk_sticks.sh
metal_sticks.sh
metal_installed_disks.sh
parts_disks.sh arch
parts_disks.sh shared'

failed=

# pass ROW: report that ROW passed.
pass()
{
    echo "together-test: result: pass: $1"
}

# fail ROW DETAIL: report that ROW failed and add it to `failed`.
fail()
{
    echo "together-test: result: fail: $1: $2"
    failed="$failed $1"
}

T="$WORK/tree"
for f in Makefile tests/registry tests/Makefile krffs/Makefile krffs/manifest_deps.sh \
    krffs/programs.sh rach2/Makefile rach2/sources.mk ish/Makefile ish/sources.mk \
    utils/Makefile utils/programs.mk ports/doom/Makefile tools/etude/sources.mk \
    rach2/sys/arch/*/sources.mk krffs/manifests/*.manifest; do
    mkdir -p "$T/$(dirname "$f")"
    cp "$f" "$T/$f"
done
cp -R mk "$T/mk"
mkdir -p "$T/tests/system"
cp "${TOGETHER_SUITES_MK:-tests/system/suites.mk}" "$T/tests/system/suites.mk"
printf 'together-noop: ; @:\n' >"$WORK/noop.mk"

# The private tree's make, outside any admission of the tree that runs this
# test, its status in $rc and both streams in FILE.
tree_make() { # tree_make FILE MAKE ARGUMENTS...
    out=$1
    shift
    rc=0
    (cd "$T" && ETUDE_SITE=/nonexistent ETUDE_ADMITTED=/nonexistent \
        "$@") >"$out" 2>&1 || rc=$?
}

# The rules of the database DB that run a maker, one line for each maker as
# KEY|TARGETS|PREREQUISITES|PATTERN|EXPLICIT. KEY is the script the recipe
# runs, and its first argument when that is a word of lowercase letters.
# PATTERN counts the pattern rules that run it, whose stem is taken as
# `build`, and EXPLICIT the files of explicit rules that do, a target named
# `&` left out. Order-only prerequisites are among the prerequisites.
maker_rules() { # maker_rules DB
    awk '
        /^\t/ {
            if (head != "" && index($0, "\tsh tests/system/") == 1) {
                n = split(substr($0, 2), w, " ")
                key = w[2]
                sub(/.*\//, "", key)
                if (n >= 3 && w[3] ~ /^[a-z]+$/) key = key " " w[3]
                i = index(head, ":")
                targets = substr(head, 1, i - 1)
                prereqs = substr(head, i + 1)
                gsub(/\|/, " ", prereqs)
                if (targets ~ /^%\//) {
                    gsub(/%/, "build", targets)
                    pattern[key]++
                } else if (targets != "&") {
                    explicit[key]++
                } else {
                    targets = ""
                }
                if (!(key in order)) order[key] = ++keys
                all[key] = all[key] " " targets
                pre[key] = pre[key] " " prereqs
            }
            head = ""
            next
        }
        /^#/ || /^$/ { next }
        { head = (index($0, ":") && $0 !~ / [:?+!]?= /) ? $0 : "" }
        END {
            for (key in order) {
                t = all[key]; p = pre[key]
                gsub(/  +/, " ", t); gsub(/^ | $/, "", t)
                gsub(/  +/, " ", p); gsub(/^ | $/, "", p)
                print order[key] "|" key "|" t "|" p "|" pattern[key] + 0 "|" explicit[key] + 0
            }
        }' "$1" | sort -n | cut -d'|' -f2-
}

# Field N of each line of the rules file RULES whose key is KEY.
field() { # field RULES KEY N
    awk -F'|' -v k="$2" -v n="$3" '$1 == k { print $n }' "$1"
}

# The explicit prerequisites of TARGET in the database DB, one a line.
explicit_prereqs() { # explicit_prereqs DB TARGET
    awk -v t="$2:" '
        prev != "# Not a target:" && index($0, t) == 1 {
            n = split(substr($0, length(t) + 1), a, " ")
            for (i = 1; i <= n; i++) if (a[i] != "|") print a[i]
            exit
        }
        { prev = $0 }' "$1"
}

# A maker of the private tree for SCRIPT: it records KEY in $WORK/log, waits
# a second and writes empty files at the targets of the rule that runs it in
# that mode, from the rules file RULES.
write_maker() { # write_maker RULES SCRIPT
    script=$2
    {
        cat <<'EOS'
#!/bin/sh
case ${1:-} in
EOS
        awk -F'|' -v s="$script" '$1 == s || index($1, s " ") == 1' "$1" |
        while IFS='|' read -r key targets _; do
            mode=${key#"$script"}
            mode=${mode# }
            printf "%s)\n    echo '%s' >>'%s'\n    files='%s' ;;\n" "${mode:-*}" "$key" \
                "$WORK/log" "$targets"
        done
        cat <<'EOS'
esac
sleep 1
for f in $files; do mkdir -p "${f%/*}" && : >"$f" || exit 1; done
EOS
    } >"$T/tests/system/$script"
}

if grep -n '^[^#]*&:' "$T/Makefile" "$T"/mk/*.mk "$T/tests/system/suites.mk" >"$WORK/grouped"; then
    fail no-grouped-targets "grouped targets in the makefiles: $(tr '\n' ' ' <"$WORK/grouped")"
else
    pass no-grouped-targets
fi

for mk in "$@"; do
    version=$("$mk" --version 2>/dev/null | sed -n '1s/^GNU Make /make /p')
    [ -n "$version" ] || {
        fail "$mk" "not a GNU make"
        continue
    }
    for arch in i686 x86_64; do
        row="$version: $arch"
        db="$WORK/db"
        tree_make "$db" "$mk" --no-print-directory -r -pn -f Makefile -f "$WORK/noop.mk" \
            ARCH="$arch" together-noop
        if [ "$rc" -ne 0 ]; then
            fail "$row: database" "status $rc: $(tail -n 5 "$db")"
            continue
        fi
        if grep -q '^&:' "$db"; then
            fail "$row: no-grouped-targets" "the database has a target named &"
        else
            pass "$row: no-grouped-targets"
        fi

        rules="$WORK/rules"
        maker_rules "$db" >"$rules"
        echo "$MAKERS" | while IFS= read -r key; do
            kinds="$(field "$rules" "$key" 4) pattern, $(field "$rules" "$key" 5) explicit"
            [ "$kinds" = '1 pattern, 0 explicit' ] || echo "$key: rules that run it: $kinds"
        done >"$WORK/kinds"
        if [ -s "$WORK/kinds" ]; then
            fail "$row: makers" "$(tr '\n' ';' <"$WORK/kinds")"
        else
            pass "$row: makers"
        fi

        # Every prerequisite that no maker makes is taken as made, and exists
        # so that a pattern rule may use it.
        outputs=$(cut -d'|' -f2 "$rules" | tr '\n' ' ')
        old=
        for p in $(cut -d'|' -f3 "$rules") $(for o in $outputs; do explicit_prereqs "$db" "$o"; done); do
            case " $outputs $old " in
                *" $p "*) continue ;;
            esac
            old="$old $p"
            [ -e "$T/$p" ] || { mkdir -p "$T/$(dirname "$p")" && : >"$T/$p"; }
        done
        oflags=
        for p in $old; do
            oflags="$oflags -o $p"
        done
        cut -d'|' -f1 "$rules" | cut -d' ' -f1 | sort -u >"$WORK/scripts"
        while IFS= read -r script; do
            write_maker "$rules" "$script"
        done <"$WORK/scripts"

        while IFS= read -r key; do
            targets=$(field "$rules" "$key" 2)
            if [ -z "$targets" ]; then
                fail "$row: $key" "no rule runs it"
                continue
            fi
            # shellcheck disable=SC2086
            (cd "$T" && rm -f $outputs)
            : >"$WORK/log"
            # shellcheck disable=SC2086
            tree_make "$WORK/out" "$mk" --no-print-directory -r -j8 ARCH="$arch" $oflags $targets
            runs=$(grep -cxF "$key" "$WORK/log" || :)
            twice=$(sort "$WORK/log" | uniq -d | tr '\n' ';')
            missing=
            while IFS= read -r ran; do
                for f in $(field "$rules" "$ran" 2); do
                    [ -f "$T/$f" ] || missing="$missing $f"
                done
            done <"$WORK/log"
            if [ "$rc" -ne 0 ]; then
                fail "$row: $key" "status $rc: $(tail -n 5 "$WORK/out" | tr '\n' ' ')"
            elif [ "$runs" -ne 1 ] || [ -n "$twice" ]; then
                fail "$row: $key" "ran $runs times; makers run twice: ${twice:-none}"
            elif [ -n "$missing" ]; then
                fail "$row: $key" "missing after make:$missing"
            else
                pass "$row: $key"
            fi
        done <<EOF
$MAKERS
EOF
    done
done

[ -z "$failed" ] || exit 1
