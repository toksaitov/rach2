# Milestone 32: resume state

Phase: 3, the host model and the suite. Complete.

Done: `tests/host/e1000_hosttest.c` drives the driver against a software model
of the chip, and 26 mutations each made one named check fail and only that one.
The `dhcp` target, `runtests`, and `ssh.sh` take the card as a parameter,
defaulting to the RTL8139. `tests/run e1000` runs the probe boot, ping, packet,
netif, dgram, netconsole, raw, dhcp, and ssh on the other card, plus three
probe-order boots.

Evidence: `tests/run e1000` PASS in 911s, including 63/63 probes, the SSH
capstone on the e1000, and all three probe-order boots (both cards attached
binds the RTL8139; only the e1000 binds the e1000; an 82574L, which no driver
claims, reports no supported card and still reaches the shell). The default
card path is unchanged: `tests/run default` PASS in 94s, `dhcp` PASS in 315s,
`ssh` PASS in 29s, each confirmed to use rtl8139. `tests/run style` and
`tests/run host` pass. The driver's rings are volatile, and the host test
passes at -O0, -O1, and -O2.

Next: phase 4, the interface. Three changes in order, each with its own
Readme and its own failing-first proof: `tx` returns a verdict, `stop`
quiesces the card, `link` reports the link.
