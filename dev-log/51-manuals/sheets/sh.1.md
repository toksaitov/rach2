# sh(1)

NAME: sh - the shell, BusyBox ash as built here.

* `/bin/sh` on every image is BusyBox 1.36.1's ash (the same bytes as `/bin/busybox`). Usage: `sh [-il] [-|+Cabefmnuvx] [-|+o OPT]... [-c 'SCRIPT' [ARG0 ARGS] | FILE ARGS | -s ARGS]`. A login shell when `argv[0]` begins with `-`, which login(1) and su(1) arrange; it then reads `/etc/profile` (staged: `export PATH=/bin:/usr/bin`, `HOME` defaulting to `/root`, `cd "$HOME"`).
* Features enabled in this build (vendor/busybox/rach2.config): job control, aliases, `$RANDOM`, prompt expansion, `$TMOUT` idle timeout, mail check, the builtins echo, printf, test, sleep (fractional seconds), help, getopts, command; 64-bit arithmetic with bases; some bash compatibility (`ASH_BASH_COMPAT`); a not-found hook; history with a size limit; embedded scripts. The standalone shell: an applet name is run without a `/bin` entry (`FEATURE_SH_STANDALONE`), which is how most BusyBox tools are reached on os-real, whose `/bin` holds only the programs the manifest stages.
* On this kernel: no threads and no dynamic linking affect nothing the shell does; `ulimit` follows limits(7) (`nproc`, `nofile`, `as` are the three the kernel keeps).
* FILES: /etc/profile.
* EXAMPLE: `sh -c 'echo $((1 + 2))'`; the shell is what every suite types at.
* Adapted from BusyBox's documentation, GPL-2.0. SEE ALSO: login(1), busybox(1) (phase 4), limits(7).
