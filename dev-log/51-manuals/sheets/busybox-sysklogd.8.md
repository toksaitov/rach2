# busybox-sysklogd(8)

NAME: busybox-sysklogd - the log tools: the BusyBox applets of its `sysklogd` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: `syslogd`, `klogd`, `logger`, `logread`: the kernel's own log is /dev/bootlog (bootlog(4)); `klogd` reads /proc/kmsg, which does not exist, so it has nothing to read here.


The usage text BusyBox carries, per applet:


## klogd

```
"[-c N] [-n]"
#define klogd_full_usage "\n\n"
      "Log kernel messages to syslog\n"
    "\n	-c N	Print to console messages more urgent than prio N (1-8)"
    "\n	-n	Run in foreground"
```


## logger

```
"[-s] [-t TAG] [-p PRIO] [MESSAGE]"
#define logger_full_usage "\n\n"
      "Write MESSAGE (or stdin) to syslog\n"
    "\n	-s	Log to stderr as well as the system log"
    "\n	-t TAG	Log using the specified tag (defaults to user name)"
    "\n	-p PRIO	Priority (number or FACILITY.LEVEL pair)"
#define logger_example_usage
      "$ logger \"hello\"\n"
```


## logread

```
"[-fF]"
#define logread_full_usage "\n\n"
      "Show messages in syslogd's circular buffer\n"
    "\n	-f	Output data as log grows"
    "\n	-F	Same as -f, but dump buffer first"
```


## syslogd

```
"[OPTIONS]"
#define syslogd_full_usage "\n\n"
      "System logging utility\n"
IF_NOT_FEATURE_SYSLOGD_CFG(
      "(this version of syslogd ignores /etc/syslog.conf)\n"
)
    "\n	-n		Run in foreground"
IF_FEATURE_REMOTE_LOG(
    "\n	-R HOST[:PORT]	Log to HOST:PORT (default PORT:514)"
    "\n	-L		Log locally and via network (default is network only if -R)"
)
IF_FEATURE_IPC_SYSLOG(
```
