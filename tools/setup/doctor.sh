#!/usr/bin/env sh
#
# tools/setup/doctor.sh: report which tools of Rach2's build, suites and
# checks this machine has. Run it directly.
#
# Reports a required tool as PASS or MISSING and an optional one as PASS or
# OPTIONAL, with an install hint for each absent one, and each judge's version
# as PASS when it is the recorded one, OTHER when it differs and ---- when the
# judge is absent. It exits non-zero if a required tool is absent (or present
# but broken), so a student or a fresh CI machine sees a checklist instead of
# a failure in the middle of a build.
#
# The output is ASCII only: the style check (tools/verify/check.sh) forbids
# other bytes in in-house files, so there are no check marks here.
#
set -u

case "$(uname -s)" in
    Darwin) MAC=1 ;;
    *)      MAC=0 ;;
esac

# hint KEY: an OS-appropriate install suggestion. No Linux package provides
# the i686-elf and x86_64-elf toolchains; tools/Readme.md links the tools of
# the archive's i686-linux-gnu and x86_64-linux-gnu cross toolchains under
# those names and wraps their compilers, and the hints point there.
hint() {
    case "$1" in
    cross)    [ "$MAC" = 1 ] && echo "brew install i686-elf-gcc" \
                             || echo "apt install gcc-i686-linux-gnu, wrapped as i686-elf-gcc as tools/Readme.md shows" ;;
    binutils) [ "$MAC" = 1 ] && echo "brew install i686-elf-binutils" \
                             || echo "apt install binutils-i686-linux-gnu, linked as i686-elf-* as tools/Readme.md shows" ;;
    cross64)  [ "$MAC" = 1 ] && echo "brew install x86_64-elf-gcc" \
                             || echo "x86_64-linux-gnu-gcc (gcc-x86-64-linux-gnu off amd64), wrapped as x86_64-elf-gcc as tools/Readme.md shows" ;;
    binutils64) [ "$MAC" = 1 ] && echo "brew install x86_64-elf-binutils" \
                             || echo "the x86_64-linux-gnu binutils (binutils-x86-64-linux-gnu off amd64), linked as x86_64-elf-* as tools/Readme.md shows" ;;
    qemu)     [ "$MAC" = 1 ] && echo "brew install qemu" || echo "apt install qemu-system-x86" ;;
    ovmf)     [ "$MAC" = 1 ] && echo "brew install qemu, which ships edk2-x86_64-code.fd" \
                             || echo "apt install ovmf" ;;
    pkgconf)  [ "$MAC" = 1 ] && echo "brew install pkg-config" || echo "apt install pkg-config" ;;
    make)     [ "$MAC" = 1 ] && echo "xcode-select --install" || echo "apt install make" ;;
    clang)    [ "$MAC" = 1 ] && echo "xcode-select --install" || echo "apt install clang" ;;
    cc)       [ "$MAC" = 1 ] && echo "xcode-select --install" || echo "apt install build-essential" ;;
    fuse)     [ "$MAC" = 1 ] && echo "brew install --cask fuse-t (or --cask macfuse)" \
                             || echo "apt install libfuse-dev" ;;
    openssh)  [ "$MAC" = 1 ] && echo "preinstalled with macOS (or brew install openssh)" \
                             || echo "apt install openssh-client" ;;
    mtools)   [ "$MAC" = 1 ] && echo "brew install mtools" || echo "apt install mtools" ;;
    e2fsprogs) [ "$MAC" = 1 ] && echo "brew install e2fsprogs" || echo "apt install e2fsprogs" ;;
    mandoc)   [ "$MAC" = 1 ] && echo "brew install mandoc" || echo "apt install mandoc" ;;
    xorriso)  [ "$MAC" = 1 ] && echo "brew install xorriso" || echo "apt install xorriso" ;;
    docker)   [ "$MAC" = 1 ] && echo "brew install --cask docker (Docker Desktop)" \
                             || echo "apt install docker.io" ;;
    *)        [ "$MAC" = 1 ] && echo "brew install $1" || echo "apt install $1" ;;
    esac
}

missing=0

# row STATUS NAME DETAIL: one aligned line.
row() { printf '  %-10s %-18s %s\n' "$1" "$2" "$3"; }

# req NAME HINTKEY: a tool the build or a run needs. It must both exist and
# run (a bare `command -v` would pass a broken Xcode shim that has no real
# Command Line Tools), so it is probed with --version, and its absence or a
# failed probe counts as missing. The probe's input is closed: bzip2 prints
# its version and then compresses its standard input, which would hold up
# this report until the input ended.
req() {
    if command -v "$1" >/dev/null 2>&1 && "$1" --version </dev/null >/dev/null 2>&1; then
        row "[PASS]" "$1" "$(command -v "$1")"
    else
        row "[MISSING]" "$1" "$(hint "$2")"
        missing=$((missing + 1))
    fi
}

# reqx NAME HINTKEY ARG...: the same, for a tool with no --version. NAME is
# run with the given arguments and empty input, and must succeed.
reqx() {
    name=$1
    key=$2
    shift 2
    if command -v "$name" >/dev/null 2>&1 &&
       "$name" "$@" </dev/null >/dev/null 2>&1
    then
        row "[PASS]" "$name" "$(command -v "$name")"
    else
        row "[MISSING]" "$name" "$(hint "$key")"
        missing=$((missing + 1))
    fi
}

# opt NAME WHAT HINTKEY: a tool only some tasks need; absence is a note, not
# a failure, and a plain presence check is enough for one. The tools of
# `tests/run style` are not among them: it does not start without them, and
# every change is checked with it before it is pushed, so calling them
# optional would tell a reader on a fresh machine that a required check can
# be skipped.
opt() {
    if command -v "$1" >/dev/null 2>&1; then
        row "[PASS]" "$1" "$(command -v "$1")"
    else
        row "[OPTIONAL]" "$1" "($2); $(hint "$3")"
    fi
}

echo "Rach2 toolchain check (tools/setup/doctor.sh)"
echo
echo "Required to build and boot both architectures:"
req i686-elf-gcc     cross
req i686-elf-ld      binutils
req i686-elf-strip   binutils
req i686-elf-readelf binutils
req i686-elf-ar      binutils
req i686-elf-ranlib  binutils
req i686-elf-nm      binutils
req qemu-system-i386 qemu
req make             make
req cc               cc
req i686-elf-objcopy binutils
# The x86-64 build (make ARCH=x86_64), which tests/run all builds beside the
# i686 one: its own cross toolchain, the emulator that enters long mode, and
# OVMF, the UEFI firmware the suites that boot by UEFI give it.
req x86_64-elf-gcc     cross64
req x86_64-elf-ld      binutils64
req x86_64-elf-strip   binutils64
req x86_64-elf-readelf binutils64
req x86_64-elf-ar      binutils64
req x86_64-elf-ranlib  binutils64
req x86_64-elf-nm      binutils64
req x86_64-elf-objcopy binutils64
req qemu-system-x86_64 qemu
. "$(dirname "$0")/ovmf_path.sh"
if [ -n "$OVMF_CODE" ]; then
    row "[PASS]" "OVMF" "$OVMF_CODE"
else
    row "[MISSING]" "OVMF" "$(hint ovmf)"
    missing=$((missing + 1))
fi
# The rest of what tools/setup/preflight.sh requires. This report and that
# check answer the same question, so they name the same tools: a machine this
# passes and the build then rejects would make the report worthless.
#
# awk and sed are probed by doing a substitution rather than with --version,
# which the BSD versions macOS ships do not accept. A tool that performs the
# operation is present and working whatever it prints for a version.
reqx awk  awk 'BEGIN { exit 0 }'
reqx sed  sed 's/x/y/'
req tar              tar
req gzip             gzip
req bzip2            bzip2
req patch            patch

# ssh-keygen provisions the demo login keys the os-real image is built with,
# so a default `make` reaches it. It has no --version, so it is probed by
# doing the work the build does with it: generate a key pair in a temporary
# directory. A tool that cannot write a key is as broken here as one that is
# absent. The directory is removed on every path, interruption included.
keygen_dir=$(mktemp -d "${TMPDIR:-/tmp}/rach2-doctor.XXXXXX") || keygen_dir=
trap 'test -n "$keygen_dir" && rm -rf "$keygen_dir"' EXIT INT TERM HUP
if command -v ssh-keygen >/dev/null 2>&1 && [ -n "$keygen_dir" ] &&
   ssh-keygen -t ed25519 -N '' -C rach2-doctor -f "$keygen_dir/probe" \
       >/dev/null 2>&1; then
    row "[PASS]" "ssh-keygen" "$(command -v ssh-keygen)"
else
    row "[MISSING]" "ssh-keygen" "$(hint openssh)"
    missing=$((missing + 1))
fi
rm -rf "$keygen_dir"
keygen_dir=

# e2fsprogs builds and checks the ext2 root image at every build: the three
# programs, found where preflight.sh finds them, and mke2fs probed for the
# -d option the image build needs, which e2fsprogs before 1.43 lacks.
. "$(dirname "$0")/e2fsprogs_path.sh"
for t in mke2fs debugfs e2fsck; do
    # -V rather than --version: debugfs takes an unknown option as none and
    # opens its prompt on standard input, which is a hang under a script.
    if command -v "$t" >/dev/null 2>&1 && "$t" -V >/dev/null 2>&1 </dev/null; then
        row "[PASS]" "$t" "$(command -v "$t")"
    else
        row "[MISSING]" "$t" "$(hint e2fsprogs)"
        missing=$((missing + 1))
    fi
done
if mke2fs 2>&1 | grep -q -- '-d root-directory'; then
    row "[PASS]" "mke2fs -d" "$(mke2fs -V 2>&1 | head -1)"
else
    row "[MISSING]" "mke2fs -d" "e2fsprogs 1.43 or later; $(hint e2fsprogs)"
    missing=$((missing + 1))
fi

# mandoc renders the manual pages the images stage. It has no version
# option, so it is probed by rendering empty input.
reqx mandoc mandoc -T ascii

echo
echo "Required by tests/run style, which does not start without them:"
req cppcheck     cppcheck
req shellcheck   shellcheck
req clang        clang
req clang-format clang-format

echo
# A default `make` reaches none of these. A set of suites, `make test` among
# them, prepares every suite's products and needs mformat, mmd, mcopy and
# xorriso, which `make prepare-tools` prints; the suites that write or read a
# FAT volume on the host need the rest of the mtools commands tests/registry
# names. Each command is named, so that one does not stand for the set.
echo "Required by the suites (tests/registry names each suite's):"
for t in mformat mmd mcopy mdir mtype mdel mdeltree minfo; do
    req "$t" mtools
done
req xorriso xorriso

echo
echo "Optional (only some tasks need these):"
opt docker       "vendor/grub regeneration (maintainer only)" docker
opt git          "cloning and committing" git

# dosfstools checks a FAT32 volume this kernel wrote (tests/run fatwrite runs
# fsck.fat -n when it is there); the suite's own walk and mtools judge the
# volume without it.
if command -v fsck.fat >/dev/null 2>&1; then
    row "[PASS]" "dosfstools" "fsck.fat present"
else
    row "[OPTIONAL]" "dosfstools" "(fsck.fat over tests/run fatwrite); $(hint dosfstools)"
fi

# exfatprogs (Linux) or the system's fsck_exfat (macOS) checks an exFAT
# volume this kernel wrote (tests/run exfat runs it when it is there); the
# suite's own reader judges the volume without it. tests/run large needs
# exfatprogs' mkfs.exfat and fsck.exfat, or macOS's hdiutil and newfs_exfat.
if command -v fsck.exfat >/dev/null 2>&1 || command -v fsck_exfat >/dev/null 2>&1; then
    row "[PASS]" "exfatprogs" "fsck.exfat or fsck_exfat present"
else
    row "[OPTIONAL]" "exfatprogs" "(fsck.exfat over tests/run exfat, and tests/run large); $(hint exfatprogs)"
fi

# FUSE mounts a KRFFS image on the host (make -C krffs mount); nothing in the
# default build or the suites needs it. It is checked through pkg-config,
# which is optional for the same reason.
if ! pkg-config --version >/dev/null 2>&1; then
    row "[OPTIONAL]" "FUSE" "(make -C krffs mount); cannot check without pkg-config, $(hint pkgconf)"
elif pkg-config --exists fuse 2>/dev/null; then
    row "[PASS]" "FUSE" "fuse ($(pkg-config --modversion fuse 2>/dev/null))"
elif pkg-config --exists fuse-t 2>/dev/null; then
    row "[PASS]" "FUSE" "fuse-t ($(pkg-config --modversion fuse-t 2>/dev/null))"
else
    row "[OPTIONAL]" "FUSE" "(make -C krffs mount); $(hint fuse)"
fi

# The judges: each tool's version beside the one tools/setup/judges records
# from the testing host. A different version is reported, not counted
# missing: the tree builds and runs with it, and only the recorded one
# decides a check whose verdict depends on the version. The version line is
# the one the tool's option prints, so the record and the report agree byte
# for byte or not at all.
# version_line TOOL: print the line TOOL's version option prints, in the form
# tools/setup/judges records.
version_line() {
    case "$1" in
    shellcheck) shellcheck --version 2>/dev/null | sed -n 's/^\(version: .*\)$/\1/p' | head -1 ;;
    pmccabe)    pmccabe -V 2>&1 | head -1 ;;
    *)          "$1" --version </dev/null 2>/dev/null | head -1 ;;
    esac
}
echo
echo "The judges, against tools/setup/judges (the testing host's versions):"
sed -n 's/^\([a-z0-9-]*\): \(.*\)$/\1|\2/p' "$(dirname "$0")/judges" | while IFS='|' read -r tool recorded; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        row "[----]" "$tool" "absent here; the testing host judges with $recorded"
        continue
    fi
    found=$(version_line "$tool")
    if [ "$found" = "$recorded" ]; then
        row "[PASS]" "$tool" "$found"
    else
        row "[OTHER]" "$tool" "$found; the record says $recorded"
    fi
done

echo
if [ "$missing" -eq 0 ]; then
    echo "doctor: all required tools present"
    exit 0
fi
echo "doctor: $missing required tool(s) missing; install them (hints above), then re-run 'sh tools/setup/doctor.sh'" >&2
exit 1
