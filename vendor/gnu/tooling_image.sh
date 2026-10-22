#!/usr/bin/env sh
#
# tooling_image.sh: make a root image of the GNU layer in etude's tooling
# machine, on a host that cannot build the layer itself.
#
# Usage: tooling_image.sh OUTPUT SIZE
#
# mk/gnu-image.mk runs it on such a host, where an x86-64 Linux host runs
# tools/image/mkext2.sh, with RACH2_ARCH, GNU_AGENTS and GNU_LINK set as for
# that rule, RACH2_PATH_MAP as the Makefile exports it, and MUSL_CRYPT_SRC,
# the file of the extracted musl that the KRFFS tools compile. It stages the
# tree as one tar archive: the files git tracks or would track, the products
# of this host that krffs/manifests/os-real-gnu.manifest names, and
# MUSL_CRYPT_SRC. It then runs tooling_job.sh in the tooling machine
# (tools/etude/Readme.md, The tooling machine), made first if it is not, with
# a network and two raw disks: the layer's build disk, tooling.disk in the
# layer's build tree (build/vendor/gnu or build/x86_64/vendor/gnu), and a new
# sparse file of SIZE MiB that the job writes the image to. When the job
# succeeds and the file's SHA-256 is the one the job computed in the machine,
# the file is renamed OUTPUT.
#
# The build disk is a sparse file that keeps the layer's build tree, its
# downloads and the job's logs between jobs, so a job that stops or fails
# loses no more than the step it was in, and the next one goes on from the
# stamps the disk has; the machine trims it, so the file holds the blocks the
# volume uses. One job at a time uses it: a second waits, by a lock beside
# the disk, until the first ends. While the job runs, each line the machine's
# make writes is printed here as it comes, and at its end etude-tooling's
# account of the boot, its `etude-tooling: cost:` line.
#
# SIGHUP, SIGINT or SIGTERM, whether it reaches the script's process group or
# the script alone, stops the job: etude-tooling is given SIGTERM, and once
# it has ended the script removes its stage directory beside the disk and the
# lock, and exits with 128 plus the signal's number. A script killed outright
# leaves both, and the next one removes them.
#
# The environment sets the machine's shape:
#
#   GNU_TOOLING_CPUS     processors, and make's jobs in the machine; 8
#   GNU_TOOLING_MIB      memory in MiB; 12288
#   GNU_TOOLING_DISK_GIB the build disk's size in GiB, 80; a disk smaller than
#                        this is grown, and its volume with it
#   GNU_TOOLING_SECONDS  the job's deadline, after which the machine is
#                        stopped; 345600, four days
#   GNU_TOOLING_DRY_RUN  when not empty, the machine runs make with -n, which
#                        prints what the layer's build would do, and makes no
#                        image; OUTPUT is not written
#   RACH2_QEMU_ACCEL     QEMU's accelerator: hvf on an Intel Mac, tcg elsewhere
#   ETUDE_TOOLING_CACHE  the machine's cache, build/tooling
#

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
. "$DIR/arch.sh"

OUT="${1:-}"
SIZE="${2:-}"
case "$SIZE" in
    '' | *[!0-9]*)
        echo "usage: tooling_image.sh OUTPUT SIZE" >&2
        exit 2
        ;;
esac
CPUS="${GNU_TOOLING_CPUS:-8}"
MIB="${GNU_TOOLING_MIB:-12288}"
DISK_GIB="${GNU_TOOLING_DISK_GIB:-80}"
DEADLINE="${GNU_TOOLING_SECONDS:-345600}"
DRY="${GNU_TOOLING_DRY_RUN:-}"
CACHE="${ETUDE_TOOLING_CACHE:-$REPO/build/tooling}"
TOOLING="$REPO/build/tools/etude/etude-tooling"
SHA="$REPO/build/tools/verify/sha256"
DISK="$GNU_OUT/tooling.disk"
LOCK="$DISK.lock"
if [ -z "${RACH2_QEMU_ACCEL:-}" ]; then
    RACH2_QEMU_ACCEL=tcg
    if [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = x86_64 ] &&
        [ "$(sysctl -n kern.hv_support 2>/dev/null)" = 1 ]; then
        RACH2_QEMU_ACCEL=hvf
    fi
fi
if [ -z "${MUSL_CRYPT_SRC:-}" ]; then
    echo "gnu: tooling: MUSL_CRYPT_SRC is not set; make sets it" >&2
    exit 2
fi
for t in "$TOOLING" "$SHA" "$REPO/$MUSL_CRYPT_SRC"; do
    [ -f "$t" ] || { echo "gnu: tooling: $t is missing; run make first" >&2; exit 1; }
done

cd "$REPO"
mkdir -p "$GNU_OUT" "$(dirname "$OUT")"
STAGE=
FOLLOWER=
JOB=
LOCKED=

# cleanup: stop the follower and the job, then remove the stage and the lock.
# A signal that reached this script alone, as make sends one, is passed on to
# etude-tooling, and the lock is kept until etude-tooling has ended, since
# its machine writes the build disk until then. etude-tooling removes its
# job's directory when a signal stops it, but not when it is killed
# outright, so the directory of a job that had not ended is removed here.
# Each step is taken even when one before it failed.
cleanup()
{
    set +e
    [ -z "$FOLLOWER" ] || kill "$FOLLOWER" 2>/dev/null
    if [ -n "$JOB" ]; then
        kill "$JOB" 2>/dev/null
        wait "$JOB"
        rm -rf "$CACHE/job-$JOB-"*
    fi
    [ -z "$STAGE" ] || rm -rf "$STAGE"
    [ -z "$LOCKED" ] || rm -f "$LOCK"
}

# stop STATUS: exit with STATUS for a signal, and ignore any later one, which
# would otherwise end cleanup part of the way through.
stop()
{
    trap '' HUP INT TERM
    exit "$1"
}
trap cleanup EXIT
trap 'stop 129' HUP
trap 'stop 130' INT
trap 'stop 143' TERM

# tooling ARG...: run etude-tooling with the tree's recipe and cache.
tooling()
{
    "$TOOLING" -r "$REPO/tools/etude/tooling" -C "$CACHE" -a "$RACH2_QEMU_ACCEL" "$@"
}

# take_lock: wait until no other job uses the build disk, and take it. The
# lock is a symbolic link whose target is the holder's process ID, which a
# holder that is gone leaves for the next to remove.
take_lock()
{
    said=
    until ln -s "$$" "$LOCK" 2>/dev/null; do
        holder=$(readlink "$LOCK" 2>/dev/null) || continue
        if ! kill -0 "$holder" 2>/dev/null; then
            rm -f "$LOCK"
            continue
        fi
        if [ -z "$said" ]; then
            echo "gnu: tooling: waiting for $DISK, which process $holder uses" >&2
            said=1
        fi
        sleep 30
    done
    LOCKED=1
}

# sweep: remove the stages that jobs killed outright left, with the job
# directories they name whose etude-tooling has gone. Only the lock's holder
# has a stage, so once the lock is taken every stage here is such a one.
sweep()
{
    for old in "$GNU_OUT"/.tooling.*; do
        [ -d "$old" ] || continue
        if [ -s "$old/pid" ]; then
            pid=$(cat "$old/pid")
            kill -0 "$pid" 2>/dev/null || rm -rf "$CACHE/job-$pid-"*
        fi
        rm -rf "$old"
    done
}

# stage_tree: write STAGE/tree.tar.gz, the files git tracks or would track
# with the host's products the manifest names, each with its mode and time.
stage_tree()
{
    if ! git rev-parse --git-dir > /dev/null 2>&1; then
        echo "gnu: tooling: the tree is not a git checkout, whose files are the ones sent" >&2
        exit 1
    fi
    git ls-files -z -c -o --exclude-standard | tr '\0' '\n' > "$STAGE/git"
    sh krffs/manifest_deps.sh krffs/manifests/os-real-gnu.manifest > "$STAGE/products" || :
    echo "$MUSL_CRYPT_SRC" >> "$STAGE/products"
    LC_ALL=C sort -u "$STAGE/git" "$STAGE/products" | while IFS= read -r f; do
        if [ -f "$f" ] || [ -L "$f" ]; then
            printf '%s\n' "$f"
        fi
    done > "$STAGE/list"
    case "$(tar --version 2>/dev/null)" in
        *bsdtar*) set -- --no-xattrs --no-mac-metadata ;;
        *) set -- ;;
    esac
    COPYFILE_DISABLE=1 tar -c -z -f "$STAGE/tree.tar.gz" "$@" -T "$STAGE/list"
    echo "gnu: tooling: staged $(wc -l < "$STAGE/list" | tr -d ' ') files," \
        "$(($(wc -c < "$STAGE/tree.tar.gz") / 1048576)) MiB compressed"
}

# write_env: write STAGE/job.env, the job's settings, as shell assignments.
write_env()
{
    for v in "$RACH2_ARCH" "${GNU_AGENTS:-}" "${GNU_LINK:-}" "${RACH2_PATH_MAP:-}" "$MUSL_CRYPT_SRC"; do
        case "$v" in
            *"'"*)
                echo "gnu: tooling: a setting has a quote: $v" >&2
                exit 1
                ;;
        esac
    done
    cat > "$STAGE/job.env" << EOF
RACH2_ARCH='$RACH2_ARCH'
GNU_AGENTS='${GNU_AGENTS:-}'
GNU_LINK='${GNU_LINK:-}'
RACH2_PATH_MAP='${RACH2_PATH_MAP:-}'
MUSL_CRYPT_SRC='$MUSL_CRYPT_SRC'
IMAGE='$(basename "$OUT")'
SIZE='$SIZE'
JOBS='$CPUS'
DRY_RUN='$DRY'
EOF
}

# sparse FILE MIB: make FILE at least MIB MiB long, as a hole.
sparse()
{
    have=0
    [ ! -f "$1" ] || have=$(($(wc -c < "$1") / 1048576))
    [ "$have" -ge "$2" ] || dd if=/dev/null of="$1" bs=1048576 count=0 seek="$2" 2> /dev/null
}

# follow: print the lines the job marked as the build's, from the channel of
# the job whose etude-tooling's process ID is in STAGE/pid, until
# STAGE/ended exists. The channel's file is read through a link in STAGE,
# which keeps it after etude-tooling removes the job's directory, so the
# lines of a job's last seconds are printed too.
follow()
{
    log=
    shown=0
    while :; do
        ended=
        [ ! -e "$STAGE/ended" ] || ended=1
        if [ -z "$log" ] && [ -s "$STAGE/pid" ]; then
            for d in "$CACHE/job-$(cat "$STAGE/pid")-"*; do
                [ -f "$d/channel.log" ] || continue
                log=$d/channel.log
                ! ln "$log" "$STAGE/channel.log" 2> /dev/null || log=$STAGE/channel.log
            done
        fi
        if [ -n "$log" ] && [ -f "$log" ]; then
            lines=$(wc -l < "$log" 2> /dev/null) || lines=$shown
            if [ "$lines" -gt "$shown" ]; then
                sed -n -e "$((shown + 1)),${lines}s/^gnu-vm: //p" -e "${lines}q" "$log" | tr -d '\r'
                shown=$lines
            fi
        fi
        [ -z "$ended" ] || return 0
        sleep 5
    done
}

take_lock
sweep
STAGE=$(mktemp -d "$GNU_OUT/.tooling.XXXXXX")
tooling check > /dev/null 2>&1 || tooling prepare -c "$CPUS"
stage_tree
write_env
sparse "$DISK" $((DISK_GIB * 1024))
set -- -d "$DISK"
if [ -z "$DRY" ]; then
    sparse "$STAGE/image" "$SIZE"
    set -- "$@" -d "$STAGE/image" -o digest
fi
volume=$(($(wc -c < "$STAGE/tree.tar.gz") / 1048576 + 64))
echo "gnu: tooling: $RACH2_ARCH: the job runs in the tooling machine with $CPUS processors and $MIB MiB, on $DISK"
follow &
FOLLOWER=$!
"$TOOLING" -r "$REPO/tools/etude/tooling" -C "$CACHE" -a "$RACH2_QEMU_ACCEL" run -n \
    -c "$CPUS" -m "$MIB" -s "$volume" -t "$DEADLINE" -i "$STAGE/tree.tar.gz" -i "$STAGE/job.env" \
    "$@" -O "$STAGE" "$DIR/tooling_job.sh" &
JOB=$!
echo "$JOB" > "$STAGE/pid"
status=0
wait "$JOB" || status=$?
JOB=
touch "$STAGE/ended"
wait "$FOLLOWER" || :
FOLLOWER=
if [ "$status" -ne 0 ]; then
    echo "gnu: tooling: the job failed with status $status; $DISK keeps what it built, and the next make goes on from there" >&2
    exit 1
fi
[ -z "$DRY" ] || exit 0
want=$(cat "$STAGE/digest")
got=$("$SHA" "$STAGE/image" | awk '{print $1}')
if [ "$got" != "$want" ]; then
    echo "gnu: tooling: the image arrived with sha256 $got, and the machine made $want" >&2
    exit 1
fi
mv "$STAGE/image" "$OUT"
echo "gnu: tooling: wrote $OUT, sha256 $got"
