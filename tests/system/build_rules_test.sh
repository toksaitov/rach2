#!/usr/bin/env sh
#
# build_rules_test.sh: the build's rules, read in private trees with nothing
# built.
#
# The source-list guard, tools/verify/check_sources.sh, must pass a tree whose
# disk and rach2/sources.mk agree, and must name a kernel source that is on
# disk and not listed, and one that is listed and not on disk. The root `make
# clean` must succeed with no cross compiler on the PATH, since
# mk/toolchain.mk excuses a clean from its check, and must remove what each
# sub-make owns while build/tools, build/man and build/vendor stay.
#
# The rest are read from the make database, `make -pn`, of a private copy of
# the root Makefile, mk/ and the suites' fragment, which parses the rules and
# runs none of them:
#
#   * grub-disk-rule (entry 112): `os-real-grub-disk` has a rule that makes
#     the disk, the disk's rule names the kernel, the recovery kernel, the
#     recovery image and the ext2 root, and every prerequisite is either a
#     target of the database or a file of the tree.
#   * disk-tools (entry 114): the rules of the sealed disk and the GRUB disk
#     name mkmbr, fatchains and grubembed, which they do only when the tools'
#     names are defined before the rules are read.
#   * images-include (entry 114): the rules mk/images.mk holds have their
#     names, which they do only when the fragment is included after the root
#     Makefile has named the recovery ISO, the sealed disk and the tools: the
#     ISO's rule names the recovery kernel and image, the install stick's
#     names the ISO, the sealed disk and its root, and each disk tool has a
#     rule.
#   * key-names: in a tree with a recovery key the sealed disk and the GRUB
#     disk are made under `-with-key` names, as the recovery image, the ISO
#     and the stick are, and not under the plain ones; and `make
#     no-recovery-key` removes both names of both disks in both
#     architectures' image directories.
#   * release-guard: in a tree with a recovery key, `make release` and a
#     build with RELEASE=1 are refused before anything is built, while a
#     development build is not; in a tree without one RELEASE=1 is not
#     refused.
#
# The trees hold copies of the real files, and marker directories stand for
# build/, so a clean there cannot reach the real one. --mutate puts each fault
# into the private copy that holds it, requires the row named for it to fail,
# then restores and touches the copy and requires every row again. --texts
# puts each fault into its copy and restores it, and runs no row, so that a
# fault whose text has left its file is found without the rows.
#
# Usage:
#     sh tests/system/build_rules_test.sh [--mutate | --texts]
#
set -eu
# The fixture lists i686 and x86-64 sources by naming each architecture, so
# none comes from the environment of the run that starts it, nor from the
# flags of the make that runs a suite, which tests/run gives ARCH.
unset ARCH MAKEFLAGS MFLAGS

case ${1:-} in
    '') mutate=0 ;;
    --mutate) mutate=1 ;;
    --texts) mutate=2 ;;
    *) echo 'build-rules-test: usage: build_rules_test.sh [--mutate | --texts]' >&2; exit 2 ;;
esac
[ "$#" -le 1 ] || exit 2

REPO=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
if [ "${1:-}" != --texts ]; then
    . "$REPO/tests/system/runlock.sh"
    runlock_hold "$REPO" build-rules-test "$REPO/tests/system/build_rules_test.sh" "$@"
fi
cd "$REPO"
mkdir -p build/tests
WORK=$(mktemp -d "$REPO/build/tests/build-rules-test.XXXXXX")
trap 'rm -rf "$WORK"' 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP

pass()
{
    echo "build-rules-test: result: pass: $1"
}

fail()
{
    echo "build-rules-test: result: fail: $1: $2"
    failed="$failed $1"
}

# The source-list tree: the guard, the kernel makefile with its list and the
# shared make fragments, and one empty file for each listed source, so the
# tree's disk and its list agree before a row changes either.
S="$WORK/sources"
mkdir -p "$S/tools/verify" "$S/rach2"
cp tools/verify/check_sources.sh "$S/tools/verify/"
cp rach2/Makefile rach2/sources.mk "$S/rach2/"
# An architecture is a directory with a cpu.h and a sources.mk.
for f in rach2/sys/arch/*/sources.mk rach2/sys/arch/*/cpu.h; do
    mkdir -p "$S/$(dirname "$f")"
    cp "$f" "$S/$f"
done
cp -R mk "$S/mk"
listed=$(MAKEFLAGS='' MFLAGS='' make --no-print-directory -C "$S/rach2" print-c-src) || {
    echo 'build-rules-test: the kernel makefile would not print its sources' >&2
    exit 1
}
# The guard reads every architecture's list, so each has its files.
listed64=$(MAKEFLAGS='' MFLAGS='' make --no-print-directory -C "$S/rach2" ARCH=x86_64 CROSS_CC=true \
    print-c-src) || {
    echo 'build-rules-test: the kernel makefile would not print the x86-64 sources' >&2
    exit 1
}
for f in $listed $listed64; do
    mkdir -p "$S/rach2/$(dirname "$f")"
    : >"$S/rach2/$f"
done
last=${listed##* }

# The build tree: the makefiles the root `make clean` reads, and the files its
# parse runs. The database rows read the same tree.
C="$WORK/clean"
for f in Makefile tests/registry tests/Makefile tests/system/suites.mk krffs/Makefile \
    krffs/manifest_deps.sh rach2/Makefile rach2/sources.mk ish/Makefile ish/sources.mk \
    utils/Makefile utils/programs.mk ports/doom/Makefile rach2/sys/arch/*/sources.mk; do
    mkdir -p "$C/$(dirname "$f")"
    cp "$f" "$C/$f"
done
cp -R mk "$C/mk"
mkdir -p "$C/krffs/manifests"
cp krffs/manifests/*.manifest "$C/krffs/manifests/"
rm -f "$C/krffs/recovery_authorized_keys"
cp "$S/tools/verify/check_sources.sh" "$WORK/check_sources.saved"
cp "$C/mk/toolchain.mk" "$WORK/toolchain.saved"
cp "$C/Makefile" "$WORK/Makefile.saved"
cp "$C/mk/images.mk" "$WORK/images.saved"
cp "$C/mk/arch.mk" "$WORK/arch.saved"

# The PATH with each directory that holds a cross compiler replaced by one of
# links to everything else in it: make, the shell and the utilities are still
# found, and neither compiler is.
n=0
nocross=
old_ifs=$IFS
IFS=:
for d in $PATH; do
    if [ -n "$d" ] && { [ -e "$d/i686-elf-gcc" ] || [ -e "$d/x86_64-elf-gcc" ]; }; then
        n=$((n + 1))
        mkdir -p "$WORK/path/$n"
        for x in "$d"/*; do
            case ${x##*/} in
                i686-elf-* | x86_64-elf-*) ;;
                *) ln -s "$x" "$WORK/path/$n/${x##*/}" ;;
            esac
        done
        d="$WORK/path/$n"
    fi
    nocross=${nocross:+$nocross:}$d
done
IFS=$old_ifs
if (PATH=$nocross command -v i686-elf-gcc || PATH=$nocross command -v x86_64-elf-gcc) \
    >/dev/null 2>&1; then
    echo 'build-rules-test: the PATH without cross compilers still finds one' >&2
    exit 1
fi

# The private guard, its output in $out and its status in $rc.
guard()
{
    rc=0
    out=$(MAKEFLAGS='' MFLAGS='' sh "$S/tools/verify/check_sources.sh" 2>&1) || rc=$?
}

sources_rows()
{
    guard
    if [ "$rc" -eq 0 ]; then
        pass sources-agree
    else
        fail sources-agree "status $rc: $out"
    fi

    : >"$S/rach2/sys/zzz_unlisted.c"
    guard
    rm -f "$S/rach2/sys/zzz_unlisted.c"
    case $rc:$out in
        1:*'MISSING from C_SRC'*'  sys/zzz_unlisted.c'*) pass sources-unlisted ;;
        *) fail sources-unlisted "status $rc: $out" ;;
    esac

    mv "$S/rach2/$last" "$WORK/moved.c"
    guard
    mv "$WORK/moved.c" "$S/rach2/$last"
    case $rc:$out in
        1:*'do NOT exist on disk'*"  $last"*) pass sources-missing ;;
        *) fail sources-missing "status $rc: $out" ;;
    esac
}

clean_row()
{
    for d in krffs rach2 tests ish utils ports/doom images iso tools man vendor; do
        mkdir -p "$C/build/$d"
        : >"$C/build/$d/marker"
    done
    rc=0
    out=$( (unset CROSS_CC
        PATH=$nocross
        export PATH
        MAKEFLAGS='' MFLAGS='' make --no-print-directory -C "$C" clean) 2>&1) || rc=$?
    left=$(cd "$C/build" && find . -name marker | sort | tr '\n' ' ')
    if [ "$rc" -eq 0 ] && [ "$left" = './man/marker ./tools/marker ./vendor/marker ' ]; then
        pass clean-without-cross-compiler
    else
        fail clean-without-cross-compiler "status $rc, markers left: $left: $out"
    fi
    rm -rf "$C/build"
}

# The build tree's make, with no parent make's flags, its status in $rc and
# its output, both streams, in $WORK/$1.
build_make() { # build_make NAME ARGUMENTS...
    name=$1
    shift
    rc=0
    (cd "$C" && MAKEFLAGS='' MFLAGS='' make --no-print-directory -r "$@") \
        >"$WORK/$name" 2>&1 || rc=$?
}

# The make database of the build tree, which parses every rule and runs
# none: `print-host-tools` is a goal whose one recipe line only echoes, and
# -n prints it instead.
database() { # database NAME
    build_make "$1" -pn print-host-tools
}

# The prerequisites of TARGET's rule in the database DB, one a line, those
# after `|` left out; nothing, and status 1, when TARGET has no rule. A name
# the database lists under `# Not a target:` has none.
prereqs() { # prereqs DB TARGET
    awk -v t="$2:" '
        prev != "# Not a target:" && index($0, t) == 1 {
            rest = substr($0, length(t) + 1)
            sub(/\|.*/, "", rest)
            n = split(rest, a, " ")
            for (i = 1; i <= n; i++) print a[i]
            found = 1
            exit
        }
        { prev = $0 }
        END { exit !found }' "$1"
}

# Whether NAME has a rule of its own in the database DB.
is_target() { # is_target DB NAME
    prereqs "$1" "$2" >/dev/null
}

# Whether the word list LIST has the word WORD.
has() { # has LIST WORD
    case " $(echo "$1" | tr '\n' ' ') " in
        *" $2 "*) return 0 ;;
        *) return 1 ;;
    esac
}

grub_disk_row()
{
    db="$WORK/base.db"
    disk=build/images/os-real-grub.disk
    if ! is_target "$db" os-real-grub-disk; then
        fail grub-disk-rule "no rule makes os-real-grub-disk"
        return
    fi
    # The phony target's recipe asks a make of the disk's own file.
    if ! awk '
        index($0, "os-real-grub-disk:") == 1 && prev != "# Not a target:" { on = 1; next }
        on && /^$/ { exit }
        on && /^\t/ && index($0, "$(OS_REAL_GRUB_DISK)") { found = 1; exit }
        { prev = $0 }
        END { exit !found }' "$db"; then
        fail grub-disk-rule "os-real-grub-disk does not make \$(OS_REAL_GRUB_DISK)"
        return
    fi
    if ! need=$(prereqs "$db" "$disk"); then
        fail grub-disk-rule "no rule makes $disk"
        return
    fi
    for p in build/rach2/kernel build/rach2-recovery/kernel build/images/rach2-recovery.krffs \
        build/images/os-real.ext2; do
        has "$need" "$p" || { fail grub-disk-rule "$disk does not name $p"; return; }
    done
    for p in $need; do
        if ! is_target "$db" "$p" && [ ! -e "$REPO/$p" ]; then
            fail grub-disk-rule "$disk names $p, which no rule makes and the tree has not"
            return
        fi
    done
    pass grub-disk-rule
}

disk_tools_row()
{
    db="$WORK/base.db"
    for disk in build/images/rach2-sealed.disk build/images/os-real-grub.disk; do
        if ! need=$(prereqs "$db" "$disk"); then
            fail disk-tools "no rule makes $disk"
            return
        fi
        for t in build/tools/image/mkmbr build/tools/image/fatchains build/tools/image/grubembed; do
            has "$need" "$t" || { fail disk-tools "$disk does not name $t"; return; }
        done
    done
    pass disk-tools
}

images_include_row()
{
    db="$WORK/base.db"
    iso=build/images/rach2-recovery.iso
    stick=build/images/rach2-install-stick.img
    if ! need=$(prereqs "$db" "$iso"); then
        fail images-include "no rule makes $iso"
        return
    fi
    for p in build/rach2-recovery/kernel build/images/rach2-recovery.krffs; do
        has "$need" "$p" || { fail images-include "$iso does not name $p"; return; }
    done
    if ! need=$(prereqs "$db" "$stick"); then
        fail images-include "no rule makes $stick"
        return
    fi
    for p in "$iso" build/images/rach2-sealed.disk build/images/rach2-metal-disk.ext2; do
        has "$need" "$p" || { fail images-include "$stick does not name $p"; return; }
    done
    for t in mkmbr fatchains grubembed stickcheck; do
        is_target "$db" "build/tools/image/$t" ||
            { fail images-include "no rule makes build/tools/image/$t"; return; }
    done
    pass images-include
}

key_names_row()
{
    : >"$C/krffs/recovery_authorized_keys"
    database keyed.db
    keyed_rc=$rc
    # The removal runs for real, in the private tree, over files planted in
    # both architectures' image directories: its recipe is a loop over them,
    # whose names a dry run prints only through the loop's variable.
    for d in build/images build/x86_64/images; do
        mkdir -p "$C/$d"
        for p in rach2-sealed.disk rach2-sealed-with-key.disk os-real-grub.disk \
            os-real-grub-with-key.disk; do
            : >"$C/$d/$p"
        done
    done
    build_make keyed.remove no-recovery-key
    remove_rc=$rc
    rm -f "$C/krffs/recovery_authorized_keys"
    db="$WORK/keyed.db"
    if [ "$keyed_rc" -ne 0 ]; then
        fail key-names "the keyed tree's make database: status $keyed_rc"
        return
    fi
    for p in build/images/rach2-recovery-with-key.krffs build/images/rach2-recovery-with-key.iso \
        build/images/rach2-install-stick-with-key.img build/images/rach2-sealed-with-key.disk \
        build/images/os-real-grub-with-key.disk; do
        is_target "$db" "$p" || { fail key-names "a tree with a key does not make $p"; return; }
    done
    for p in build/images/rach2-sealed.disk build/images/os-real-grub.disk; do
        if is_target "$db" "$p"; then
            fail key-names "a tree with a key makes $p under its plain name"
            return
        fi
    done
    need=$(prereqs "$db" build/images/rach2-install-stick-with-key.img) || need=
    has "$need" build/images/rach2-sealed-with-key.disk ||
        { fail key-names "the keyed stick does not carry the keyed sealed disk"; return; }
    [ "$remove_rc" -eq 0 ] || { fail key-names "make no-recovery-key: status $remove_rc"; return; }
    for d in build/images build/x86_64/images; do
        for p in rach2-sealed.disk rach2-sealed-with-key.disk os-real-grub.disk \
            os-real-grub-with-key.disk; do
            [ ! -e "$C/$d/$p" ] ||
                { fail key-names "make no-recovery-key does not remove $d/$p"; return; }
        done
    done
    pass key-names
}

release_row()
{
    : >"$C/krffs/recovery_authorized_keys"
    build_make release.keyed -n RELEASE=1 print-host-tools
    keyed_rc=$rc
    if [ "$keyed_rc" -ne 0 ] && grep -q 'release: refused' "$WORK/release.keyed"; then
        build_make release.goal -n release
        goal_rc=$rc
    else
        goal_rc=0
    fi
    build_make release.development -n print-host-tools
    development_rc=$rc
    rm -f "$C/krffs/recovery_authorized_keys"
    build_make release.keyless -n RELEASE=1 print-host-tools
    keyless_rc=$rc
    if [ "$keyed_rc" -eq 0 ] || ! grep -q 'release: refused' "$WORK/release.keyed"; then
        fail release-guard "RELEASE=1 was not refused in a tree with a key: status $keyed_rc"
    elif [ "$goal_rc" -eq 0 ] || ! grep -q 'release: refused' "$WORK/release.goal"; then
        fail release-guard "make release was not refused in a tree with a key: status $goal_rc"
    elif [ "$development_rc" -ne 0 ]; then
        fail release-guard "a development build was refused in a tree with a key: status $development_rc"
    elif [ "$keyless_rc" -ne 0 ]; then
        fail release-guard "RELEASE=1 was refused in a tree with no key: status $keyless_rc"
    else
        pass release-guard
    fi
}

rows()
{
    failed=
    sources_rows
    clean_row
    database base.db
    if [ "$rc" -ne 0 ]; then
        fail grub-disk-rule "the build tree's make database: status $rc"
        fail disk-tools "the build tree's make database: status $rc"
        fail images-include "the build tree's make database: status $rc"
    else
        grub_disk_row
        disk_tools_row
        images_include_row
    fi
    key_names_row
    release_row
}

# Change one private copy where the old text occurs exactly once.
mutate()
{
    python3 - "$1" "$2" "$3" <<'PY'
import pathlib
import sys

path, old, new = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(old) != 1:
    raise SystemExit("build-rules-test: pattern is not unique: " + old)
path.write_text(text.replace(old, new))
PY
    touch "$1"
}

# Move the lines of the private Makefile from the one that begins FIRST to
# the one that begins LAST: to the top of the private mk/images.mk with
# `images`, or to just after the line AFTER with `after`.
move_lines()
{
    python3 - "$C/Makefile" "$C/mk/images.mk" "$1" "$2" "$3" "${4:-}" <<'PY'
import pathlib
import sys

makefile, images = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
first, last, where, after = sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6]
lines = makefile.read_text().splitlines(keepends=True)
starts = [i for i, l in enumerate(lines) if l.startswith(first)]
ends = [i for i, l in enumerate(lines) if l.startswith(last)]
if len(starts) != 1 or len(ends) != 1 or ends[0] < starts[0]:
    raise SystemExit("build-rules-test: the lines to move are not unique: " + first)
block = lines[starts[0]:ends[0] + 1]
del lines[starts[0]:ends[0] + 1]
if where == "images":
    images.write_text("".join(block) + images.read_text())
else:
    at = [i for i, l in enumerate(lines) if l.startswith(after)]
    if len(at) != 1:
        raise SystemExit("build-rules-test: the line to move after is not unique: " + after)
    lines[at[0] + 1:at[0] + 1] = block
makefile.write_text("".join(lines))
PY
    touch "$C/Makefile" "$C/mk/images.mk"
}

if [ "$mutate" -ne 2 ]; then
    rows
    [ -z "$failed" ] || exit 1
    [ "$mutate" -eq 1 ] || exit 0
fi

# Each fault, the copy it changes and the row it must fail. The make faults
# are the states the entries were found in: no rule for the GRUB disk (112),
# the tools' names defined in mk/images.mk after the disk rules that name
# them, and the fragment included before the names its own rules take (114).
# shellcheck disable=SC2016
for fault in unlisted missing clean grub-target tools-late include-early plain-sealed one-arch \
    release; do
    case $fault in
        unlisted)
            copies="$S/tools/verify/check_sources.sh=$WORK/check_sources.saved"
            mutate "$S/tools/verify/check_sources.sh" 'unlisted=$(comm -13 "$lf" "$df")' 'unlisted='
            row='sources-unlisted' ;;
        missing)
            copies="$S/tools/verify/check_sources.sh=$WORK/check_sources.saved"
            mutate "$S/tools/verify/check_sources.sh" 'missing=$(comm -23 "$lf" "$df")' 'missing='
            row='sources-missing' ;;
        clean)
            copies="$C/mk/toolchain.mk=$WORK/toolchain.saved"
            mutate "$C/mk/toolchain.mk" 'ifneq ($(filter-out clean,$(or $(MAKECMDGOALS),build)),)' \
                'ifneq ($(or $(MAKECMDGOALS),build),)'
            row='clean-without-cross-compiler' ;;
        grub-target)
            copies="$C/Makefile=$WORK/Makefile.saved"
            mutate "$C/Makefile" 'os-real-grub-disk: krffs' 'os-real-grub-disk-gone: krffs'
            row='grub-disk-rule' ;;
        tools-late)
            copies="$C/Makefile=$WORK/Makefile.saved $C/mk/images.mk=$WORK/images.saved"
            move_lines 'MKMBR           =' 'MKEXFATFIXTURE  =' images
            row='disk-tools' ;;
        include-early)
            copies="$C/Makefile=$WORK/Makefile.saved"
            move_lines 'include mk/images.mk' 'include mk/images.mk' after 'os-real-ext2: $(OS_REAL_EXT2)'
            row='images-include' ;;
        plain-sealed)
            copies="$C/Makefile=$WORK/Makefile.saved"
            mutate "$C/Makefile" 'SEALED_DISK = $(IMAGES_DIR)/rach2-sealed$(RECOVERY_KEY_TAG).disk' \
                'SEALED_DISK = $(IMAGES_DIR)/rach2-sealed.disk'
            row='key-names' ;;
        one-arch)
            copies="$C/mk/arch.mk=$WORK/arch.saved"
            mutate "$C/mk/arch.mk" 'ARCH_BUILDS = build build/x86_64' 'ARCH_BUILDS = build'
            row='key-names' ;;
        release)
            copies="$C/Makefile=$WORK/Makefile.saved"
            mutate "$C/Makefile" '$(error release: refused:' '$(info release: allowed:'
            row='release-guard' ;;
    esac
    if [ "$mutate" -eq 2 ]; then
        for pair in $copies; do
            cp "${pair#*=}" "${pair%%=*}"
        done
        continue
    fi
    rows
    case " $failed " in
        *" $row "*) echo "build-rules-test: result: $fault: the fault fails $row" ;;
        *) echo "build-rules-test: result: fail: the $fault fault passed $row" >&2; exit 1 ;;
    esac
    for pair in $copies; do
        cp "${pair#*=}" "${pair%%=*}"
        touch "${pair%%=*}"
    done
    rows
    [ -z "$failed" ] || exit 1
    echo "build-rules-test: result: $fault: restored, every row passes"
done
[ "$mutate" -ne 2 ] || echo 'build-rules-test: texts: every fault is once in its source'
