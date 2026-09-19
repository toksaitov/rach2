# busybox-miscutils(1)

NAME: busybox-miscutils - the miscellaneous tools: the BusyBox applets of its `miscutils` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: `man` has a page of its own (man(1)); `less` is described here, as its alias says; `bc`, `dc`, `strings`, `time`, `crond`, `setserial`, `devmem` (reads physical memory through /dev/mem, which does not exist here: say so), `hexedit`, `tree`, `ascii`, `microcom`. Found at the writing: `makedevs` is built in its table form only (rach2.config), so the page describes that form and not the argument form the usage text also shows.


The usage text BusyBox carries, per applet:


## adjtimex

```
"[-q] [-o OFS] [-f FREQ] [-p TCONST] [-t TICK]"
#define adjtimex_full_usage "\n\n"
      "Read or set kernel time variables. See adjtimex(2)\n"
    "\n	-q	Quiet"
    "\n	-o OFF	Time offset, microseconds"
    "\n	-f FREQ	Frequency adjust, integer kernel units (65536 is 1ppm)"
    "\n	-t TICK	Microseconds per tick, usually 10000"
    "\n		(positive -t or -f values make clock run faster)"
    "\n	-p TCONST"
```


## ascii

```
(no usage text; state the applet by its BusyBox documentation)
```


## bc

```
"[-sqlw] [FILE]..."
#define bc_full_usage "\n"
    "\nArbitrary precision calculator"
    "\n"
```


## chat

```
"EXPECT [SEND [EXPECT [SEND]]...]"
#define chat_full_usage "\n\n"
      "Useful for interacting with a modem connected to stdin/stdout.\n"
      "A script consists of \"expect-send\" argument pairs.\n"
      "Example:\n"
      "chat '' ATZ OK ATD123456 CONNECT '' ogin: pppuser word: ppppass '~'"
```


## crond

```
"[-fbS] [-l N] " IF_FEATURE_CROND_D("[-d N] ") "[-L LOGFILE] [-c DIR]"
#define crond_full_usage "\n\n"
      "	-f	Foreground"
    "\n	-b	Background (default)"
    "\n	-S	Log to syslog (default)"
    "\n	-l N	Set log level. Most verbose 0, default 8"
IF_FEATURE_CROND_D(
    "\n	-d N	Set log level, log to stderr"
)
    "\n	-L FILE	Log to FILE"
    "\n	-c DIR	Cron dir. Default:"CONFIG_FEATURE_CROND_DIR"/crontabs"
```


## dc

```
IF_FEATURE_DC_BIG("[-x] ")"[-eSCRIPT]... [-fFILE]... [FILE]..."
#define dc_full_usage "\n"
    "\nTiny RPN calculator. Operations:"
    "\nArithmetic: + - * / % ^"
IF_FEATURE_DC_BIG(
    "\n~ - divide with remainder"
    "\n| - modular exponentiation"
    "\nv - square root"
```


## devmem

```
"ADDRESS [WIDTH [VALUE]]"
#define devmem_full_usage "\n\n"
      "Read/write from physical address\n"
    "\n	ADDRESS	Address to act upon"
    "\n	WIDTH	Width (8/16/...)"
    "\n	VALUE	Data to be written"
```


## hexedit

```
"FILE"
#define hexedit_full_usage "\n\n"
"Edit FILE in hexadecimal"
```


## less

```
"[-EF" IF_FEATURE_LESS_REGEXP("I")IF_FEATURE_LESS_FLAGS("Mm")
      "N" IF_FEATURE_LESS_TRUNCATE("S") IF_FEATURE_LESS_RAW("R") "h~] [FILE]..."
#define less_full_usage "\n\n"
      "View FILE (or stdin) one screenful at a time\n"
    "\n	-E	Quit once the end of a file is reached"
    "\n	-F	Quit if entire file fits on first screen"
IF_FEATURE_LESS_REGEXP(
    "\n	-I	Ignore case in all searches"
)
IF_FEATURE_LESS_FLAGS(
    "\n	-M,-m	Display status line with line numbers"
    "\n		and percentage through the file"
)
    "\n	-N	Prefix line number to each line"
IF_FEATURE_LESS_TRUNCATE(
    "\n	-S	Truncate long lines"
)
IF_FEATURE_LESS_RAW(
    "\n	-R	Remove color escape codes in input"
)
    "\n	-~	Suppress ~s displayed past EOF"
```


## lsscsi

```
(no usage text; state the applet by its BusyBox documentation)
```


## makedevs

```
"NAME TYPE MAJOR MINOR FIRST LAST [s]"
#define makedevs_full_usage "\n\n"
      "Create a range of block or character special files"
    "\n"
    "\nTYPE is:"
    "\n	b	Block device"
    "\n	c	Character device"
    "\n	f	FIFO, MAJOR and MINOR are ignored"
    "\n"
    "\nFIRST..LAST specify numbers appended to NAME."
    "\nIf 's' is the last argument, the base device is created as well."
    "\n"
    "\nExamples:"
    "\n	makedevs /dev/ttyS c 4 66 2 63   ->  ttyS2-ttyS63"
    "\n	makedevs /dev/hda b 3 0 0 8 s    ->  hda,hda1-hda8"
#define makedevs_example_usage
      "# makedevs /dev/ttyS c 4 66 2 63\n"
      "[creates ttyS2-ttyS63]\n"
      "# makedevs /dev/hda b 3 0 0 8 s\n"
      "[creates hda,hda1-hda8]\n"
#endif
#if ENABLE_FEATURE_MAKEDEVS_TABLE
#define makedevs_trivial_usage
      "[-d device_table] rootdir"
#define makedevs_full_usage "\n\n"
      "Create a range of special files as specified in a device table.\n"
      "Device table entries take the form of:\n"
      "<name> <type> <mode> <uid> <gid> <major> <minor> <start> <inc> <count>\n"
      "Where name is the file name, type can be one of:\n"
      "	f	Regular file\n"
      "	d	Directory\n"
      "	c	Character device\n"
      "	b	Block device\n"
      "	p	Fifo (named pipe)\n"
      "uid is the user id for the target file, gid is the group id for the\n"
      "target file. The rest of the entries (major, minor, etc) apply to\n"
      "to device special files. A '-' may be used for blank entries."
#define makedevs_example_usage
      "For example:\n"
      "<name>    <type> <mode><uid><gid><major><minor><start><inc><count>\n"
      "/dev         d   755    0    0    -      -      -      -    -\n"
      "/dev/console c   666    0    0    5      1      -      -    -\n"
      "/dev/null    c   666    0    0    1      3      0      0    -\n"
      "/dev/zero    c   666    0    0    1      5      0      0    -\n"
      "/dev/hda     b   640    0    0    3      0      0      0    -\n"
      "/dev/hda     b   640    0    0    3      1      1      1    15\n\n"
      "Will Produce:\n"
      "/dev\n"
      "/dev/console\n"
      "/dev/null\n"
      "/dev/zero\n"
      "/dev/hda\n"
      "/dev/hda[0-15]\n"
#endif
```


## man

```
"[-aw] [SECTION] MANPAGE[.SECTION]..."
#define man_full_usage "\n\n"
      "Display manual page\n"
    "\n	-a	Display all pages"
    "\n	-w	Show page locations"
    "\n"
    "\n$COLUMNS overrides output width"
```


## microcom

```
"[-d DELAY_MS] [-t TIMEOUT_MS ] [-s SPEED] [-X] TTY"
#define microcom_full_usage "\n\n"
      "Copy bytes from stdin to TTY and from TTY to stdout\n"
    "\n	-d DELAY	Wait up to DELAY ms for TTY output before sending"
    "\n			every next byte to it"
    "\n	-t TIMEOUT	Exit if both stdin and TTY are silent for TIMEOUT ms"
    "\n	-s SPEED	Set serial line to SPEED"
    "\n	-X		Disable special meaning of NUL and Ctrl-X from stdin"
```


## mim

```
"[-f FILE] [SHELL_OPTIONS] [TARGET] ..."
#define mim_full_usage "\n\n"
"Run a script from a Makefile-like specification file\n"
    "\n	-f FILE		Spec file (default Mimfile)"
```


## mt

```
"[-f DEVICE] OPCODE VALUE"
#define mt_full_usage "\n\n"
      "Control magnetic tape drive operation\n"
      "\n"
      "Opcodes:\n"
      "\n"
      "bsf bsfm bsr bss datacompression drvbuffer eof eom erase\n"
      "fsf fsfm fsr fss load lock mkpart nop offline ras1 ras2\n"
      "ras3 reset retension rewind rewoffline seek setblk setdensity\n"
      "setpart tell unload unlock weof wset"
```


## readahead

```
"[FILE]..."
#define readahead_full_usage "\n\n"
      "Preload FILEs to RAM"
```


## runlevel

```
"[FILE]"
#define runlevel_full_usage "\n\n"
      "Find the current and previous system runlevel\n"
      "\n"
      "If no utmp FILE exists or if no runlevel record can be found,\n"
      "print \"unknown\""
#define runlevel_example_usage
      "$ runlevel /var/run/utmp\n"
      "N 2"
```


## rx

```
"FILE"
#define rx_full_usage "\n\n"
      "Receive a file using the xmodem protocol"
#define rx_example_usage
      "$ rx /tmp/foo\n"
```


## setfattr

```
"[-h] -n|-x ATTR [-v VALUE] FILE..."
#define setfattr_full_usage "\n\n"
      "Set extended attributes"
    "\n"
    "\n	-h		Do not follow symlinks"
    "\n	-x ATTR		Remove attribute ATTR"
    "\n	-n ATTR		Set attribute ATTR to VALUE"
    "\n	-v VALUE	(default: empty)"
```


## setserial

```
"[-abGvz] { DEVICE [PARAMETER [ARG]]... | -g DEVICE... }"
#define setserial_full_usage "\n\n"
"Print or set serial port parameters"
  "\n"
  "\n""	-a	Print all"
  "\n""	-b	Print summary"
  "\n""	-G	Print as setserial PARAMETERs"
  "\n""	-v	Verbose"
  "\n""	-z	Zero out serial flags before setting"
  "\n""	-g	All args are device names"
  "\n"
  "\n""PARAMETERs: (* = takes ARG, ^ = can be turned off by preceding ^)"
  "\n""	*port, *irq, *divisor, *uart, *baud_base, *close_delay, *closing_wait,"
  "\n""	^fourport, ^auto_irq, ^skip_test, ^sak, ^session_lockout, ^pgrp_lockout,"
  "\n""	^callout_nohup, ^split_termios, ^hup_notify, ^low_latency, autoconfig,"
  "\n""	spd_normal, spd_hi, spd_vhi, spd_shi, spd_warp, spd_cust"
  "\n""ARG for uart:"
  "\n""	unknown, 8250, 16450, 16550, 16550A, Cirrus, 16650, 16650V2, 16750,"
  "\n""	16950, 16954, 16654, 16850, RSA, NS16550A, XSCALE, RM9000, OCTEON, AR7,"
  "\n""	U6_16550A"
```


## strings

```
"[-fo] [-t o|d|x] [-n LEN] [FILE]..."
#define strings_full_usage "\n\n"
      "Display printable strings in a binary file\n"
```


## time

```
"[-vpa] [-o FILE] PROG ARGS"
#define time_full_usage "\n\n"
      "Run PROG, display resource usage when it exits\n"
    "\n	-v	Verbose"
    "\n	-p	POSIX output format"
    "\n	-f FMT	Custom format"
    "\n	-o FILE	Write result to FILE"
    "\n	-a	Append (else overwrite)"
```


## tree

```
(no usage text; state the applet by its BusyBox documentation)
```


## ts

```
"[-is] [STRFTIME]"
#define ts_full_usage "\n\n"
      "Pipe stdin to stdout, add timestamp to each line\n"
    "\n	-s	Time since start"
    "\n	-i	Time since previous line"
```


## ttysize

```
"[w] [h]"
#define ttysize_full_usage "\n\n"
      "Print dimensions of stdin tty, or 80x24"
```


## volname

```
"[DEVICE]"
#define volname_full_usage "\n\n"
      "Show CD volume name of the DEVICE (default /dev/cdrom)"
```
