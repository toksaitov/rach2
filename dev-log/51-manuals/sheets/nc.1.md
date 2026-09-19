# nc(1)

NAME: nc - a TCP client. Source: utils/nc.c header, utils/Readme.md; BusyBox's nc applet usage (networking/nc.c).
* This project's `nc HOST PORT` (os-simple): connects over TCP, relays one chunk of standard input to the socket and prints one reply chunk, half duplex, until end of input or an error; the host as ping(1) resolves it; the port a positive decimal prefix narrowed to 16 bits (not strictly checked; `nc: invalid port` for no digits). `usage: nc <host> <port>` for missing operands.
* On os-real `nc` is BusyBox's applet (its usage from the source: listen mode, `-e`, delays as built); say which image has which.
* EXAMPLE: `nc 10.0.2.2 80`; `tests/run socket` and `tcp` use the project's; the suites' hostpeer answers it.
* SEE ALSO: ping(1), busybox-networking(1), socketcall(2).
* Found at the proofreading: BusyBox's -n suppresses only the reverse lookup in verbose reports, host operands are still resolved; no suite runs nc (tests/run socket runs udptest and tcptest).
