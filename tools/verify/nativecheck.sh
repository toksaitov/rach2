#!/bin/sh
# nativecheck.sh: check the native cases of tests/native against the guest
# programs tests/ builds.
#
# Every program has a case of its own, is staged by a case, or has a line of
# tests/native/machine, and a program with a case has no such line. Each case
# is `case NAME FIXTURE... -- PROGRAM [ARGUMENT...]`, NAME is PROGRAM with an
# optional `.VARIANT`, and no two cases share a NAME; each fixture fits its
# form; the files it stages exist; and the ids, ports and images it names are
# those of tests/native/maps, whose new ids, ports and image names are each
# given once. tests/native/Readme.md gives the format. NATIVECHECK_ROOT, when
# set, is the tree to check in place of this one.
set -eu
REPO=${NATIVECHECK_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
WORK=
# bash 3.2, the Mac's sh, gives an EXIT trap a status of 0 when `set -u`
# stops a script that also has `set -e`, so a run that reached no success
# exit fails here.
finished=
status=0
trap 'status=$?; rm -rf ${WORK:+"$WORK"}; [ -n "$finished" ] || [ "$status" -ne 0 ] || status=1; exit "$status"' 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rach2-nativecheck.XXXXXX")
NATIVE=$REPO/tests/native
# The programs of both architectures, as probecheck.sh lists them.
for arch in i686 x86_64; do
    make -s --no-print-directory -C "$REPO/tests" -f Makefile -f - ARCH=$arch nativecheck-list <<'MK'
.PHONY: nativecheck-list
nativecheck-list:
	@printf '%s\n' $(PROGS)
MK
done | sort -u > "$WORK/built"
for source in "$REPO"/tests/musl/*.c; do
    [ -f "$source" ] || continue
    printf 'musl/%s\n' "$(basename "$source" .c)" >> "$WORK/built"
done
(cd "$NATIVE" && find . -type f | sed 's|^\./|native/|') > "$WORK/files"
# The cases, one a line with continuations joined and comments removed, each
# after its file and line.
for file in probes.cases musl.cases; do
    awk -v file="tests/native/$file" '
        { sub(/(^|[ \t])#.*/, "") }
        joined == "" { at = FNR }
        /\\$/ { sub(/\\$/, ""); joined = joined $0 " "; next }
        { joined = joined $0 }
        joined ~ /[^ \t]/ { print file ":" at, joined }
        { joined = "" }
        END { if (joined != "") print file ":" at, joined }
    ' "$NATIVE/$file"
done > "$WORK/cases"
awk '
    function fail(where, message) { print "nativecheck: " where ": " message > "/dev/stderr"; bad = 1 }
    function list(where, what, value, kind,    n, item, i) {
        n = split(value, item, ",")
        if (value == "" || value ~ /,,|,$/) { fail(where, "an empty item in " what); return }
        for (i = 1; i <= n; i++) {
            if (kind == "id" && !(item[i] in idnew)) fail(where, "an id not in maps: " item[i])
            if (kind == "port" && !(item[i] in portnew)) fail(where, "a port not in maps: " item[i])
            if (kind == "image" && !(item[i] in imagetype)) fail(where, "an image not in maps: " item[i])
            if (kind == "stage") {
                if (item[i] in built) staged[item[i]] = 1
                else if (!(item[i] in files) || item[i] !~ /^native\//) fail(where, "a staged file that is not built or in tests/native: " item[i])
            }
        }
    }
    function once(where, kind) { if (seen[kind]++) fail(where, "two fixtures " kind) }
    function fixture(where, f,    p, n) {
        n = split(f, p, ":")
        if (f ~ /^(mountns|ipcns|utsns|pty|log|alone|console)$/) once(where, f)
        else if (p[1] == "stage" && n == 2) list(where, "stage", p[2], "stage")
        else if (p[1] == "image" && n == 2) list(where, "image", p[2], "image")
        else if (p[1] == "ids" && n == 2) { once(where, "ids"); list(where, "ids", p[2], "id") }
        else if (p[1] == "ports" && n == 2) { once(where, "ports"); list(where, "ports", p[2], "port") }
        else if (p[1] == "volume" && (n == 3 || (n == 4 && p[4] == "attached")) && p[2] ~ /^(krffs|ext2|vfat|exfat)$/) {
            volumes++
            if (!(p[3] in imagetype)) fail(where, "an image not in maps: " p[3])
            else if (imagetype[p[3]] != p[2]) fail(where, "a volume of type " p[2] " on an image of type " imagetype[p[3]])
        }
        else if (f ~ /^tmpfs:[1-9][0-9]*[KM]?$/) once(where, "tmpfs")
        else if (f ~ /^root:(krffs|ext2|ext2-dir)(:[1-9][0-9]*)?$/) { once(where, "root"); volumes++ }
        else if (f ~ /^pty:[^:\/]+$/) {
            once(where, "pty")
            if (!(("native/" p[2]) in files)) fail(where, "a terminal input not in tests/native: " p[2])
        }
        else if (f ~ /^peer:(echo|dgram-outside|network)$/) once(where, "peer")
        else if (f ~ /^env:[A-Za-z_][A-Za-z0-9_]*(=.*)?$/) { }
        else if (f ~ /^end:signal:[1-9][0-9]?$/ && p[3] <= 64) once(where, "end")
        else fail(where, "a fixture that fits no form: " f)
    }
    FILENAME == ARGV[1] { built[$1] = 1; next }
    FILENAME == ARGV[2] { files[$1] = 1; next }
    FILENAME == ARGV[3] {
        if ($0 ~ /^#/ || $0 !~ /[^ \t]/) next
        where = "tests/native/maps:" FNR
        if ($1 == "idmap" || $1 == "portmap") {
            for (i = 2; i <= NF; i++) {
                if ($i !~ /^[0-9]+:[0-9]+$/) { fail(where, "a pair that is not OLD:NEW: " $i); continue }
                split($i, pair, ":")
                if ($1 == "idmap") { if (pair[1] in idnew || newid[pair[2]]++) fail(where, "an id mapped twice: " $i); idnew[pair[1]] = 1 }
                else if (pair[1] in portnew || newport[pair[2]]++ || pair[2] > 65535) fail(where, "a port mapped twice or out of range: " $i)
                else portnew[pair[1]] = 1
            }
        } else if ($1 == "account") {
            accounts++
            if (NF != 3 || $2 !~ /^[A-Za-z0-9_.+-]+$/ || length($2) > 63 ||
                $3 !~ /^[1-9][0-9]*$/ || $3 > 65535)
                fail(where, "an account that is not NAME UID with a safe name and uid 1 to 65535")
            accountuid = $3
        } else if ($1 == "image" && (NF == 4 || (NF == 6 && $5 == "from")) && $3 ~ /^(krffs|ext2|vfat|exfat)$/ && $4 ~ /^[1-9][0-9]*$/) {
            if ($2 in imagetype) fail(where, "an image named twice: " $2)
            imagetype[$2] = $3
        } else fail(where, "a line that fits no form")
        next
    }
    FILENAME == ARGV[4] {
        if ($0 ~ /^#/ || $0 !~ /[^ \t]/) next
        where = "tests/native/machine:" FNR
        if (NF < 2) fail(where, "a program with no reason: " $1)
        else if (!($1 in built)) fail(where, "a program that is not built: " $1)
        else if (machine[$1]++) fail(where, "a program listed twice: " $1)
        next
    }
    {
        where = $1
        for (dash = 3; dash <= NF && $dash != "--"; dash++) { }
        if ($2 != "case" || dash > NF - 1) { fail(where, "not a case NAME FIXTURE... -- PROGRAM line"); next }
        name = $3
        program = $(dash + 1)
        base = name
        sub(/\..*/, "", base)
        if (name !~ /^[a-z0-9\/]+(\.[a-z0-9-]+)?$/ || base != program) fail(where, "a name that is not its program with a variant: " name)
        if (cases[name]++) fail(where, "a case named twice: " name)
        if (!(program in built)) fail(where, "a program that is not built: " program)
        cased[program] = 1
        split("", seen)
        volumes = 0
        for (i = 4; i < dash; i++) fixture(where, $i)
        if (volumes > 4) fail(where, "more volumes and roots than the four loop devices")
    }
    END {
        if (accounts != 1) fail("tests/native/maps", "exactly one account is required")
        if (accountuid in newid) fail("tests/native/maps", "the account uid is also a mapped id")
        for (p in built) {
            if (p in cased && p in machine) fail("tests/native/machine", "a program with a case: " p)
            else if (!(p in cased) && !(p in staged) && !(p in machine)) fail("tests/native", "a program with no case and no reason: " p)
        }
        if (!bad) print "nativecheck: passed: every guest program has a native case or a reason"
        exit bad
    }
' "$WORK/built" "$WORK/files" "$NATIVE/maps" "$NATIVE/machine" "$WORK/cases"
finished=1
