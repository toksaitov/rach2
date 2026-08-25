#!/usr/bin/env sh

#
# regen_grub.sh: regenerate the pinned GRUB boot artifacts. Maintainer only.
#
# The ordinary build consumes the six artifacts beside this script and never
# runs GRUB's tools; this procedure is how those artifacts are made when a
# GRUB upgrade is chosen on purpose. It runs GRUB inside the pinned Ubuntu
# container, generates every artifact twice from clean directories, requires
# the two rounds byte-identical, and writes manifest.txt recording the
# container digest, the exact package versions and their .deb digests, the
# tool version, each command's module list, the embedded configuration's
# digest, and each output's size and digest. Replacing the committed
# artifacts and manifest is a reviewed change, and the GRUB suites must pass
# on the result before it lands.
#
# Usage: sh vendor/grub/regen_grub.sh
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
DISK_MODULES="biosdisk part_msdos fat normal multiboot configfile loadenv echo test serial terminal"
ISO_MODULES="biosdisk iso9660 part_msdos fat normal multiboot configfile loadenv echo test serial terminal"

# One generation round into $1. Every artifact the ordinary build consumes is
# produced here, and nothing else.
round() {
    out="$1"
    mkdir -p "$out"
    docker run --rm --platform linux/amd64 -v "$REPO":"$REPO" -w "$REPO" \
        "$UBUNTU_IMAGE" sh -e -c "
        apt-get update -qq >/dev/null 2>&1
        apt-get install -y -qq grub-pc-bin grub-common >/dev/null 2>&1
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
        {
            echo '# vendor/grub/manifest.txt: provenance of the pinned GRUB artifacts.'
            echo '# Regenerated only by regen_grub.sh; see vendor/grub/Readme.md.'
            echo \"container $UBUNTU_IMAGE\"
            reldate=\$(grep -h '^Date:' /var/lib/apt/lists/*Release 2>/dev/null | head -1 | cut -d' ' -f2-)
            echo \"snapshot https://snapshot.ubuntu.com/ubuntu/ at archive state: \$reldate\"
            mkdir -p /tmp/debs
            ( cd /tmp/debs && apt-get download -qq grub-pc-bin grub-common >/dev/null 2>&1 )
            for p in grub-pc-bin grub-common; do
                deb=\$(ls /tmp/debs/\$p*.deb)
                echo \"package \$p \$(dpkg-query -W -f '\${Version}' \$p) deb-sha256 \$(sha256sum \$deb | cut -d' ' -f1)\"
            done
            echo \"tool \$(grub-mkimage --version)\"
            echo 'disk-modules $DISK_MODULES'
            echo 'iso-modules $ISO_MODULES'
        } > '$out/manifest.head'
    "
    # The digests are taken on the host with the project tool, outside the
    # container, so the record and the verifier use one implementation.
    SHA="$REPO/build/tools/verify/sha256"
    [ -x "$SHA" ] || {
        mkdir -p "${SHA%/*}"
        cc -O2 -o "$SHA" "$REPO/tools/verify/sha256.c"
    }
    {
        cat "$out/manifest.head"
        printf 'embed-cfg %s\n' "$("$SHA" "$EMBED" | awk '{print $1}')"
        for a in bios-boot.img boot-hybrid.img disk-core.img \
                 disk-core-a.img disk-core-b.img eltorito.img; do
            printf 'artifact %s %s %s\n' "$a" \
                "$(wc -c < "$out/$a" | tr -d ' ')" \
                "$("$SHA" "$out/$a" | awk '{print $1}')"
        done
    } > "$out/manifest.txt"
    rm -f "$out/manifest.head"
}

R1="$REPO/build/grub-regen/round1"
R2="$REPO/build/grub-regen/round2"
rm -rf "$REPO/build/grub-regen"
echo "regen_grub: round 1"
round "$R1"
echo "regen_grub: round 2"
round "$R2"

for a in bios-boot.img boot-hybrid.img disk-core.img disk-core-a.img \
         disk-core-b.img eltorito.img manifest.txt; do
    cmp -s "$R1/$a" "$R2/$a" || {
        echo "regen_grub: $a differs between the two rounds; not reproducible" >&2
        exit 1
    }
done

echo "regen_grub: the two rounds agree; staging into vendor/grub/"
for a in bios-boot.img boot-hybrid.img disk-core.img disk-core-a.img \
         disk-core-b.img eltorito.img manifest.txt; do
    cp "$R1/$a" "$SCRIPT_DIR/$a"
done
rm -rf "$REPO/build/grub-regen"
echo "regen_grub: result: staged; run tests/run grub, menu, install, sealed, and drill before committing"
