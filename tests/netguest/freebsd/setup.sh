#!/bin/sh
#
# setup.sh: the FreeBSD gateway guest's setup, which runs once, as root, at
# the end of the preparing boot. prepare.sh puts it on the seed as nuageinit's
# user-data, after the lines that write the guest's boot script to
# /root/netguest.sh; nuageinit runs a user-data that begins with `#!` from
# rc.d/nuageinit_user_data_script (nuageinit(7)).
#
# It has rc.local run the boot script at every later boot, leaves the
# interfaces to that script, turns off what the gateway does not need,
# nuageinit and the first boot's other work among it, keeps the serial port
# as a console, and powers the guest off. It reports on the console, which
# prepare.sh reads: `NETGUEST RECORD NAME VALUE` lines, then `NETGUEST
# PREPARED` or `NETGUEST FAILED reason`.

say()
{
    echo "NETGUEST $*" > /dev/console
}

fail()
{
    say "FAILED $*"
    exit 1
}

# loader NAME VALUE: set NAME in /boot/loader.conf, once.
loader()
{
    sed -i '' "/^$1=/d" /boot/loader.conf
    echo "$1=\"$2\"" >> /boot/loader.conf
}

[ -s /root/netguest.sh ] || fail "the seed did not write /root/netguest.sh"
chmod 755 /root/netguest.sh
echo '/bin/sh /root/netguest.sh' > /etc/rc.local || fail "/etc/rc.local was not written"

# The image asks DHCP on every interface and waits for it; the inner link
# has no DHCP server, so the boot script configures both interfaces itself.
sysrc -x ifconfig_DEFAULT > /dev/null 2>&1
sysrc nuageinit_enable=NO firstboot_pkg_upgrade_enable=NO growfs_enable=NO \
    sshd_enable=NO sendmail_enable=NONE cron_enable=NO dumpdev=NO \
    > /dev/null || fail "rc.conf was not changed"

# The image's loader has both consoles and no delay; say so again, so that
# the guest does not depend on it.
loader console 'comconsole,vidconsole'
loader autoboot_delay -1
loader beastie_disable YES

# This boot is the first: rc removes the mark only at its end, which the
# power off below comes before, and a boot that found the mark would run
# this script again from nuageinit's cache.
rm -rf /firstboot /firstboot-reboot /var/cache/nuageinit/user_data /var/cache/nuageinit/runcmds
[ ! -e /firstboot ] || fail "/firstboot was not removed"

say "RECORD system $(freebsd-version -k) kernel, $(freebsd-version -u) userland"
say "RECORD pfctl $(command -v pfctl || echo absent)"
say "RECORD cc $(command -v cc || echo absent)"
say "RECORD console $(kenv console 2> /dev/null || echo unknown)"
sync
say "PREPARED"
shutdown -p now
