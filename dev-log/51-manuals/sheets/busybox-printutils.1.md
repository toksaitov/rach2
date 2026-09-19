# busybox-printutils(1)

NAME: busybox-printutils - the print tools: the BusyBox applets of its `printutils` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: `lpd`, `lpq`, `lpr`: no printer here.


The usage text BusyBox carries, per applet:


## lpd

```
"SPOOLDIR [HELPER [ARGS]]"
#define lpd_full_usage "\n\n"
      "SPOOLDIR must contain (symlinks to) device nodes or directories"
    "\nwith names matching print queue names. In the first case, jobs are"
    "\nsent directly to the device. Otherwise each job is stored in queue"
    "\ndirectory and HELPER program is called. Name of file to print"
    "\nis passed in $DATAFILE variable."
    "\nExample:"
    "\n	tcpsvd -E 0 515 softlimit -m 999999 lpd /var/spool ./print"
```


## lpq

```
"[-P queue[@host[:port]]] [-U USERNAME] [-d JOBID]... [-fs]"
#define lpq_full_usage "\n\n"
      "	-P	lp service to connect to (else uses $PRINTER)"
    "\n	-d	Delete jobs"
    "\n	-f	Force any waiting job to be printed"
    "\n	-s	Short display"
```


## lpr

```
"-P queue[@host[:port]] -U USERNAME -J TITLE -Vmh [FILE]..."
```
