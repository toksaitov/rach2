#!/usr/bin/env sh
#
# gen_compile_commands.sh: generate compile_commands.json, the LSP compilation
# database clangd reads for per-file flags (run via `make compile-commands`).
#
# GCC has no clang-style -MJ, so the database is generated FROM the build's own
# commands: each in-house component is dry-run (`make -Bn`), wrapped recipe
# lines are joined back into one command each, and every joined command whose
# first word is the i686-elf cross GCC becomes one entry per C source operand
# (directory + file + the complete command). This covers the FREESTANDING /
# musl in-house code: the kernel (rach2) and the guest programs (tests, ish,
# utils). The host krffs FUSE tools (built with the host `cc` and dynamic
# `pkg-config` flags), and the vendored musl/busybox/dropbear and Doom trees,
# are intentionally excluded: clangd-on-the-kernel is the goal and those carry
# flags that do not belong in a portable database.
#
# The finished database is validated before it is published: the generator
# emits a known JSON subset, so a raw control character or a malformed entry
# is decidable line by line and fails the run instead of leaving an invalid
# file that reports success. `--selftest` runs the join, selection, escaping,
# and validation stages on fixtures, including the wrapped Homebrew recipe
# whose continuation line once matched through its libgcc path and wrote a
# raw tab into the database.
#
# The output (compile_commands.json at the repo root) is git-ignored and
# regenerable; rerun this after adding/removing a source or changing flags.
#
set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$REPO/compile_commands.json"

# Escape one string for a JSON double-quoted value. The four escapes cover
# everything a build command line can carry: backslash, double quote, tab,
# and carriage return. The validator rejects anything else.
json_escape() {
    printf '%s' "$1" | LC_ALL=C awk '
        NR > 1 { printf "\\n" }
        {
            gsub(/\\/, "\\\\")
            gsub(/"/, "\\\"")
            gsub(/\t/, "\\t")
            gsub(/\r/, "\\r")
            printf "%s", $0
        }'
}

# Join backslash-continued recipe lines from `make -Bn` into one logical
# command per output line. A continuation's leading blanks collapse to one
# space, so the joined line is the command the shell would have run. Without
# this join a continuation fragment can be selected as if it were a complete
# command.
join_continuations() {
    LC_ALL=C awk '
        {
            line = $0
            while (line ~ /\\$/ && (getline nxt) > 0) {
                sub(/\\$/, "", line)
                sub(/^[[:space:]]*/, "", nxt)
                line = line " " nxt
            }
            print line
        }'
}

# Read joined commands from $1 and print one database entry per C source
# operand of each cross-compile command, with $2 as the escaped directory.
# Selection keys on the FIRST word of the command: Homebrew installs libgcc
# under a path containing `i686-elf-gcc`, so matching the whole line would
# select linker lines and continuation fragments through that path.
emit_entries() {
    dump_file=$1
    dir_json=$2
    while IFS= read -r line; do
        set -f
        # shellcheck disable=SC2086 # word-splitting the command is the point
        set -- $line
        set +f
        case "${1-}" in
            *i686-elf-*gcc) ;;
            *) continue ;;
        esac
        cmd_json=$(json_escape "$line")
        for word do
            case "$word" in
                *.c)
                    file_json=$(json_escape "$word")
                    printf '{"directory": "%s", "file": "%s", "command": "%s"}\n' \
                        "$dir_json" "$file_json" "$cmd_json"
                    ;;
            esac
        done
    done <"$dump_file"
}

# Validate the finished database in $1. The generator writes one JSON array
# of one-line objects whose strings contain no raw control character and only
# the four escapes json_escape produces, so validity is a line-by-line check.
# A failure here is a generator defect and the database is not published.
validate_db() {
    LC_ALL=C awk '
        BEGIN {
            s = "(\\\\[\\\\\"trn]|[^\"\\\\[:cntrl:]])*"
            entry = "^\\{\"directory\": \"" s "\", \"file\": \"" s \
                    "\", \"command\": \"" s "\"\\}"
        }
        { line[NR] = $0 }
        $0 ~ /[[:cntrl:]]/ {
            printf "compile_commands: raw control character on line %d\n", NR
            bad = 1
        }
        END {
            if (bad)
                exit 1
            if (NR < 2 || line[1] != "[" || line[NR] != "]") {
                print "compile_commands: not one JSON array"
                exit 1
            }
            for (i = 2; i < NR; i++) {
                want = entry ((i < NR - 1) ? ",$" : "$")
                if (line[i] !~ want) {
                    printf "compile_commands: malformed entry on line %d\n", i
                    exit 1
                }
            }
        }' "$1" >&2
}

# Wrap entry lines from $1 into one JSON array on stdout (comma after all but
# the last; [] if empty).
build_array() {
    echo '['
    awk 'NR > 1 { print prev "," } { prev = $0 } END { if (NR > 0) print prev }' "$1"
    echo ']'
}

selftest() {
    st_dir=$(mktemp -d)
    trap 'rm -rf "$st_dir"' EXIT INT TERM
    st_fail=0

    # Case 1: the wrapped Homebrew recipe. The first command line ends in a
    # backslash; its continuation begins with a tab and carries the libgcc
    # path under .../i686-elf-gcc/..., the exact shape that once selected the
    # fragment as its own command and wrote its raw tab into the database.
    cat >"$st_dir/dump" <<'EOF'
mkdir -p ../build/tests/
/opt/homebrew/bin/i686-elf-gcc -m32 -static -o ../build/tests/dnstest \
	    crt1.o dnstest.c ../utils/resolv.c /opt/homebrew/Cellar/i686-elf-gcc/16.2.0/lib/gcc/i686-elf/16.2.0/libgcc.a -Wl,-z,noexecstack
/opt/homebrew/bin/i686-elf-gcc -m32 -o ../build/tests/auxvprobe auxvprobe.c
EOF
    join_continuations <"$st_dir/dump" >"$st_dir/joined"
    emit_entries "$st_dir/joined" "/fixture" >"$st_dir/entries"
    n=$(wc -l <"$st_dir/entries" | tr -d ' ')
    if [ "$n" != 3 ]; then
        echo "compile_commands: selftest: expected 3 entries, got $n" >&2
        st_fail=1
    fi
    for want in '"file": "dnstest.c"' '"file": "../utils/resolv.c"' \
                '"file": "auxvprobe.c"'; do
        grep -qF "$want" "$st_dir/entries" || {
            echo "compile_commands: selftest: missing entry $want" >&2
            st_fail=1
        }
    done
    if grep -v '"command": "/opt/homebrew/bin/i686-elf-gcc ' \
            "$st_dir/entries" | grep -q .; then
        echo "compile_commands: selftest: an entry's command does not start" \
             "with the compiler (continuation selected as a command?)" >&2
        st_fail=1
    fi
    if [ "$(grep -cF 'noexecstack' "$st_dir/entries")" != 2 ]; then
        echo "compile_commands: selftest: the joined command lost its tail" >&2
        st_fail=1
    fi

    # Case 2: the array built from those entries validates.
    build_array "$st_dir/entries" >"$st_dir/db"
    if ! validate_db "$st_dir/db" 2>>"$st_dir/log"; then
        echo "compile_commands: selftest: a good database failed validation" >&2
        st_fail=1
    fi

    # Case 3: a raw tab inside a string value is rejected.
    printf '[\n{"directory": "/a", "file": "b.c", "command": "x\ty"}\n]\n' \
        >"$st_dir/bad_tab"
    if validate_db "$st_dir/bad_tab" 2>/dev/null; then
        echo "compile_commands: selftest: a raw tab passed validation" >&2
        st_fail=1
    fi

    # Case 4: a malformed entry (an unescaped quote splits the string) is
    # rejected.
    cat >"$st_dir/bad_quote" <<'EOF'
[
{"directory": "/a", "file": "b.c", "command": "x"y"}
]
EOF
    if validate_db "$st_dir/bad_quote" 2>/dev/null; then
        echo "compile_commands: selftest: an unescaped quote passed validation" >&2
        st_fail=1
    fi

    # Case 5: json_escape maps the four characters and nothing else.
    got=$(json_escape "$(printf 'a\tb"c\\d')")
    if [ "$got" != 'a\tb\"c\\d' ]; then
        echo "compile_commands: selftest: json_escape produced '$got'" >&2
        st_fail=1
    fi

    if [ "$st_fail" != 0 ]; then
        echo "compile_commands: selftest: FAIL" >&2
        exit 1
    fi
    echo "compile_commands: selftest: OK (5 cases)"
    exit 0
}

case "${1-}" in
    --selftest) selftest ;;
    '') ;;
    *)
        echo "usage: gen_compile_commands.sh [--selftest]" >&2
        exit 2
        ;;
esac

entries=$(mktemp)
dump_raw=$(mktemp)
dump=$(mktemp)
out_tmp=$(mktemp)
rawlist=$(mktemp)
expected=$(mktemp)
missing=$(mktemp)
trap 'rm -f "$entries" "$dump_raw" "$dump" "$out_tmp" "$rawlist" "$expected" "$missing"' EXIT INT TERM

# In-house, cross-compiled components (host krffs tools + vendored/Doom trees
# excluded).
for comp in rach2 tests ish utils; do
    dir="$REPO/$comp"
    [ -d "$dir" ] || continue
    # Dry-run an always-make build. Capture it to a file (a pipe would lose the
    # exit status), so a toolchain/Make failure is a hard error, not a silent
    # empty DB.
    if ! ( cd "$dir" && make -Bn ) >"$dump_raw" 2>/dev/null; then
        echo "compile_commands: 'make -Bn' failed in $comp/: cannot generate the database" >&2
        echo "       (is the i686-elf cross toolchain installed? try 'make doctor')" >&2
        exit 1
    fi
    join_continuations <"$dump_raw" >"$dump"
    emit_entries "$dump" "$(json_escape "$dir")" >>"$entries"
done

# Build the JSON array into a TEMP file. It is promoted to
# compile_commands.json only after validation and coverage pass, so a failure
# never leaves a bad/partial DB in place.
build_array "$entries" >"$out_tmp"

if ! validate_db "$out_tmp"; then
    echo "compile_commands: validation failed; the database was not published" >&2
    exit 1
fi

# Coverage assertion: every kernel source listed in C_SRC must have made it into
# the database. Capture the list to a file FIRST (a pipe would lose
# print-c-src's exit status and an empty list would vacuously "pass"), then
# require it to have succeeded and be non-empty before checking each source is
# present.
if ! make --no-print-directory -C "$REPO/rach2" print-c-src >"$rawlist" 2>/dev/null; then
    echo "compile_commands: 'make -C rach2 print-c-src' failed: cannot verify kernel coverage" >&2
    exit 1
fi
tr ' ' '\n' <"$rawlist" | sed '/^[[:space:]]*$/d' | sort -u >"$expected"
if [ ! -s "$expected" ]; then
    echo "compile_commands: kernel source list (C_SRC) is empty; is rach2's print-c-src intact?" >&2
    exit 1
fi
while IFS= read -r src; do
    grep -qF "\"file\": \"$src\"" "$out_tmp" || echo "$src" >>"$missing"
done <"$expected"
if [ -s "$missing" ]; then
    echo "compile_commands: kernel sources missing from compile_commands.json (generator bug):" >&2
    sed 's/^/  /' "$missing" >&2
    exit 1
fi

# Validation and coverage passed; promote the temp DB into place (guarded: the
# script does not run under set -e, so an unchecked mv could fall through and
# falsely report success).
if ! mv "$out_tmp" "$OUT"; then
    echo "compile_commands: failed to write $OUT" >&2
    exit 1
fi

n=$(grep -c '"file"' "$OUT" 2>/dev/null || echo 0)
echo "compile_commands.json: $n entries -> $OUT"
