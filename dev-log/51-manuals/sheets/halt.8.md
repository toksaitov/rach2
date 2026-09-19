# halt(8)

NAME: halt, poweroff, reboot - stop or restart the machine. Source: utils/halt.c header; BusyBox init/halt.c usage for the applets.
* This project's `halt [-p|-r]` (os-simple): power off by default and with `-p`, restart with `-r`; root's, since reboot(2) is. Nothing is torn down first: the kernel flushes the page cache and clears the dirty-mount flag inside the call, with scheduling already off. `usage: halt [-p|-r]` otherwise.
* On os-real the names are BusyBox's applets `halt`, `poweroff` and `reboot`, dispatched by the standalone shell before the file system, which is why the project's program is not staged there; their options as the usage text says; they signal init (SIGUSR1 halts, SIGUSR2 powers off, SIGTERM restarts, BusyBox's mapping, which init(8) honours) or call reboot(2) directly with `-f`.
* EXAMPLE: `halt`; `tests/run shutdown` reads the machine gone and the disk's flag clean.
* SEE ALSO: reboot(2), init(8), busybox-init(8).
