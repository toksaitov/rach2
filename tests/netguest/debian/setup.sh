#!/bin/sh
#
# setup.sh: the Debian gateway guest's setup, which runs once, as root, at
# the end of the preparing boot. prepare.sh puts it on the seed as
# cloud-init's user-data, after the lines that write the guest's boot script
# to /root/netguest.sh, with the date of Debian's snapshot in place of
# @SNAPSHOT@; cloud-init runs a user-data that begins with `#!` in its last
# stage.
#
# It installs nftables from the snapshot when the image lacks it, gives the
# interfaces to systemd-networkd with two files, the inner one by its
# address and any other Ethernet interface by DHCP, adds the unit that runs
# the boot script at every later boot, turns off cloud-init and what the
# gateway does not need, and powers the guest off. It reports on the serial
# port, which prepare.sh reads: `NETGUEST RECORD NAME VALUE` lines, then
# `NETGUEST PREPARED` or `NETGUEST FAILED reason`.

say()
{
    echo "NETGUEST $*" > /dev/ttyS0
}

fail()
{
    say "FAILED $*"
    exit 1
}

[ -s /root/netguest.sh ] || fail "the seed did not write /root/netguest.sh"
chmod 755 /root/netguest.sh

if ! command -v nft > /dev/null; then
    say "RECORD nftables installed from the snapshot @SNAPSHOT@"
    rm -f /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources
    cat > /etc/apt/sources.list.d/debian.sources << 'SOURCES'
Types: deb
URIs: http://snapshot.debian.org/archive/debian/@SNAPSHOT@
Suites: trixie trixie-updates
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg

Types: deb
URIs: http://snapshot.debian.org/archive/debian-security/@SNAPSHOT@
Suites: trixie-security
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
SOURCES
    : > /etc/apt/sources.list
    cat > /etc/apt/apt.conf.d/80rach2-snapshot << 'CONF'
Acquire::Check-Valid-Until "false";
Acquire::Retries "5";
APT::Install-Recommends "false";
CONF
    export DEBIAN_FRONTEND=noninteractive
    apt-get -q update > /dev/null 2>&1 || fail "apt-get update failed"
    apt-get -q -y install nftables > /dev/null 2>&1 || fail "nftables was not installed"
    apt-get -q -y clean
fi
command -v nft > /dev/null || fail "no nft"
command -v ip > /dev/null || fail "no ip"

# The interfaces are systemd-networkd's alone: the files cloud-init wrote
# for netplan or ifupdown go, and the inner interface's file sorts before
# the one that takes every other Ethernet interface.
rm -f /etc/netplan/*.yaml /etc/network/interfaces.d/* /etc/systemd/network/*
mkdir -p /etc/systemd/network
cat > /etc/systemd/network/05-netguest-inner.network << 'NETWORK'
[Match]
MACAddress=52:54:00:77:00:01

[Network]
Address=10.77.0.1/24
LinkLocalAddressing=no
IPv6AcceptRA=no
NETWORK
cat > /etc/systemd/network/10-netguest-outer.network << 'NETWORK'
[Match]
Type=ether

[Network]
DHCP=ipv4
LinkLocalAddressing=no
IPv6AcceptRA=no
NETWORK

cat > /etc/systemd/system/netguest.service << 'UNIT'
[Unit]
Description=The gateway between Rach2 and the network
After=systemd-networkd.service
Wants=systemd-networkd.service

[Service]
Type=simple
ExecStart=/bin/sh /root/netguest.sh

[Install]
WantedBy=multi-user.target
UNIT
systemctl enable systemd-networkd.service netguest.service > /dev/null 2>&1 ||
    fail "the units were not enabled"
for unit in ssh.service networking.service unattended-upgrades.service apt-daily.timer \
    apt-daily-upgrade.timer systemd-networkd-wait-online.service; do
    systemctl disable "$unit" > /dev/null 2>&1
done

# Every later boot is the same: no cloud-init, and no wait in GRUB's menu.
touch /etc/cloud/cloud-init.disabled || fail "cloud-init was not turned off"
if grep -q -s '^GRUB_TIMEOUT=' /etc/default/grub; then
    sed -i 's/^GRUB_TIMEOUT=.*/GRUB_TIMEOUT=0/' /etc/default/grub /etc/default/grub.d/*.cfg 2> /dev/null
    update-grub > /dev/null 2>&1
fi

say "RECORD system Debian $(cat /etc/debian_version), kernel $(uname -r)"
say "RECORD nft $(nft --version 2> /dev/null)"
say "RECORD cc $(command -v cc || echo absent)"
say "RECORD console $(cat /sys/class/tty/console/active 2> /dev/null || echo unknown)"
sync
say "PREPARED"
systemctl poweroff
