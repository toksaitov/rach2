# dhcpc(8)

NAME: dhcpc - the DHCP client of the images without BusyBox. Source: utils/dhcpc.c header, utils/Readme.md "Network programs"; utils/dhcpmsg.c, ipcheck.c.
* `dhcpc [-i INTERFACE] [-b] [-l SECONDS]`; `dhcpc -i INTERFACE -k` stops the recorded client. The interface defaults to `eth0`. A packet socket until an address is held, the lease validated whole, the address, mask, route and `/etc/resolv.conf` applied; renewal at half the lease (T1), rebinding at seven eighths (T2), the address removed at expiry or on DHCPNAK, and discovery again. Leases under 10 seconds ignored; longer than a week, or than `-l`, capped. `-b` backgrounds after the first four-request discovery round, configured or not. One holder per interface in `/var/run/dhcpc.INTERFACE.pid` (pid and address, made with O_EXCL; a stale file removed only when the interface still has that address); no `fcntl` locks. There is no configure-once mode: the client holds the lease for as long as it runs.
* os-real takes its address with BusyBox's `udhcpc` instead (busybox-networking(1)); the two use the same kernel interface.
* FILES: /var/run/dhcpc.INTERFACE.pid, /etc/resolv.conf.
* EXAMPLE: `dhcpc -b`; `tests/run dhcp` runs the lease, renewal and expiry.
* SEE ALSO: netup(8), busybox-networking(1).
