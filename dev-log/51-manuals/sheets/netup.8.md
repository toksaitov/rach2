# netup(8)

NAME: netup - bring the interfaces up from /etc/network/interfaces. Source: utils/netup.c header, utils/netcfg.c, utils/Readme.md.
* `netup [-a] [INTERFACE ...]`. Reads `/etc/network/interfaces` in the `ifup` format: `auto NAME`, `allow-hotplug NAME`, `iface NAME inet dhcp`, `iface NAME inet static` with `address`, `netmask` (required) and `gateway` (optional). With no operands or `-a`, every interface `auto` or `allow-hotplug` names; a named interface needs neither. A dhcp stanza runs dhcpc(8), which stays in the foreground until its first round; a static stanza is validated whole before any ioctl changes the interface, and a live or incomplete DHCP claim blocks it. `usage: netup [-a] [interface ...]`.
* init-metal runs the same job on a `rach2.ip=none` boot; os-real uses BusyBox's `ifup`.
* FILES: /etc/network/interfaces.
* EXAMPLE: `netup -a`; `tests/run netif` runs both stanzas.
* SEE ALSO: dhcpc(8), busybox-networking(1), recovery(7).
