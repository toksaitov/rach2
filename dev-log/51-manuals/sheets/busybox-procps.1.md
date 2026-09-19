# busybox-procps(1)

NAME: busybox-procps - the process tools: the BusyBox applets of its `procps` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: `ps` and `top` read /proc/PID/stat and cmdline; `free` reads /proc/meminfo; `uptime` /proc/uptime; `kill`, `killall`, `pkill` send signals through kill(2); no `/proc/PID/status` or `maps`; `sysctl` has no /proc/sys to read.


The usage text BusyBox carries, per applet:


## free

```
"" IF_DESKTOP("[-bkmgh]")
#define free_full_usage "\n\n"
      "Display free and used memory"
#define free_example_usage
      "$ free\n"
      "              total         used         free       shared      buffers\n"
      "  Mem:       257628       248724         8904        59644        93124\n"
      " Swap:       128516         8404       120112\n"
      "Total:       386144       257128       129016\n"
```


## fuser

```
"[-msk46] [-SIGNAL] FILE or PORT/PROTO"
#define fuser_full_usage "\n\n"
      "Find processes which use FILEs or PORTs\n"
    "\n	-m	Find processes which use same fs as FILEs"
    "\n	-4,-6	Search only IPv4/IPv6 space"
    "\n	-s	Don't display PIDs"
    "\n	-k	Kill found processes"
    "\n	-SIGNAL	Signal to send (default: KILL)"
```


## iostat

```
"[-c] [-d] [-t] [-z] [-k|-m] [ALL|BLOCKDEV...] [INTERVAL [COUNT]]"
#define iostat_full_usage "\n\n"
      "Report CPU and I/O statistics\n"
    "\n	-c	Show CPU utilization"
    "\n	-d	Show device utilization"
    "\n	-t	Print current time"
    "\n	-z	Omit devices with no activity"
    "\n	-k	Use kb/s"
    "\n	-m	Use Mb/s"
```


## kill

```
"[-l] [-SIG] PID..."
#define kill_full_usage "\n\n"
      "Send a signal (default: TERM) to given PIDs\n"
    "\n	-l	List all signal names and numbers"
```


## killall

```
"[-lq] [-SIG] PROCESS_NAME..."
#define killall_full_usage "\n\n"
      "Send a signal (default: TERM) to given processes\n"
    "\n	-l	List all signal names and numbers"
```


## killall5

```
"[-l] [-SIG] [-o PID]..."
#define killall5_full_usage "\n\n"
      "Send a signal (default: TERM) to all processes outside current session\n"
    "\n	-l	List all signal names and numbers"
    "\n	-o PID	Don't signal this PID"
```


## lsof

```
""
#define lsof_full_usage "\n\n"
      "Show all open files"
```


## mpstat

```
"[-A] [-I SUM|CPU|ALL|SCPU] [-u] [-P num|ALL] [INTERVAL [COUNT]]"
#define mpstat_full_usage "\n\n"
      "Per-processor statistics\n"
    "\n	-A			Same as -I ALL -u -P ALL"
    "\n	-I SUM|CPU|ALL|SCPU	Report interrupt statistics"
    "\n	-P num|ALL		Processor to monitor"
    "\n	-u			Report CPU utilization"
```


## nmeter

```
"[-d MSEC] FORMAT_STRING"
#define nmeter_full_usage "\n\n"
      "Monitor system in real time"
    "\n"
    "\n -d MSEC	Milliseconds between updates, default:1000, none:-1"
    "\n"
    "\nFormat specifiers:"
    "\n %Nc or %[cN]	CPU. N - bar size (default 10)"
    "\n		(displays: S:system U:user N:niced D:iowait I:irq i:softirq)"
    "\n %[nINTERFACE]	Network INTERFACE"
    "\n %m		Allocated memory"
    "\n %[md]		Dirty file-backed memory"
    "\n %[mw]		Memory being written to storage"
    "\n %[mf]		Free memory"
    "\n %[mt]		Total memory"
    "\n %s		Allocated swap"
    "\n %f		Number of used file descriptors"
    "\n %Ni		Total/specific IRQ rate"
    "\n %x		Context switch rate"
    "\n %p		Forks"
    "\n %[pn]		# of processes"
    "\n %b		Block io"
    "\n %Nt		Time (with N decimal points)"
    "\n %NT		Zero-based timestamp (with N decimal points)"
    "\n %r		Print <cr> instead of <lf> at EOL"
```


## pgrep

```
"[-flanovx] [-s SID|-P PPID|PATTERN]"
#define pgrep_full_usage "\n\n"
      "Display process(es) selected by regex PATTERN\n"
    "\n	-l	Show command name too"
    "\n	-a	Show command line too"
    "\n	-f	Match against entire command line"
    "\n	-n	Show the newest process only"
    "\n	-o	Show the oldest process only"
    "\n	-v	Negate the match"
    "\n	-x	Match whole name (not substring)"
    "\n	-s	Match session ID (0 for current)"
    "\n	-P	Match parent process ID"
#define pkill_trivial_usage
      "[-l|-SIGNAL] [-xfvnoe] [-s SID|-P PPID|PATTERN]"
#define pkill_full_usage "\n\n"
      "Send signal to processes selected by regex PATTERN\n"
    "\n	-l	List all signals"
    "\n	-x	Match whole name (not substring)"
    "\n	-f	Match against entire command line"
    "\n	-s SID	Match session ID (0 for current)"
    "\n	-P PPID	Match parent process ID"
    "\n	-v	Negate the match"
    "\n	-n	Signal the newest process only"
    "\n	-o	Signal the oldest process only"
    "\n	-e	Display name and PID of the process being killed"
```


## pidof

```
IF_FEATURE_PIDOF_SINGLE("[-s] ")IF_FEATURE_PIDOF_OMIT("[-o PID] ")"[NAME]..."
#define USAGE_PIDOF "\n"
#else
#define pidof_trivial_usage
      "[NAME]..."
#define USAGE_PIDOF /* none */
#endif
#define pidof_full_usage "\n\n"
      "List PIDs of all processes with names that match NAMEs"
USAGE_PIDOF
IF_FEATURE_PIDOF_SINGLE(
    "\n	-s	Show only one PID"
)
IF_FEATURE_PIDOF_OMIT(
    "\n	-o PID	Omit given pid"
    "\n		Use %PPID to omit pid of pidof's parent"
)
#define pidof_example_usage
      "$ pidof init\n"
      "1\n"
IF_FEATURE_PIDOF_OMIT(
      "$ pidof /bin/sh\n20351 5973 5950\n")
IF_FEATURE_PIDOF_OMIT(
      "$ pidof /bin/sh -o %PPID\n20351 5950")
```


## pkill

```
(no usage text; state the applet by its BusyBox documentation)
```


## pmap

```
"[-xq] PID..."
#define pmap_full_usage "\n\n"
      "Display process memory usage"
    "\n"
    "\n	-x	Show details"
    "\n	-q	Quiet"
```


## powertop

```
""
#define powertop_full_usage "\n\n"
      "Analyze power consumption on Intel-based laptops"
```


## ps

```
"[-o COL1,COL2=HEADER]" IF_FEATURE_SHOW_THREADS(" [-T]")
#define ps_full_usage "\n\n"
      "Show list of processes\n"
    "\n	-o COL1,COL2=HEADER	Select columns for display"
IF_FEATURE_SHOW_THREADS(
    "\n	-T			Show threads"
)
#else /* !ENABLE_DESKTOP */
#if !ENABLE_SELINUX && !ENABLE_FEATURE_PS_WIDE
#define USAGE_PS "\nThis version of ps accepts no options"
#else
#define USAGE_PS ""
#endif
#define ps_trivial_usage
      ""
#define ps_full_usage "\n\n"
      "Show list of processes\n"
USAGE_PS
IF_SELINUX(
    "\n	-Z	Show selinux context"
)
IF_FEATURE_PS_WIDE(
    "\n	w	Wide output"
)
IF_FEATURE_PS_LONG(
    "\n	l	Long output"
)
IF_FEATURE_SHOW_THREADS(
    "\n	T	Show threads"
)
#endif /* ENABLE_DESKTOP */
#define ps_example_usage
      "$ ps\n"
      "  PID  Uid      Gid State Command\n"
      "    1 root     root     S init\n"
      "    2 root     root     S [kflushd]\n"
      "    3 root     root     S [kupdate]\n"
      "    4 root     root     S [kpiod]\n"
      "    5 root     root     S [kswapd]\n"
      "  742 andersen andersen S [bash]\n"
      "  743 andersen andersen S -bash\n"
      "  745 root     root     S [getty]\n"
      " 2990 andersen andersen R ps\n"
```


## pstree

```
"[-p] [PID|USER]"
#define pstree_full_usage "\n\n"
      "Display process tree, optionally start from USER or PID\n"
    "\n	-p	Show pids"
```


## pwdx

```
"PID..."
#define pwdx_full_usage "\n\n"
      "Show current directory for PIDs"
```


## smemcap

```
(no usage text; state the applet by its BusyBox documentation)
```


## sysctl

```
"[-enq] { -a | -p [FILE]... | [-w] [KEY[=VALUE]]... }"
#define sysctl_full_usage "\n\n"
      "Show/set kernel parameters\n"
    "\n	-e	Don't warn about unknown keys"
    "\n	-n	Don't show key names"
    "\n	-q      Quiet"
    "\n	-a	Show all values"
```


## top

```
"[-b"IF_FEATURE_TOPMEM("m")IF_FEATURE_SHOW_THREADS("H")"]"
      " [-n COUNT] [-d SECONDS]"
#define top_full_usage "\n\n"
      "Show a view of process activity in real time."
  "\n""Read the status of all processes from /proc each SECONDS"
  "\n""and show a screenful of them."
  "\n"
IF_FEATURE_TOP_INTERACTIVE(
      "Keys:"
  "\n""	N/M"
               IF_FEATURE_TOP_CPU_USAGE_PERCENTAGE("/P")
               IF_FEATURE_TOP_CPU_USAGE_PERCENTAGE("/T")
          ": " IF_FEATURE_TOPMEM("show CPU usage, ") "sort by pid/mem"
               IF_FEATURE_TOP_CPU_USAGE_PERCENTAGE("/cpu")
               IF_FEATURE_TOP_CPU_USAGE_PERCENTAGE("/time")
IF_FEATURE_TOPMEM(
  "\n""	S: show memory"
)
  "\n""	R: reverse sort"
IF_SHOW_THREADS_OR_TOP_SMP(
  "\n""	"
               IF_FEATURE_SHOW_THREADS("H: toggle threads")
               IF_FEATURE_SHOW_THREADS(IF_FEATURE_TOP_SMP_CPU(", "))
               IF_FEATURE_TOP_SMP_CPU("1: toggle SMP")
)
  "\n""	Q,^C: exit"
  "\n""Options:"
)
  "\n""	-b	Batch mode"
  "\n""	-n N	Exit after N iterations"
  "\n""	-d SEC	Delay between updates"
IF_FEATURE_TOPMEM(
  "\n""	-m	Same as 's' key"
)
IF_FEATURE_SHOW_THREADS(
  "\n""	-H	Show threads"
)
```


## uptime

```
""
#define uptime_full_usage "\n\n"
      "Display the time since the last boot"
#define uptime_example_usage
      "$ uptime\n"
      "  1:55pm  up  2:30, load average: 0.09, 0.04, 0.00\n"
```


## watch

```
"[-n SEC] [-t] PROG ARGS"
#define watch_full_usage "\n\n"
      "Run PROG periodically\n"
    "\n	-n SEC	Period (default 2)"
    "\n	-t	Don't print header"
#define watch_example_usage
      "$ watch date\n"
      "Mon Dec 17 10:31:40 GMT 2000\n"
      "Mon Dec 17 10:31:42 GMT 2000\n"
      "Mon Dec 17 10:31:44 GMT 2000"
```
