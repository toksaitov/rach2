# busybox-klibc-utils(1)

NAME: busybox-klibc-utils - resume: the BusyBox applets of its `klibc-utils` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: `resume`: for swap, which does not exist here.


The usage text BusyBox carries, per applet:


## resume

```
"BLOCKDEV [OFFSET]"
#define resume_full_usage "\n"
  "\n""Restore system state from 'suspend-to-disk' data in BLOCKDEV"
```
