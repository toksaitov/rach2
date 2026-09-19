# ping(1)

NAME: ping - send ICMP echo requests. Source: utils/ping.c header, utils/Readme.md "Network programs", utils/resolv.c.
* `ping [HOST]`; HOST an IPv4 address or a name (resolved through `/etc/hosts` then one DNS A query at the first `nameserver` of `/etc/resolv.conf`, `10.0.2.3` by default, about six seconds of retries); the default is `10.0.2.2`, QEMU's gateway. Sends four requests through the kernel's private `SC_PING` socket operation (ping(2)), a direct socketcall since musl has no wrapper, and prints the round trip in ticks of 10 ms, or a timeout; succeeds when at least one reply came. The kernel permits one request in flight for the whole machine: a second ping running is `EBUSY`, reported as a busy request and not as a timeout. Root is not needed.
* On os-real this program is `/bin/ping`; BusyBox's ping applet is not built.
* EXAMPLE: `ping 10.0.2.2`; `tests/run ping` runs it.
* SEE ALSO: ping(2), nc(1), busybox-networking(1).
