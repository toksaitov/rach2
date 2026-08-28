# Milestone 32: resume state

Phase: 4, the interface. Items 1 and 2 committed; item 3 ready to commit.

Done: `tx` returns a verdict and the interface counts what each layer refused,
so `/proc/net/dev` prints six real counters instead of two. `stop` silences a
card at shutdown and after a bring-up that failed with DMA already enabled.
`link` reports the link, and `IFF_RUNNING` now means the link is up rather
than a device being bound.

Evidence: 43 mutations proved across five scripts; new `tests/host/netdev_hosttest.c`
covers the probe walk; `procfiles` asserts the sixteen `/proc/net/dev` fields
and `ifconfig` reporting RUNNING; `netconsole` asserts the shutdown wire
contract with a new `pcapcheck --reject`. Suites passing: style, host, netif,
procfiles, default, shutdown, reset, netconsole.

Two defects found and fixed along the way: `k_puts` never reached serial, so a
netconsole reader was never told the machine was stopping; and the card really
was transmitting through `power_stop`, which a capture now shows it no longer
does.

Next: phase 5, the scripts and documents.
