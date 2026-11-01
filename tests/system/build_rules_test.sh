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
#   * grub-disk-rule: `os-real-grub-disk` has a rule that makes
#     the disk, the disk's rule names the kernel, the recovery kernel, the
#     recovery image and the ext2 root, and every prerequisite is either a
#     target of the database or a file of the tree.
#   * disk-tools: the rules of the sealed disk and the GRUB disk
#     name mkmbr, fatchains and grubembed, which they do only when the tools'
#     names are defined before the rules are read.
#   * images-include: the rules mk/images.mk holds have their
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
#   * manifest-parts: every rule whose recipe stages a manifest that names
#     an e2fsprogs program, with the KRFFS stager, the disk stager or
#     mkext2.sh, reaches `check-e2fsck` through its prerequisites, order-only
#     ones included, so that a tree where e2fsprogs is not yet built builds it
#     before the stager reads it.
#   * ish-absolute: the test image's dependency file names the shell by its
#     absolute path, as krffs/mkimage.sh writes it, beside an object newer
#     than the shell. `make -n` of that path, with make's built-in rules,
#     runs the ISH sub-make and does not link the object with the host cc.
#
# The trees hold copies of the real files, and marker directories stand for
# build/, so a clean there cannot reach the real one.
#
# Usage:
#     sh tests/system/build_rules_test.sh
#
set -eu
# The fixture lists i686 and x86-64 sources by naming each architecture, so
# none comes from the environment of the run that starts it, nor from the
# flags of the make that runs a suite, which tests/run gives ARCH.
unset ARCH MAKEFLAGS MFLAGS

[ "$#" -eq 0 ] || { echo 'build-rules-test: usage: build_rules_test.sh' >&2; exit 2; }

REPO=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
. "$REPO/tools/etude/hold.sh"
etude_hold "$REPO" build-rules-test "$REPO/tests/system/build_rules_test.sh" "$@"
# The private trees' makes run outside any admission of this tree, so that on
# an enrolled site the root Makefile reads its rules instead of sending every
# goal to the site's launcher.
ETUDE_SITE=/nonexistent ETUDE_ADMITTED=/nonexistent
export ETUDE_SITE ETUDE_ADMITTED
cd "$REPO"
mkdir -p build/tests
WORK=
trap '[ -z "$WORK" ] || rm -rf "$WORK"' 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
WORK=$(mktemp -d "$REPO/build/tests/build-rules-test.XXXXXX")

# pass ROW: report that ROW passed.
pass()
{
    echo "build-rules-test: result: pass: $1"
}

# fail ROW DETAIL: report that ROW failed and add it to `failed`.
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
# An architecture is a directory with a cpu.h and a sources.mk; the lists
# they include, such as sys/acpi/sources.mk, come with them.
for f in $(find rach2/sys -name sources.mk) rach2/sys/arch/*/cpu.h; do
    mkdir -p "$S/$(dirname "$f")"
    cp "$f" "$S/$f"
done
cp -R mk "$S/mk"
listed=$(MAKEFLAGS='' MFLAGS='' make --no-print-directory -C "$S/rach2" CROSS_CC=true print-c-src) || {
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
    krffs/manifest_deps.sh krffs/programs.sh rach2/Makefile rach2/sources.mk ish/Makefile \
    ish/sources.mk term/sources.mk utils/Makefile utils/programs.mk ports/doom/Makefile tools/etude/sources.mk \
    tests/cases/host/scripts.mk tests/cases/host/family.mk tests/system/fakeapi/native.mk \
    tests/native/native.mk tests/native/sources.mk \
    $(find rach2/sys -name sources.mk); do
    mkdir -p "$C/$(dirname "$f")"
    cp "$f" "$C/$f"
done
cp -R mk "$C/mk"
mkdir -p "$C/krffs/manifests"
cp krffs/manifests/*.manifest "$C/krffs/manifests/"
rm -f "$C/krffs/recovery_authorized_keys"

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

# The three rows of the source-list guard: the tree as it is, a kernel
# source on disk that is not listed, and the last listed source moved away.
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

# Source manifests need no compiler; ordinary and mixed build goals do.
source_query_rows()
{
    for arch in i686 x86_64; do
        for goal in print-c-src print-asm-src; do
            expected=$(MAKEFLAGS='' MFLAGS='' make -s -C "$S/rach2" \
                ARCH="$arch" CROSS_CC=true "$goal")
            rc=0
            out=$(MAKEFLAGS='' MFLAGS='' make -s -C "$S/rach2" \
                ARCH="$arch" CROSS_CC= CC= "$goal" 2>&1) || rc=$?
            if [ "$rc" -eq 0 ] && [ -n "$expected" ] && [ "$out" = "$expected" ]; then
                pass "source-query-$arch-$goal"
            else
                fail "source-query-$arch-$goal" "status $rc: $out"
            fi
        done
        for goals in '' all 'print-c-src all'; do
            rc=0
            # shellcheck disable=SC2086
            out=$(MAKEFLAGS='' MFLAGS='' make -s -C "$S/rach2" \
                ARCH="$arch" CROSS_CC= CC= $goals 2>&1) || rc=$?
            if [ "$rc" -ne 0 ] && printf '%s\n' "$out" | grep -qF 'no cross compiler found'; then
                pass "build-needs-compiler-$arch-${goals:-default}"
            else
                fail "build-needs-compiler-$arch-${goals:-default}" "status $rc: $out"
            fi
        done
    done
    if [ ! -e "$S/build" ]; then
        pass source-queries-write-nothing
    else
        fail source-queries-write-nothing 'a source-list query created build output'
    fi
}

# Plant a marker in each directory under the build tree's build/, run the
# root `make clean` with no cross compiler on the PATH, and require only the
# markers of build/man, build/tools and build/vendor to be left.
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

# The grub-disk-rule row, on the database base.db.
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

# The disk-tools row, on the database base.db.
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

# The images-include row, on the database base.db.
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

# The manifest-parts row, on the database base.db: the rules that stage a
# manifest naming build/vendor/e2fsprogs/ and do not reach check-e2fsck.
manifest_parts_row()
{
    db="$WORK/base.db"
    manifests=$(cd "$C" && grep -l '^[^#]*build/vendor/e2fsprogs/' krffs/manifests/*.manifest)
    if [ -z "$manifests" ]; then
        fail manifest-parts "no manifest names an e2fsprogs program"
        return
    fi
    # shellcheck disable=SC2016 # the stagers, by their unexpanded names
    lone=$(awk -v manifests="$manifests" '
        function reach(t,   n, i, a) {
            if (t == "check-e2fsck") return 1
            if (t in seen) return 0
            seen[t] = 1
            n = split(deps[t], a, " ")
            for (i = 1; i <= n; i++) if (reach(a[i])) return 1
            return 0
        }
        /^$/ { cur = "" }
        cur != "" && /^\t/ { recipe[cur] = recipe[cur] " " $0 }
        prev != "# Not a target:" && /^[^#\t =][^ \t=]*:( |$)/ && !/^[^ ]*::/ {
            cur = substr($0, 1, index($0, ":") - 1)
            rest = substr($0, length(cur) + 2)
            gsub(/\|/, " ", rest)
            deps[cur] = deps[cur] " " rest
        }
        { prev = $0 }
        END {
            nm = split(manifests, m, " ")
            for (t in recipe) {
                r = recipe[t]
                if (!index(r, "$(MKIMAGE)") && !index(r, "$(MKDISK)") && !index(r, "mkext2.sh"))
                    continue
                for (i = 1; i <= nm; i++) {
                    if (!index(r, m[i])) continue
                    split("", seen)
                    if (!reach(t)) print t
                    break
                }
            }
        }' "$db" | sort | tr '\n' ' ')
    if [ -n "$lone" ]; then
        fail manifest-parts "these stage e2fsprogs without check-e2fsck: $lone"
        return
    fi
    # The row is empty if it finds no stager at all, so require the rules
    # this fault would leave without e2fsprogs to have been read.
    for t in build/images/tcc-test.disk build/images/os-real.krffs; do
        is_target "$db" "$t" || { fail manifest-parts "no rule makes $t"; return; }
    done
    pass manifest-parts
}

# The key-names row: the database of the build tree with a recovery key,
# then `make no-recovery-key`. The removal runs for real, in the private
# tree, over files planted in both architectures' image directories: its
# recipe is a loop over them, whose names a dry run prints only through the
# loop's variable.
key_names_row()
{
    : >"$C/krffs/recovery_authorized_keys"
    database keyed.db
    keyed_rc=$rc
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

# The release-guard row: make -n with RELEASE=1, `make release` and a
# development build in the tree with a key, and RELEASE=1 in the tree
# without one.
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

# The ish-absolute row. MAKE is `true`, so the sub-make's line runs under -n
# and builds nothing, and the planted files are removed after.
ish_absolute_row()
{
    top=$(cd "$C" && pwd -P)
    shell=$top/build/ish/ish
    mkdir -p "$C/build/ish" "$C/build/images"
    : >"$shell"
    touch -t 200001010000 "$shell"
    : >"$shell.o"
    printf '%s: %s\n%s:\n' "$top/build/images/test.krffs" "$shell" "$shell" \
        >"$C/build/images/test.krffs.d"
    rc=0
    (cd "$C" && MAKEFLAGS='' MFLAGS='' make --no-print-directory -n MAKE=true "$shell") \
        >"$WORK/ish.absolute" 2>&1 || rc=$?
    rm -rf "$C/build"
    if [ "$rc" -ne 0 ]; then
        fail ish-absolute "make -n $shell: status $rc: $(cat "$WORK/ish.absolute")"
    elif grep -qF -- "-o $shell" "$WORK/ish.absolute"; then
        fail ish-absolute "make links $shell itself: $(cat "$WORK/ish.absolute")"
    elif ! grep -qF -- '-C ish' "$WORK/ish.absolute"; then
        fail ish-absolute "make does not run the ISH sub-make for $shell"
    else
        pass ish-absolute
    fi
}

# Run every row; the names of those that failed are left in `failed`.
rows()
{
    failed=
    sources_rows
    source_query_rows
    clean_row
    database base.db
    if [ "$rc" -ne 0 ]; then
        fail grub-disk-rule "the build tree's make database: status $rc"
        fail disk-tools "the build tree's make database: status $rc"
        fail images-include "the build tree's make database: status $rc"
        fail manifest-parts "the build tree's make database: status $rc"
    else
        grub_disk_row
        disk_tools_row
        images_include_row
        manifest_parts_row
    fi
    key_names_row
    release_row
    ish_absolute_row
}

rows
[ -z "$failed" ] || exit 1
