#!/bin/sh
#
# provision.sh: make the tooling machine on its first boot from the cloud
# image. cloud-init runs it as root, with the seed volume mounted at SEED.
# It installs the packages that give the commands of SEED/tools and the
# pinned Codex, lets an ordinary user make user namespaces, installs the job
# runner and its service, turns off what would slow a later boot, and checks
# that every command is found. cloud-init then powers the machine off.
#
# On arm64 it also installs QEMU's user-mode emulators and registers them
# with the kernel for i386 and x86-64 programs, with the flag F, which opens
# the emulator once, so that a program in a chroot or another namespace runs
# too; the GNU layer's build runs the programs it builds.
#
# It reports on the machine's second serial port, ttyS1 on amd64 and ttyAMA1
# on arm64, which etude-tooling keeps:
# a line `etude-tooling: tool NAME PATH VERSION` for each command, and last
# `etude-tooling: provision: ok`, or `etude-tooling: provision: failed: WHY`.
#
# Usage: sh provision.sh SEED
#
set -u
SEED=${1:?seed}
CHAN=/dev/ttyS1
[ ! -c /dev/ttyAMA1 ] || CHAN=/dev/ttyAMA1
# The version of Codex that the codexapi cases run,
# tests/cases/host/agentapi.sh.
CODEX_VERSION=0.162.1
PACKAGES='util-linux iproute2 exfatprogs dosfstools e2fsprogs python3 openssl git
curl wget ca-certificates nodejs npm build-essential gawk bison flex m4 perl
texinfo xz-utils patch gzip bzip2 tar gnupg gpgv binutils mandoc'
X86=
if [ "$(uname -m)" = aarch64 ]; then
    X86='qemu-i386 qemu-x86_64'
    PACKAGES="$PACKAGES qemu-user qemu-user-binfmt"
fi
# What a boot of the tooling machine does not need: the network's services,
# the package timers, snapd and cloud-init itself.
MASK='snapd.service snapd.socket snapd.seeded.service apt-daily.timer
apt-daily-upgrade.timer unattended-upgrades.service man-db.timer motd-news.timer
fwupd-refresh.timer systemd-networkd-wait-online.service ssh.service ssh.socket'

say()
{
    printf 'etude-tooling: %s\n' "$*" > "$CHAN"
}

fail()
{
    say "provision: failed: $*"
    exit 1
}

export DEBIAN_FRONTEND=noninteractive
say 'provision: packages'
apt-get update -q > /var/log/etude-provision.log 2>&1 || fail 'apt-get update'
# shellcheck disable=SC2086 # the list is words
apt-get install -y -q --no-install-recommends $PACKAGES >> /var/log/etude-provision.log 2>&1 ||
    fail 'apt-get install'
say "provision: codex $CODEX_VERSION"
npm install -g "@openai/codex@$CODEX_VERSION" >> /var/log/etude-provision.log 2>&1 ||
    fail "npm install codex $CODEX_VERSION"

say 'provision: the job runner'
echo 'kernel.apparmor_restrict_unprivileged_userns = 0' > /etc/sysctl.d/60-etude-userns.conf
sysctl -q -p /etc/sysctl.d/60-etude-userns.conf || fail 'the sysctl of user namespaces'
install -m 0755 "$SEED/runner.sh" /usr/local/sbin/etude-job-runner || fail 'the runner'
install -m 0644 "$SEED/etude-job.service" /etc/systemd/system/etude-job.service ||
    fail 'the service'
systemctl enable etude-job.service > /dev/null 2>&1 || fail 'systemctl enable etude-job.service'
# shellcheck disable=SC2086 # the list is words
systemctl mask $MASK > /dev/null 2>&1 || fail 'systemctl mask'
touch /etc/cloud/cloud-init.disabled || fail 'cloud-init.disabled'

grep -v '^#' "$SEED/tools" > /run/etude-tools || fail 'the tool list'
while read -r tool; do
    path=$(command -v "$tool") || fail "$tool is not found"
    version=$("$tool" --version 2>/dev/null < /dev/null) || version=
    version=$(printf '%s\n' "$version" | head -n 1 | tr -c '[:print:]\n' ' ')
    say "tool $tool $path ${version:--}"
done < /run/etude-tools
[ "$(codex --version 2>/dev/null)" = "codex-cli $CODEX_VERSION" ] ||
    fail "codex is not version $CODEX_VERSION"
modprobe exfat || fail 'the exFAT driver does not load'
for f in $X86; do
    grep -q '^flags: .*F' "/proc/sys/fs/binfmt_misc/$f" 2>/dev/null ||
        fail "the kernel runs no $f program through QEMU with the flag F"
    say "binfmt $f $(sed -n 's/^interpreter //p' "/proc/sys/fs/binfmt_misc/$f")"
done
setpriv --reuid=65534 --regid=65534 --clear-groups unshare --user --map-root-user true ||
    fail 'an ordinary user cannot make a user namespace'
apt-get clean
sync
say 'provision: ok'
