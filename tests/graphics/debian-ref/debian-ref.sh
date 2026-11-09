#!/bin/sh
# debian-ref.sh: the Debian 13 reference and builder guest of the graphics
# tests, booted in the same QEMU, accelerator and display profiles as
# Rach2's graphics runs (rach2/qemu_devices.sh).
#
#   debian-ref.sh fetch
#   debian-ref.sh prepare [-n]
#   debian-ref.sh run [-p PROFILE] [-c CPUS] [-m MIB] [-g CLASS] [-d] [-l]
#                     [-r RECORD] [-i FILE]... [-o FILE]... COMMAND
#
# `fetch` downloads the pinned cloud image that `image` names and checks its
# size and SHA-512. `prepare` makes the prepared image, an overlay of it:
# one boot with the seed, whose `network-config` is empty, runs setup.sh.in,
# which installs `packages` from the dated snapshot and the Venus driver that
# vendor/gnu/debian/venus.list pins, and turns cloud-init off. The image's
# key, the SHA-256 of the seed's files and the image's pins, is kept beside
# it; with -n `prepare` does nothing when an image of the same key exists,
# and `run` refuses an image of another key.
# `run` boots a new overlay of the prepared image with the display PROFILE
# (std-vga by default), CPUS processors (8) and MIB of memory (8192), copies
# tests/graphics/guest-env.sh to /tmp/guest-env.sh and each FILE of -i to
# /tmp under its own name, runs COMMAND in sh as the user `gfx` over SSH with
# that file sourced for the run's CLASS (performance by default; -d for a
# Done's run), copies the guest's /tmp/NAME to each FILE of -o, NAME being
# its last component, once COMMAND has succeeded, powers the guest off and
# removes the overlay. It writes the run's record to RECORD, or to standard
# error: the profile, firmware, accelerator, processor, CPUs, memory, QEMU's
# version, the image, the claim it ran in and the guest's environment. It
# exits with COMMAND's status, or 1 when a file of -o cannot be copied.
#
# On a machine with an installed site policy, /etc/rach2/etude-site or the
# file ETUDE_SITE names, `run` runs as one claim of the site's ledger through
# the tree's `etude admit -c`, which takes the guest's processors and its
# memory with QEMU's 1 GiB: every CPU, or with -l, a light guest's, two.
# It waits in the ledger for the site's wait, as etude's own guests do.
#
# Debian's GRUB under SeaBIOS resets at once with a display that has no VGA,
# so the virtio-gpu and none profiles boot by OVMF and the others by
# SeaBIOS. The images and the account's key are in DEBIAN_REF_CACHE, by
# default build/cache/debian-ref of the clone whose worktree this is, so that
# its worktrees share one prepared image. ACCEL chooses the accelerator as
# qemu_devices.sh says: KVM where /dev/kvm opens.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
common=$(git -C "$tree" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || echo "$tree/.git")
cache=${DEBIAN_REF_CACHE:-$(dirname "$common")/build/cache/debian-ref}

die()
{
    echo "debian-ref: $*" >&2
    exit 1
}

pin()
{
    awk -v n="$1" '$1 == n { print $2 }' "$here/image"
}

url=$(pin url)
base=$cache/$(basename "$url")
prepared=$cache/prepared.qcow2
key=$cache/id_ed25519
venus=$(awk '$1 == "mesa-vulkan-drivers-amd64"' "$tree/vendor/gnu/debian/venus.list")
[ -n "$venus" ] || die "vendor/gnu/debian/venus.list has no amd64 line"

# seedkey: the key of the image that the seed's files and the pins make.
seedkey()
{
    sha256sum "$here/setup.sh.in" "$here/packages" "$here/user-data.in" \
        "$here/meta-data" "$here/network-config" "$here/image" |
        awk '{ print $1 }' | { cat; echo "$venus"; } | sha256sum | awk '{ print $1 }'
}

fetch()
{
    mkdir -p "$cache"
    if [ ! -f "$base" ]; then
        echo "debian-ref: fetching $url"
        curl -sSfL -o "$base.part" "$url"
        mv "$base.part" "$base"
    fi
    [ "$(wc -c < "$base")" -eq "$(pin size)" ] || die "$base: not $(pin size) bytes"
    echo "$(pin sha512)  $base" | sha512sum -c --quiet - || die "$base: SHA-512 differs"
    echo "debian-ref: result: $base checks"
}

# machine PROFILE CPUS MIB OVERLAY: set QEMU's words for the guest in `words`
# and its firmware in `firmware`, sourcing qemu_devices.sh for the profile
# and the accelerator.
machine()
{
    # shellcheck disable=SC2034 # qemu_devices.sh reads them
    DISPLAY_PROFILE=$1 DISPLAY_MODE=none ARCH=x86_64 AUDIO_DEVICE=none
    # shellcheck disable=SC2034 # qemu_devices.sh reads them
    NIC=virtio-net-pci NET_HOSTFWD="hostfwd=tcp:127.0.0.1:$port-:22" MEMORY=${3}M
    # shellcheck source=../../../rach2/qemu_devices.sh
    . "$tree/rach2/qemu_devices.sh"
    case $1 in
        virtio-gpu|none)
            # shellcheck source=../../../tools/setup/ovmf_path.sh
            . "$tree/tools/setup/ovmf_path.sh"
            [ -n "$OVMF_CODE" ] || die "no OVMF for the $1 profile"
            firmware="ovmf $OVMF_CODE"
            fw_opts=$OVMF_ARGS
            ;;
        *)
            firmware=seabios
            fw_opts=
            ;;
    esac
    words="$ACCEL_OPTS -smp $2 -m ${3}M -machine q35 -nodefaults -no-reboot $fw_opts"
    words="$words -drive file=$4,if=virtio,format=qcow2 $NET_OPTS -device virtio-rng-pci"
    words="$words $DISPLAY_OPTS"
}

free_port()
{
    python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'
}

# watch PID LOG MARK QUIET: wait for QEMU's PID to end while its serial LOG
# grows, and stop it once the log has not grown for QUIET seconds or holds
# MARK. Succeed when the log holds MARK.
watch()
{
    last=-1 still=0
    while kill -0 "$1" 2>/dev/null; do
        sleep 5
        grep -aq "$3" "$2" 2>/dev/null && break
        size=$(wc -c < "$2" 2>/dev/null || echo 0)
        if [ "$size" -eq "$last" ]; then
            still=$((still + 5))
            [ "$still" -lt "$4" ] || { echo "debian-ref: no console output for $4 s" >&2; break; }
        else
            last=$size still=0
        fi
    done
    grep -aq "$3" "$2"
}

prepare()
{
    if [ "${1:-}" = -n ] && [ -f "$prepared" ] &&
        [ "$(cat "$prepared.key" 2>/dev/null)" = "$(seedkey)" ]; then
        return 0
    fi
    fetch
    command -v xorriso >/dev/null || die "xorriso makes the seed; install it"
    [ -f "$key" ] || ssh-keygen -q -t ed25519 -N '' -C debian-ref -f "$key"
    work=$cache/prepare
    rm -rf "$work"
    mkdir -p "$work/seed"
    trap 'kill "$qemu" 2>/dev/null || true; rm -rf "$work"' EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    # shellcheck disable=SC2086 # the line's fields
    set -- $venus
    sed -e "s|@SNAPSHOT@|$(pin snapshot)|" \
        -e "s|@PACKAGES@|$(grep -v '^#' "$here/packages" | tr '\n' ' ')|" \
        -e "s|@VENUS_VERSION@|$2|" -e "s|@VENUS_URL@|$3|" -e "s|@VENUS_SHA256@|$4|" \
        "$here/setup.sh.in" > "$work/setup.sh"
    sed -e "s|@KEY@|$(cat "$key.pub")|" -e "s|@SETUP@|$(base64 -w0 < "$work/setup.sh")|" \
        "$here/user-data.in" > "$work/seed/user-data"
    cp "$here/meta-data" "$here/network-config" "$work/seed/"
    xorriso -as mkisofs -quiet -o "$work/seed.iso" -V cidata -J -r "$work/seed"
    rm -f "$prepared" "$prepared.key"
    qemu-img create -q -f qcow2 -F qcow2 -b "$base" "$work/prepared.qcow2" 32G
    port=$(free_port)
    machine std-vga 8 8192 "$work/prepared.qcow2"
    echo "debian-ref: preparing on $firmware, $ACCEL_OPTS; console in $work/console.log"
    # shellcheck disable=SC2086 # words
    qemu-system-x86_64 $words -drive file="$work/seed.iso",if=virtio,format=raw,readonly=on \
        -serial file:"$work/console.log" -monitor none &
    qemu=$!
    watch "$qemu" "$work/console.log" 'debian-ref: \(prepared\|failed\)' 1200 || true
    wait "$qemu" || true
    grep -a 'debian-ref: ' "$work/console.log" | tr -d '\r' | sed 's/^.*debian-ref:/debian-ref:/'
    grep -aq 'debian-ref: prepared' "$work/console.log" || die "result: preparation failed"
    mv "$work/prepared.qcow2" "$prepared"
    chmod a-w "$prepared"
    seedkey > "$prepared.key"
    echo "debian-ref: result: prepared $prepared"
}

# site_field NAME: the value of the field NAME of the site policy $site.
site_field()
{
    sed -n "s/^$1 //p" "$site"
}

# admitted ARGS: run `run ARGS` again as a claim of the site's ledger, with
# the processors and the memory that $cpus, $mib and $light give.
admitted()
{
    etude=$tree/build/tools/etude/etude
    make -s -C "$tree" ARCH=i686 build/tools/etude/etude >&2 || die "run: etude did not build"
    if [ "$light" = 1 ] && [ "$cpus" -gt 2 ]; then
        millis=2000
    else
        millis=$((cpus * 1000))
    fi
    bytes=$(((mib + 1024) * 1048576))
    id=debian-ref:$(id -un):$$:$(date +%s)
    exec "$etude" admit -c -d "$(site_field locks)" -w "$(site_field wait)" \
        "$(site_field ledger)" "$id" "$(id -un)" shared "$millis,$bytes,1" \
        "$(site_field capacity)" "$(site_field seconds)" \
        "$(command -v env)" DEBIAN_REF_CLAIM="$id" sh "$0" run "$@"
}

run()
{
    profile=std-vga cpus=8 mib=8192 class=performance done=0 light=0 record='' ins='' outs=''
    site=${ETUDE_SITE:-/etc/rach2/etude-site}
    while getopts p:c:m:g:dlr:i:o: o; do
        case $o in
            p) profile=$OPTARG ;;
            c) cpus=$OPTARG ;;
            m) mib=$OPTARG ;;
            g) class=$OPTARG ;;
            d) done=1 ;;
            l) light=1 ;;
            r) record=$OPTARG ;;
            i)
                [ -f "$OPTARG" ] || die "run: no file $OPTARG"
                ins="$ins $OPTARG"
                ;;
            o) outs="$outs $OPTARG" ;;
            *) exit 2 ;;
        esac
    done
    [ $# -ge "$OPTIND" ] || die "run: no command"
    [ -f "$prepared" ] || die "no prepared image; run debian-ref.sh prepare"
    [ "$(cat "$prepared.key" 2>/dev/null)" = "$(seedkey)" ] ||
        die "the prepared image is not the one this tree's seed makes; run debian-ref.sh prepare"
    if [ -z "${DEBIAN_REF_CLAIM:-}" ] && [ -f "$site" ]; then
        admitted "$@"
    fi
    shift $((OPTIND - 1))
    mkdir -p "${TMPDIR:-$tree/build/tmp}"
    work=$(mktemp -d "${TMPDIR:-$tree/build/tmp}/debian-ref.XXXXXX")
    qemu=
    trap 'kill "$qemu" 2>/dev/null || true; rm -rf "$work"' EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    qemu-img create -q -f qcow2 -F qcow2 -b "$prepared" "$work/run.qcow2"
    port=$(free_port)
    machine "$profile" "$cpus" "$mib" "$work/run.qcow2"
    # shellcheck disable=SC2086 # words
    qemu-system-x86_64 $words -serial file:"$work/console.log" -monitor none &
    qemu=$!
    opts="-i $key -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
    tries=0
    # shellcheck disable=SC2086 # opts are words
    until ssh $opts -o ConnectTimeout=5 -p "$port" gfx@127.0.0.1 true 2>/dev/null </dev/null; do
        kill -0 "$qemu" 2>/dev/null || die "QEMU ended before SSH answered: $(tail -c 400 "$work/console.log")"
        tries=$((tries + 1))
        [ "$tries" -lt 120 ] || die "SSH did not answer in 120 tries"
        sleep 1
    done
    # shellcheck disable=SC2086 # opts are words
    scp -q $opts -P "$port" "$tree/tests/graphics/guest-env.sh" $ins gfx@127.0.0.1:/tmp/
    {
        echo "profile $profile"
        echo "firmware ${firmware%% *}"
        echo "accelerator $ACCEL"
        echo "cpu ${ACCEL_OPTS##*-cpu }"
        echo "cpus $cpus"
        echo "memory_mib $mib"
        echo "qemu $(qemu-system-x86_64 --version | head -n 1)"
        echo "image $(basename "$base") sha512 $(pin sha512)"
        echo "snapshot $(pin snapshot)"
        [ -z "${DEBIAN_REF_CLAIM:-}" ] || echo "claim $DEBIAN_REF_CLAIM"
    } > "$work/record"
    status=0
    # shellcheck disable=SC2086 # opts are words
    ssh $opts -p "$port" gfx@127.0.0.1 \
        "GRAPHICS_CLASS=$class GRAPHICS_DONE=$done; export GRAPHICS_CLASS GRAPHICS_DONE;
         . /tmp/guest-env.sh && graphics_env > /tmp/graphics.env && $*" </dev/null || status=$?
    for f in $outs; do
        [ "$status" -eq 0 ] || break
        # shellcheck disable=SC2086 # opts are words
        scp -q $opts -P "$port" "gfx@127.0.0.1:/tmp/$(basename "$f")" "$f" || {
            echo "debian-ref: run: cannot copy /tmp/$(basename "$f") out of the guest" >&2
            status=1
        }
    done
    # shellcheck disable=SC2086 # opts are words
    ssh $opts -p "$port" gfx@127.0.0.1 'cat /tmp/graphics.env; sudo systemctl poweroff' \
        </dev/null >> "$work/record" 2>/dev/null || true
    watch "$qemu" "$work/console.log" 'reboot: Power down' 120 || kill "$qemu" 2>/dev/null || true
    wait "$qemu" 2>/dev/null || true
    if [ -n "$record" ]; then
        cp "$work/record" "$record"
    else
        sed 's/^/debian-ref: record: /' "$work/record" >&2
    fi
    return "$status"
}

case ${1:-} in
    fetch) fetch ;;
    prepare) shift; prepare "$@" ;;
    run) shift; run "$@" ;;
    *)
        echo "usage: debian-ref.sh fetch | prepare [-n] | run [-p PROFILE] [-c CPUS] [-m MIB] [-g CLASS] [-d] [-l] [-r RECORD] [-i FILE]... [-o FILE]... COMMAND" >&2
        exit 2
        ;;
esac
