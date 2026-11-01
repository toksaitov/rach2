**Fable review, round three, against origin/master 2dd8dd1b7.** Kernel paths start at `rach2/sys/`.

**1. IPv6**

1. 18a's Done cannot pass inside 18a. Pinging the host's link-local address needs neighbour resolution, which is 18b's, and `ping -6` needs raw ICMPv6 sockets, which are 18c's; the in-house ping (`utils/programs.mk:14`) has no `-6` (`utils/ping.c:41`), and slirp cannot ping into the guest. Change: 18a owns `raw.c`'s IPv6 side and `utils/ping.c -6`; its Done is `ping -6 ::1`, a fragmented echo over `::1`, and a packet socket seeing EtherType 0x86DD. The host ping moves to 18b's Done.

2. "The drivers' multicast filters" is the e1000 alone: the RTL8139 and RTL8168 already pass every group (`rtl8139.c:161-163`, `rtl8168.c:734-738`), the e1000 clears its table and sets no MPE (`e1000.c:586-587,609`), so QEMU's e1000 drops every RA and NS. One line, `E1000_RCTL_MPE`.

3. The addresses are not "on `netif.c`": they live in `struct netdev` (`netdev.h:163-173`) and are set by netconf.c (`netconf.h:150-201`); netif.c only passes ioctls through (`netif.c:303-305`). The loopback ring carries IPv4 datagrams only (`ip.c:431-432`, drained into `ip_rx`). Add `netdev.h`, `netconf.c` and ip.c's ring to 18a's Owns.

4. The address width is one change across four files, so give it one owner, 18c: `struct socket` (`socket.h:61,81,138,187-190`), the receive reporters (`socket.h:250,305,310`), `struct tcb` and `tcp_row` (`tcp.h:116-119,366-369`), `tcp_find` (`tcp.c:1087,1102`), UDP's bindings (`udp.c:55,184,195`), raw's queue (`raw.c:28,85-88`), and `readv_dgram` in `syscall_fs.c:4699-4733`, which recvmsg uses and which the split list does not give 18c. Also 18c's: TCP_MSS 1460 and the 12-byte pseudo-header buffer (`tcp.h:26`, `tcp.c:59`), UDP's checksum, sent as 0 (`udp.c:248`) but mandatory over IPv6, and `tcp_local_addr_changed` (`tcp.c:1156-1166`) for an address whose lifetime ends.

5. Netlink: only `neigh_dump` reads the request's family (`netlink.c:565`); the address and route dumps answer IPv4 to any family (`:1217-1238`), and inet_diag answers AF_INET6 with nothing (`:1533-1534`). 18d's, but say so.

6. 18c is too large: dual-family TCP and UDP, every socket call, v4-mapped, V6ONLY, the IPV6_* options, raw and ping sockets, two Lenovo proofs. Split off 18e: raw ICMPv6's filter, the options with ancillary data, and the Lenovo ssh and curl proofs. Cut "ICMP datagram sockets": nothing staged uses them (BusyBox has no ping, `rach2.config:960-961`; `ping` is in-house). Cut RDNSS from 18b: the kernel has no consumer for it.

7. 18d's tools are wrong: BusyBox has no `ip` and no `udhcpc6` (`rach2.config:923,1016`); `ip` comes from iproute2 in the GNU root (`vendor/gnu/ports/iproute2.sh`). Either 18d owns `vendor/busybox/rach2.config` and rebuilds BusyBox, or the QEMU proof uses the GNU image. Start-up is `netup` (`utils/netup.c`) and BusyBox `ifup` (`init.c:1566-1586`); `utils/netcfg.c` steps over `inet6` stanzas (`tests/host/netcfg_hosttest.c:139-147`). Name `netup.c`, `netcfg.c` and the three manifests' `/etc/hosts` lines (`os-real.manifest:151`, `os-real-gnu.manifest:159`, `grub-test.manifest:149`) as 18d's.

8. QEMU (11.1.3 on the Mac) proves SLAAC and TCP over IPv6: slirp advertises `fec0::/64` on RS, answers NS, echoes at `fec0::2` and `fe80::2`, and maps `fec0::2` to the host's loopback, so curl to `[fec0::2]:port` works. Slirp neither fragments nor reassembles IPv6, so the fragment case runs over `::1` there and against the Mac on the Lenovo. Nothing needs a tap. The Mac's en0 has only `fd91...` and link-local addresses and no default route, so an IPv6 connect to the Internet fails at once with ENETUNREACH: 18c's claim holds, and the SLAAC proof is "the Mac pings it", as written.

9. The Lenovo steps in 18b, 18c, 18d and 21 need the owner's accounts; mark them `owner-only` as tasks 3 and 20 are, or leave them to the release's acceptance.

**2. term**

10. The cause is as stated: mode 1049 saves and restores only `alt_cursor_x/y` (`tsm-vte.c:1778-1796`; patch 0021 keeps that, lines 160-175), while SGR sets the erase colour (`:1544-1545`, BCE on at `:857`). Two caveats. First, vim resets SGR before leaving: captured on the Mac, `\e[29m\e[m` precede `\e[?1049l`, so restoring attributes alone may not change the owner's case. Second, tsm-screen.c saves `def_attr_main` on entering the alternate screen (`:997`) and never restores it on leaving (`:1024-1030`), a dead store. Change: the task's first subtask captures the bytes term receives in the owner's case (term's `-H` mode), then fixes both, 1049 as DECSC/DECRC with the main screen's slot of patch 0024 and `def_attr` restored from `def_attr_main`.

11. The test cannot run vim: it is a GNU port (`vendor/gnu/ports/vim.sh`), and the termconsole scenario boots the metal disk's system (`machine.termconsole.json`, "the metal disk's system"). Write the bytes with printf and read the grid's colours from the screen dump.

12. The bell setting belongs in `/etc/term.conf`, which term already reads and rewrites (`main.c:155-170,334`; Readme "### /etc/term.conf"), not in vconsole.conf through init: init reads vconsole.conf for the keymap alone (`init.c:339-357`) and passes term only `-l` (`:773-775`). Then init.c is untouched and the share with task 6 disappears.

13. KDMKTONE is right, and the Lenovo's speaker works through KIOCSOUND (`syscall.c:2080-2084`, `speaker.c:25,54`; the owner heard it on 2026-09-25, dev-log 62). The kernel has no timer-callback API; the tone's end is a call from the tick in `rach2/kernel.c:298-302`, which no task owns: give those lines to 21. CSI 10 and 11 `]` need the libtsm patch, since `do_csi` has no `]` case; defaults 750 Hz, 125 ms as Linux.

**3. Now wrong**

14. Line 12 still sets aside "full IPv6 with neighbour discovery", against line 9.

15. The split list says "`procfs.c` ... 18 net files"; 18d.

16. Three release cases assert IPv6's absence and will fail: `guest.packet.json:24` (`packetprobe.c:249,781`, 18a), `machine.procfiles.json:551` (`procfilesmusl.c:208-209`, 18d), `netcfg_hosttest.c:139-147` (18d). Task 14's Done expects `misc_syscalls.py`'s AF_INET6 line to fail; after 18c it passes. `network/Readme.md:123-124` ("IPv4 only", "no fragments") needs an owner: 18d.

17. Queue counts (fourteen start, thirteen wait) and the rest: no objection.