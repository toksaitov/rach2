#!/usr/bin/env sh
#
# tools/setup/doctor.sh: the wider Rach2 machine-setup report, run directly.
#
# Reports each tool as PASS / MISSING / OPTIONAL with an install hint, and exits
# non-zero if a tool in either required group is absent (or present but
# broken), so a student or a fresh CI box sees a clear checklist instead of a
# cryptic failure mid-build.
#
# ASCII-only output: the style check (tools/verify/check.sh) forbids
# bytes in in-house files, so no checkmarks/Unicode here.
#
set -u

case "$(uname -s)" in
    Darwin) MAC=1 ;;
    *)      MAC=0 ;;
esac

# hint KEY: an OS-appropriate install suggestion. The i686-elf cross toolchain
# is NOT a stock apt package on Linux, so its hint is spelled out rather than
# assuming a package manager.
hint() {
    case "$1" in
    cross)    [ "$MAC" = 1 ] && echo "brew install i686-elf-gcc" \
                             || echo "build it from source; tools/Readme.md has the commands" ;;
    binutils) [ "$MAC" = 1 ] && echo "brew install i686-elf-binutils" \
                             || echo "part of the i686-elf toolchain; tools/Readme.md has the commands" ;;
    qemu)     [ "$MAC" = 1 ] && echo "brew install qemu" || echo "apt install qemu-system-x86" ;;
    pkgconf)  [ "$MAC" = 1 ] && echo "brew install pkg-config" || echo "apt install pkg-config" ;;
    make)     [ "$MAC" = 1 ] && echo "xcode-select --install" || echo "apt install make" ;;
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

# req NAME HINTKEY: a tool the build/run needs. It must both EXIST and RUN (a
# bare `command -v` would PASS a broken Xcode shim that has no real Command Line
# Tools), so it is probed with --version. Absence/breakage is a failure.
req() {
    if command -v "$1" >/dev/null 2>&1 && "$1" --version >/dev/null 2>&1; then
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
# a failure, and a plain presence check is enough for one. The three style
# tools are not among them. `tests/run style` refuses to start without all
# three, and CLAUDE.md requires that suite on every change, so calling them
# optional would tell a reader on a fresh machine that the one check the
# project insists on can be skipped.
opt() {
    if command -v "$1" >/dev/null 2>&1; then
        row "[PASS]" "$1" "$(command -v "$1")"
    else
        row "[OPTIONAL]" "$1" "($2); $(hint "$3")"
    fi
}

echo "Rach2 toolchain check (tools/setup/doctor.sh)"
echo
echo "Required (build + run the OS):"
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

echo
echo "Required to verify (tests/run style refuses to run without these):"
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

req mandoc       mandoc
req cppcheck     cppcheck
req shellcheck   shellcheck
req clang-format clang-format

echo
# A default `make` builds the three RAM images and reaches none of these. The
# GRUB disks do, and tools/image/mkdisk_grub.sh needs all four mtools
# commands, so all four are named rather than one standing for the set.
echo "Needed by the GRUB disk suites (grub, menu, install, sealed, drill, selfbuild):"
opt mformat      "the FAT boot partition" mtools
opt mmd          "the FAT boot partition" mtools
opt mcopy        "the FAT boot partition" mtools

echo
echo "Optional (only some tasks need these):"
opt xorriso      "make isos, the bootable ISO per image" xorriso
opt docker       "vendor/grub regeneration (maintainer only)" docker
opt git          "cloning and committing" git

# dosfstools checks a FAT32 volume this kernel wrote (tests/run fatwrite runs
# fsck.fat -n when it is there); the suite's own walk and mtools judge the
# volume without it.
if command -v fsck.fat >/dev/null 2>&1; then
    row "[PASS]" "dosfstools" "fsck.fat present"
else
    row "[----]" "dosfstools" "optional, for fsck.fat over tests/run fatwrite; $(hint dosfstools)"
fi

# exfatprogs (Linux) or the system's fsck_exfat (macOS) checks an exFAT
# volume this kernel wrote (tests/run exfat runs it when it is there); the
# suite's own reader judges the volume without it.
if command -v fsck.exfat >/dev/null 2>&1 || command -v fsck_exfat >/dev/null 2>&1; then
    row "[PASS]" "exfatprogs" "fsck.exfat or fsck_exfat present"
else
    row "[----]" "exfatprogs" "optional, for fsck.exfat over tests/run exfat; $(hint exfatprogs)"
fi

# FUSE mounts a KRFFS image on the host (make -C krffs mount); nothing in the
# default build or the suites needs it. Checked via pkg-config, which is now
# also optional for the same reason.
if ! pkg-config --version >/dev/null 2>&1; then
    row "[----]" "FUSE" "optional; cannot check without pkg-config (pkgconf)"
elif pkg-config --exists fuse 2>/dev/null; then
    row "[PASS]" "FUSE" "fuse ($(pkg-config --modversion fuse 2>/dev/null))"
elif pkg-config --exists fuse-t 2>/dev/null; then
    row "[PASS]" "FUSE" "fuse-t ($(pkg-config --modversion fuse-t 2>/dev/null))"
else
    row "[----]" "FUSE" "optional, for make -C krffs mount; $(hint fuse)"
fi

echo
if [ "$missing" -eq 0 ]; then
    echo "doctor: all required tools present"
    exit 0
fi
echo "doctor: $missing required tool(s) missing; install them (hints above), then re-run 'sh tools/setup/doctor.sh'" >&2
exit 1
