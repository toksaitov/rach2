#!/bin/sh
#
# provision.sh: make the tooling machine on its first boot from the cloud
# image. cloud-init runs it as root, with the seed volume mounted at SEED.
# It installs the packages that give the commands of SEED/tools, lets an
# ordinary user make user namespaces, installs the job runner and its
# service, turns off what would slow a later boot, and checks
# that every command is found. cloud-init then powers the machine off.
#
# On amd64 it installs the i386 runtime for programs the GNU build runs.
# On arm64 it installs QEMU's user-mode emulators and registers them
# with the kernel for i386 and x86-64 programs, with the flag F, which opens
# the emulator once, so that a program in a chroot or another namespace runs
# too; the GNU layer's build runs the programs it builds.
#
# It reports on the machine's second serial port, ttyS1 on amd64 and ttyAMA1
# on arm64, which etude keeps:
# a line `etude-tooling: tool NAME PATH VERSION` for each command, and last
# `etude-tooling: provision: ok`, or `etude-tooling: provision: failed: WHY`.
#
# Usage: sh provision.sh SEED
#
set -u
SEED=${1:?seed}
CHAN=/dev/ttyS1
[ ! -c /dev/ttyAMA1 ] || CHAN=/dev/ttyAMA1
PACKAGES='util-linux iproute2 exfatprogs dosfstools e2fsprogs python3 openssl git
curl wget ca-certificates build-essential gawk bison flex m4 perl
texinfo xz-utils patch gzip bzip2 tar gnupg gpgv binutils mandoc mtools xorriso qemu-utils
openssh-client rsync cppcheck shellcheck clang clang-format gcc-i686-linux-gnu binutils-i686-linux-gnu
gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu sparse'
X86=
if [ "$(uname -m)" = aarch64 ]; then
    X86='qemu-i386 qemu-x86_64'
    PACKAGES="$PACKAGES qemu-user qemu-user-binfmt"
else
    PACKAGES="$PACKAGES libc6-i386"
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
    if [ -f /var/log/etude-provision.log ]; then
        tail -n 30 /var/log/etude-provision.log | while IFS= read -r line; do
            say "provision log: $line"
        done
    fi
    say "provision: failed: $*"
    exit 1
}

# Keep a command's output and relay it without interpreting it as a report.
run_logged()
{
    { "$@"; echo "$?" > /run/etude-provision-status; } 2>&1 |
        while IFS= read -r line; do
            printf '%s\n' "$line" >> /var/log/etude-provision.log
            say "packages: $line"
        done
    status=$(cat /run/etude-provision-status) || return 1
    rm -f /run/etude-provision-status
    return "$status"
}

export DEBIAN_FRONTEND=noninteractive
: > /var/log/etude-provision.log
say 'provision: packages'
for source in /etc/apt/sources.list /etc/apt/sources.list.d/*.sources \
    /etc/apt/sources.list.d/*.list; do
    [ ! -f "$source" ] || sed -i \
        -e 's|http://archive.ubuntu.com/|https://archive.ubuntu.com/|g' \
        -e 's|http://security.ubuntu.com/|https://security.ubuntu.com/|g' \
        -e 's|http://ports.ubuntu.com/|https://ports.ubuntu.com/|g' "$source" ||
        fail 'the apt sources'
done
run_logged apt-get update -q || fail 'apt-get update'
# shellcheck disable=SC2086 # the list is words
run_logged apt-get install -y -q --no-install-recommends $PACKAGES ||
    fail 'apt-get install'
if [ "$(uname -m)" = x86_64 ]; then
    /lib/ld-linux.so.2 --version > /dev/null || fail 'the i386 dynamic loader does not run'
fi
run_logged /usr/lib/systemd/systemd-networkd-wait-online --any ||
    fail 'the network after package service restarts'
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
