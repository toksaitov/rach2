#!/usr/bin/env sh

#
# regen_grub.sh: regenerate the pinned GRUB boot artifacts. Maintainer only.
#
# The ordinary build consumes the eight artifacts beside this script and never
# runs GRUB's tools; this procedure is how those artifacts are made when a
# GRUB upgrade is chosen on purpose. It runs GRUB inside the pinned Ubuntu
# container with apt pointed at one dated state of the Ubuntu snapshot
# archive, generates every artifact twice from clean directories, requires
# the two rounds byte-identical, and writes manifest.txt recording the
# container digest, the snapshot identifier, every package the install
# added with its version, the three GRUB packages' .deb digests, the tool
# version, each command's module list, the two embedded configurations'
# digests, and each output's size and digest. The container digest and the
# snapshot identifier name the regeneration's inputs exactly; a future run
# with the same two produces the same bytes without depending on what the
# archive holds that day. Replacing the committed artifacts and manifest is a
# reviewed change, and the GRUB suites must pass on the result before it
# lands.
#
# Usage: sh vendor/grub/regen_grub.sh
#
# SNAPSHOT below pins the archive state; a maintainer moving to newer
# packages raises it (any past timestamp the snapshot service holds,
# YYYYMMDDTHHMMSSZ) and commits the result.
#

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"

UBUNTU_IMAGE="$(sed -n 's/^\(ubuntu@sha256:[0-9a-f]*\)$/\1/p' \
                "$REPO/tools/image/ubuntu-image.txt" | head -1)"
[ -n "$UBUNTU_IMAGE" ] || {
    echo "regen_grub: tools/image/ubuntu-image.txt has no digest" >&2; exit 1; }

command -v docker >/dev/null 2>&1 || {
    echo "regen_grub: docker is required for this maintainer procedure" >&2
    exit 1
}

EMBED="$REPO/tools/image/grub-embed.cfg"
EFI_EMBED="$REPO/tools/image/grub-efi-embed.cfg"
DISK_MODULES="biosdisk part_msdos fat normal multiboot configfile loadenv echo test serial terminal search search_label search_fs_uuid search_fs_file"
ISO_MODULES="biosdisk iso9660 part_msdos fat normal multiboot configfile loadenv echo test serial terminal"
# The core for a GUID-partitioned disk's BIOS boot partition reads the GUID
# table, and both images that boot a GPT disk can start a kernel by Multiboot 2
# as well as by Multiboot. The EFI image reads an ISO, for a CD, and passes the
# firmware's framebuffer through its GOP and UGA drivers.
GPT_MODULES="biosdisk part_gpt part_msdos fat normal multiboot multiboot2 configfile loadenv echo test serial terminal search search_label search_fs_uuid search_fs_file"
EFI_MODULES="part_gpt part_msdos fat iso9660 normal multiboot multiboot2 configfile loadenv echo test serial terminal search search_label search_fs_uuid search_fs_file efi_gop efi_uga"
ARTIFACTS="bios-boot.img boot-hybrid.img disk-core.img disk-core-a.img disk-core-b.img eltorito.img gpt-core.img bootx64.efi"
GRUB_PACKAGES="grub-pc-bin grub-efi-amd64-bin grub-common"

# The pinned archive state. Every package this procedure installs, except
# the ca-certificates bootstrap noted below, comes from this one dated
# snapshot, so the container digest and this identifier together name the
# regeneration's inputs.
SNAPSHOT="${RACH2_GRUB_SNAPSHOT:-20260825T013635Z}"
SNAPURL="https://snapshot.ubuntu.com/ubuntu/$SNAPSHOT"

# One generation round into $1. Every artifact the ordinary build consumes is
# produced here, and nothing else.
round() {
    out="$1"
    mkdir -p "$out"
    docker run --rm --platform linux/amd64 -v "$REPO":"$REPO" -w "$REPO" \
        "$UBUNTU_IMAGE" sh -e -c "
        dpkg-query -W -f '\${Package} \${Version}\n' > /tmp/pkgs.before
        # The snapshot service serves only TLS and the base image has no CA
        # roots, so ca-certificates comes from the container's default
        # archive first. It carries trust anchors, not bytes that reach any
        # artifact; everything installed after this line comes from the one
        # dated snapshot, verified against apt's signed indexes, and the
        # whole installed set is recorded below.
        apt-get update -qq >/dev/null 2>&1
        apt-get install -y -qq ca-certificates >/dev/null 2>&1
        sed -i 's|http://archive.ubuntu.com/ubuntu/|$SNAPURL|; s|http://security.ubuntu.com/ubuntu/|$SNAPURL|' \
            /etc/apt/sources.list.d/ubuntu.sources
        rm -rf /var/lib/apt/lists/*
        apt-get update -qq -o Acquire::Check-Valid-Until=false >/dev/null
        apt-get install -y -qq $GRUB_PACKAGES >/dev/null 2>&1
        dpkg-query -W -f '\${Package} \${Version}\n' > /tmp/pkgs.after
        cp /usr/lib/grub/i386-pc/boot.img '$out/bios-boot.img'
        cp /usr/lib/grub/i386-pc/boot_hybrid.img '$out/boot-hybrid.img'
        grub-mkimage -O i386-pc -p '(hd0,msdos1)/boot/grub' \
            -c '$EMBED' -o '$out/disk-core.img' $DISK_MODULES
        for m in A B; do
            {
                printf 'serial --unit=0 --speed=115200\n'
                printf 'terminal_output serial console\n'
                printf 'echo GRUBCORE=%s\n' \"\$m\"
                cat '$EMBED'
            } > /tmp/marked.cfg
            grub-mkimage -O i386-pc -p '(hd0,msdos1)/boot/grub' \
                -c /tmp/marked.cfg -o \"$out/disk-core-\$(echo \$m | tr AB ab).img\" \
                $DISK_MODULES
        done
        grub-mkimage -O i386-pc-eltorito -p /boot/grub \
            -o '$out/eltorito.img' $ISO_MODULES
        grub-mkimage -O i386-pc -p '(hd0,gpt1)/boot/grub' \
            -c '$EMBED' -o '$out/gpt-core.img' $GPT_MODULES
        grub-mkimage -O x86_64-efi -p /boot/grub \
            -c '$EFI_EMBED' -o '$out/bootx64.efi' $EFI_MODULES
        {
            echo '# vendor/grub/manifest.txt: provenance of the pinned GRUB artifacts.'
            echo '# Regenerated only by regen_grub.sh; see vendor/grub/Readme.md.'
            echo \"container $UBUNTU_IMAGE\"
            echo 'snapshot $SNAPURL'
            mkdir -p /tmp/debs
            ( cd /tmp/debs && apt-get download -qq $GRUB_PACKAGES >/dev/null 2>&1 )
            for p in $GRUB_PACKAGES; do
                deb=\$(ls /tmp/debs/\$p*.deb)
                echo \"package \$p \$(dpkg-query -W -f '\${Version}' \$p) deb-sha256 \$(sha256sum \$deb | cut -d' ' -f1)\"
            done
            # Every package the install added or changed, resolver-selected
            # dependencies included, so the recorded inputs are the whole
            # install and not just the three names asked for.
            comm -13 /tmp/pkgs.before /tmp/pkgs.after | \
                while read -r p v; do echo \"installed \$p \$v\"; done
            echo \"tool \$(grub-mkimage --version)\"
            echo 'disk-modules $DISK_MODULES'
            echo 'iso-modules $ISO_MODULES'
            echo 'gpt-modules $GPT_MODULES'
            echo 'efi-modules $EFI_MODULES'
        } > '$out/manifest.head'
    "
    # The digests are taken on the host with the project tool, outside the
    # container, so the record and the verifier use one implementation.
    SHA="$REPO/build/tools/verify/sha256"
    make -s -C "$REPO" build/tools/verify/sha256 || exit 1
    {
        cat "$out/manifest.head"
        printf 'embed-cfg %s\n' "$("$SHA" "$EMBED" | awk '{print $1}')"
        printf 'efi-embed-cfg %s\n' "$("$SHA" "$EFI_EMBED" | awk '{print $1}')"
        for a in $ARTIFACTS; do
            printf 'artifact %s %s %s\n' "$a" \
                "$(wc -c < "$out/$a" | tr -d ' ')" \
                "$("$SHA" "$out/$a" | awk '{print $1}')"
        done
    } > "$out/manifest.txt"
    rm -f "$out/manifest.head"
}

# GRUB reads the two embedded configurations with its rescue parser, where a
# quote mark in a comment or a missing command loses the recovery path with no
# message, so cfgcheck checks both before any image is built from them.
make -s -C "$REPO" build/tools/inspect/cfgcheck
"$REPO/build/tools/inspect/cfgcheck" grubembed "$EMBED"
"$REPO/build/tools/inspect/cfgcheck" grubefi "$EFI_EMBED"

R1="$REPO/build/grub-regen/round1"
R2="$REPO/build/grub-regen/round2"
rm -rf "$REPO/build/grub-regen"
echo "regen_grub: round 1"
round "$R1"
echo "regen_grub: round 2"
round "$R2"

for a in $ARTIFACTS manifest.txt; do
    cmp -s "$R1/$a" "$R2/$a" || {
        echo "regen_grub: $a differs between the two rounds; not reproducible" >&2
        exit 1
    }
done

echo "regen_grub: the two rounds agree; staging into vendor/grub/"
for a in $ARTIFACTS manifest.txt; do
    cp "$R1/$a" "$SCRIPT_DIR/$a"
done
rm -rf "$REPO/build/grub-regen"
echo "regen_grub: result: staged; run tests/run grub, menu, install, sealed, drill, installer and uefi before committing"
