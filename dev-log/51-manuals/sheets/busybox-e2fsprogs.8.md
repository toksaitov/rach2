# busybox-e2fsprogs(8)

NAME: busybox-e2fsprogs - the file system attribute and check tools: the BusyBox applets of its `e2fsprogs` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: `chattr`, `lsattr`, `fsck`: `fsck` dispatches to `fsck.TYPE`; the machine's ext2 check is the real e2fsprogs `e2fsck` on recovery (e2fsck(8), phase 4 stages it: an adapted page or a short one naming what recovery carries).


The usage text BusyBox carries, per applet:


## chattr

```
"[-R] [-v VERSION] [-p PROJID] [-+=AacDdijsStTu] FILE..."
#define chattr_full_usage "\n\n"
      "Change ext2 file attributes\n"
    "\n	-R	Recurse"
    "\n	-v NUM	Set version/generation number"
    "\n	-p NUM	Set project number"
```


## fsck

```
"[-ANPRTV] [-t FSTYPE] [FS_OPTS] [BLOCKDEV]..."
#define fsck_full_usage "\n\n"
      "Check and repair filesystems\n"
    "\n	-A	Walk /etc/fstab and check all filesystems"
    "\n	-N	Don't execute, just show what would be done"
    "\n	-P	With -A, check filesystems in parallel"
    "\n	-R	With -A, skip the root filesystem"
    "\n	-T	Don't show title on startup"
    "\n	-V	Verbose"
```


## lsattr

```
"[-Radlpv] [FILE]..."
#define lsattr_full_usage "\n\n"
      "List ext2 file attributes\n"
    "\n	-R	Recurse"
    "\n	-a	Include names starting with ."
    "\n	-d	List directory names, not contents"
```
