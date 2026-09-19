# busybox-shell(1)

NAME: busybox-shell - the shells: the BusyBox applets of its `shell` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: `sh` and `ash` are sh(1); `hush` is built as well; `cttyhack` gives a shell a controlling terminal.


The usage text BusyBox carries, per applet:


## ash

```
"[-il] [-|+Cabefmnuvx] [-|+o OPT]... [-c 'SCRIPT' [ARG0 ARGS] | FILE ARGS | -s ARGS]"
```


## cttyhack

```
"[PROG ARGS]"
#define cttyhack_full_usage "\n\n"
      "Give PROG a controlling tty if possible."
    "\nExample for /etc/inittab (for busybox init):"
    "\n	::respawn:/bin/cttyhack /bin/sh"
    "\nGiving controlling tty to shell running with PID 1:"
    "\n	$ exec cttyhack sh"
    "\nStarting interactive shell from boot shell script:"
    "\n	setsid cttyhack sh"
```


## hush

```
"[-enxl] [-c 'SCRIPT' [ARG0 ARGS] | FILE ARGS | -s ARGS]"
#define hush_full_usage "\n\n"
"Unix shell interpreter"
```


## sh

```
(no usage text; state the applet by its BusyBox documentation)
```
