# busybox-runit(8)

NAME: busybox-runit - the runit supervision tools: the BusyBox applets of its `runit` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: The runit supervision tools; nothing on the images uses them; they run as BusyBox builds them.


The usage text BusyBox carries, per applet:


## chpst

```
"[-vP012] [-u USER[:GRP]] [-U USER[:GRP]] [-e DIR]\n"
      "	[-/ DIR] [-n NICE] [-m BYTES] [-d BYTES] [-o N]\n"
      "	[-p N] [-f BYTES] [-c BYTES] PROG ARGS"
#define chpst_full_usage "\n\n"
      "Change the process state, run PROG\n"
    "\n	-u USER[:GRP]	Set uid and gid"
    "\n	-U USER[:GRP]	Set $UID and $GID in environment"
    "\n	-e DIR		Set environment variables as specified by files"
    "\n			in DIR: file=1st_line_of_file"
    "\n	-/ DIR		Chroot to DIR"
    "\n	-n NICE		Add NICE to nice value"
    "\n	-m BYTES	Same as -d BYTES -s BYTES -l BYTES"
    "\n	-d BYTES	Limit data segment"
    "\n	-o N		Limit number of open files per process"
    "\n	-p N		Limit number of processes per uid"
    "\n	-f BYTES	Limit output file sizes"
    "\n	-c BYTES	Limit core file size"
    "\n	-v		Verbose"
    "\n	-P		Create new process group"
    "\n	-0		Close stdin"
    "\n	-1		Close stdout"
    "\n	-2		Close stderr"
#define envdir_trivial_usage
      "DIR PROG ARGS"
#define envdir_full_usage "\n\n"
      "Set various environment variables as specified by files\n"
      "in the directory DIR, run PROG"
#define envuidgid_trivial_usage
      "USER PROG ARGS"
#define envuidgid_full_usage "\n\n"
      "Set $UID to USER's uid and $GID to USER's gid, run PROG"
#define setuidgid_trivial_usage
      "USER PROG ARGS"
#define setuidgid_full_usage "\n\n"
      "Set uid and gid to USER's uid and gid, drop supplementary group ids,\n"
      "run PROG"
#define softlimit_trivial_usage
      "[-a BYTES] [-m BYTES] [-d BYTES] [-s BYTES] [-l BYTES]\n"
      "	[-f BYTES] [-c BYTES] [-r BYTES] [-o N] [-p N] [-t N]\n"
      "	PROG ARGS"
#define softlimit_full_usage "\n\n"
      "Set soft resource limits, then run PROG\n"
    "\n	-a BYTES	Limit total size of all segments"
    "\n	-m BYTES	Same as -d BYTES -s BYTES -l BYTES -a BYTES"
    "\n	-d BYTES	Limit data segment"
    "\n	-s BYTES	Limit stack segment"
    "\n	-l BYTES	Limit locked memory size"
    "\n	-o N		Limit number of open files per process"
    "\n	-p N		Limit number of processes per uid"
    "\nOptions controlling file sizes:"
    "\n	-f BYTES	Limit output file sizes"
    "\n	-c BYTES	Limit core file size"
    "\nEfficiency opts:"
    "\n	-r BYTES	Limit resident set size"
    "\n	-t N		Limit CPU time, process receives"
    "\n			a SIGXCPU after N seconds"
```


## envdir

```
(no usage text; state the applet by its BusyBox documentation)
```


## envuidgid

```
(no usage text; state the applet by its BusyBox documentation)
```


## runsv

```
"DIR"
#define runsv_full_usage "\n\n"
      "Start and monitor a service and optionally an appendant log service"
```


## runsvdir

```
"[-P] [-s SCRIPT] DIR"
#define runsvdir_full_usage "\n\n"
      "Start a runsv process for each subdirectory. If it exits, restart it.\n"
    "\n	-P		Put each runsv in a new session"
    "\n	-s SCRIPT	Run SCRIPT <signo> after signal is processed"
```


## setuidgid

```
(no usage text; state the applet by its BusyBox documentation)
```


## softlimit

```
(no usage text; state the applet by its BusyBox documentation)
```


## sv

```
"[-v] [-w SEC] CMD SERVICE_DIR..."
#define sv_full_usage "\n\n"
      "Control services monitored by runsv supervisor.\n"
      "Commands (only first character is enough):\n"
      "\n"
      "status: query service status\n"
      "up: if service isn't running, start it. If service stops, restart it\n"
      "once: like 'up', but if service stops, don't restart it\n"
      "down: send TERM and CONT signals. If ./run exits, start ./finish\n"
      "	if it exists. After it stops, don't restart service\n"
      "exit: send TERM and CONT signals to service and log service. If they exit,\n"
      "	runsv exits too\n"
      "pause, cont, hup, alarm, interrupt, quit, 1, 2, term, kill: send\n"
      "STOP, CONT, HUP, ALRM, INT, QUIT, USR1, USR2, TERM, KILL signal to service"
```


## svc

```
"[-udopchaitkx] SERVICE_DIR..."
#define svc_full_usage "\n\n"
      "Control services monitored by runsv supervisor"
  "\n"
  "\n""	-u	If service is not running, start it; restart if it stops"
  "\n""	-d	If service is running, send TERM+CONT signals; do not restart it"
  "\n""	-o	Once: if service is not running, start it; do not restart it"
  "\n""	-pchaitk Send STOP, CONT, HUP, ALRM, INT, TERM, KILL signal to service"
  "\n""	-x	Exit: runsv will exit as soon as the service is down"
```


## svlogd

```
"[-tttv] [-r C] [-R CHARS] [-l MATCHLEN] [-b BUFLEN] DIR..."
#define svlogd_full_usage "\n\n"
      "Read log data from stdin and write to rotated log files in DIRs"
  "\n"
  "\n""	-r C	Replace non-printable characters with C"
  "\n""	-R CHARS Also replace CHARS with C (default _)"
  "\n""	-t	Timestamp with @tai64n"
  "\n""	-tt	Timestamp with yyyy-mm-dd_hh:mm:ss.sssss"
  "\n""	-ttt	Timestamp with yyyy-mm-ddThh:mm:ss.sssss"
  "\n""	-v	Verbose"
  "\n"
  "\n""DIR/config file modifies behavior:"
  "\n""sSIZE - when to rotate logs (default 1000000, 0 disables)"
  "\n""nNUM - number of files to retain"
```


## svok

```
"SERVICE_DIR"
#define svok_full_usage "\n\n"
      "Check whether runsv supervisor is running.\n"
      "Exit code is 0 if it does, 100 if it does not,\n"
      "111 (with error message) if SERVICE_DIR does not exist."
```
