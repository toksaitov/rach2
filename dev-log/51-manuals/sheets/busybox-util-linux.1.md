# busybox-util-linux(1)

NAME: busybox-util-linux - the system utilities: the BusyBox applets of its `util-linux` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: `mount` and `umount` have their own page, mount(8); `blkid` and `findfs` read the disk nodes' volumes (disk(4)); `fdisk` reads and can write a table only on the recovery kernel; `hexdump`, `xxd`, `getopt`, `script`, `setsid`, `flock` work as usual; `swapon`, `swapoff`, `mkswap`, `losetup` are absent or without a kernel to serve them (swap does not exist; the loop device is loopctl(8)); `hwclock` reads the RTC through the kernel's clock; `lspci`, `lsusb` read /proc (say what they find here). Found at the writing: `flock` fails, since musl issues the flock call and this kernel registers no handler for it; `hwclock` needs an RTC device node this kernel does not publish; `lspci` and `lsusb` read /sys/bus/pci/devices and /sys/bus/usb/devices, which do not exist here.


The usage text BusyBox carries, per applet:


## blkid

```
"[BLOCKDEV]..."
#define blkid_full_usage "\n\n"
      "Print UUIDs of all filesystems"
```


## cal

```
"[-jmy] [[MONTH] YEAR]"
#define cal_full_usage "\n\n"
      "Display a calendar\n"
    "\n	-j	Use julian dates"
    "\n	-m	Week starts on Monday"
    "\n	-y	Display the entire year"
```


## chrt

```
"-m | -p [PRIO] PID | [-rfobi] PRIO PROG ARGS"
#define chrt_full_usage "\n\n"
      "Change scheduling priority and class for a process\n"
    "\n	-m	Show min/max priorities"
    "\n	-p	Operate on PID"
    "\n	-r	Set SCHED_RR class"
    "\n	-f	Set SCHED_FIFO class"
    "\n	-o	Set SCHED_OTHER class"
    "\n	-b	Set SCHED_BATCH class"
    "\n	-i	Set SCHED_IDLE class"
#define chrt_example_usage
      "$ chrt -r 4 sleep 900; x=$!\n"
      "$ chrt -f -p 3 $x\n"
      "You need CAP_SYS_NICE privileges to set scheduling attributes of a process"
```


## fallocate

```
"[-o OFS] -l LEN FILE"
```


## fatattr

```
"[-+rhsvda] FILE..."
#define fatattr_full_usage "\n\n"
      "Change file attributes on FAT filesystem\n"
    "\n	-	Clear attributes"
    "\n	+	Set attributes"
    "\n	r	Read only"
    "\n	h	Hidden"
    "\n	s	System"
    "\n	v	Volume label"
    "\n	d	Directory"
    "\n	a	Archive"
```


## fbset

```
"[OPTIONS] [MODE]"
#define fbset_full_usage "\n\n"
      "Show and modify frame buffer settings"
#define fbset_example_usage
      "$ fbset\n"
      "mode \"1024x768-76\"\n"
      "	# D: 78.653 MHz, H: 59.949 kHz, V: 75.694 Hz\n"
      "	geometry 1024 768 1024 768 16\n"
      "	timings 12714 128 32 16 4 128 4\n"
      "	accel false\n"
      "	rgba 5/11,6/5,5/0,0/0\n"
      "endmode\n"
```


## fdflush

```
(no usage text; state the applet by its BusyBox documentation)
```


## fdformat

```
"[-n] DEVICE"
#define fdformat_full_usage "\n\n"
      "Format floppy disk\n"
    "\n	-n	Don't verify after format"
```


## fdisk

```
"[-ul" IF_FEATURE_FDISK_BLKSIZE("s") "] "
      "[-C CYLINDERS] [-H HEADS] [-S SECTORS] [-b SSZ] DISK"
#define fdisk_full_usage "\n\n"
      "Change partition table\n"
    "\n	-u		Start and End are in sectors (instead of cylinders)"
    "\n	-l		Show partition table for each DISK, then exit"
IF_FEATURE_FDISK_BLKSIZE(
    "\n	-s		Show partition sizes in kb for each DISK, then exit"
)
    "\n	-b 2048		(for certain MO disks) use 2048-byte sectors"
    "\n	-C CYLINDERS	Set number of cylinders/heads/sectors"
    "\n	-H HEADS	Typically 255"
    "\n	-S SECTORS	Typically 63"
```


## findfs

```
"LABEL=label or UUID=uuid"
#define findfs_full_usage "\n\n"
      "Find a filesystem device based on a label or UUID"
#define findfs_example_usage
      "$ findfs LABEL=MyDevice"
```


## flock

```
"[-sxun] FD | { FILE [-c] PROG ARGS }"
#define flock_full_usage "\n\n"
      "[Un]lock file descriptor, or lock FILE, run PROG\n"
    "\n	-s	Shared lock"
    "\n	-x	Exclusive lock (default)"
    "\n	-u	Unlock FD"
    "\n	-n	Fail rather than wait"
```


## freeramdisk

```
"DEVICE"
#define freeramdisk_full_usage "\n\n"
      "Free all memory used by the specified ramdisk"
#define freeramdisk_example_usage
      "$ freeramdisk /dev/ram2\n"
#define fdflush_trivial_usage
      "DEVICE"
#define fdflush_full_usage "\n\n"
      "Force floppy disk drive to detect disk change"
```


## fsck.minix

```
(no usage text; state the applet by its BusyBox documentation)
```


## getopt

```
"[OPTIONS] [--] OPTSTRING PARAMS"
#define getopt_full_usage "\n\n"
IF_FEATURE_GETOPT_LONG(
      "	-a		Allow long options starting with single -\n"
      "	-l LOPT[,...]	Long options to recognize\n"
)
      "	-n PROGNAME	The name under which errors are reported"
    "\n	-o OPTSTRING	Short options to recognize"
    "\n	-q		No error messages on unrecognized options"
    "\n	-Q		No normal output"
    "\n	-s SHELL	Set shell quoting conventions"
    "\n	-T		Version test (exits with 4)"
    "\n	-u		Don't quote output"
IF_FEATURE_GETOPT_LONG( /* example uses -l, needs FEATURE_GETOPT_LONG */
    "\n"
    "\nExample:"
    "\n"
    "\nO=`getopt -l bb: -- ab:c:: \"$@\"` || exit 1"
    "\neval set -- \"$O\""
    "\nwhile true; do"
    "\n	case \"$1\" in"
    "\n	-a)	echo A; shift;;"
    "\n	-b|--bb) echo \"B:'$2'\"; shift 2;;"
    "\n	-c)	case \"$2\" in"
    "\n		\"\")	echo C; shift 2;;"
    "\n		*)	echo \"C:'$2'\"; shift 2;;"
    "\n		esac;;"
    "\n	--)	shift; break;;"
    "\n	*)	echo Error; exit 1;;"
    "\n	esac"
    "\ndone"
)
#define getopt_example_usage
      "$ cat getopt.test\n"
      "#!/bin/sh\n"
      "GETOPT=`getopt -o ab:c:: --long a-long,b-long:,c-long:: \\\n"
      "       -n 'example.busybox' -- \"$@\"`\n"
      "if [ $? != 0 ]; then exit 1; fi\n"
      "eval set -- \"$GETOPT\"\n"
      "while true; do\n"
      " case $1 in\n"
      "   -a|--a-long) echo \"Option a\"; shift;;\n"
      "   -b|--b-long) echo \"Option b, argument '$2'\"; shift 2;;\n"
      "   -c|--c-long)\n"
      "     case \"$2\" in\n"
      "       \"\") echo \"Option c, no argument\"; shift 2;;\n"
      "       *)  echo \"Option c, argument '$2'\"; shift 2;;\n"
      "     esac;;\n"
      "   --) shift; break;;\n"
      "   *) echo \"Internal error!\"; exit 1;;\n"
      " esac\n"
      "done\n"
```


## hd

```
"FILE..."
#define hd_full_usage "\n\n"
      "hd is an alias for hexdump -C"
```


## hexdump

```
"[-bcdoxCv] [-e FMT] [-f FMT_FILE] [-n LEN] [-s OFS] [FILE]..."
#define hexdump_full_usage "\n\n"
      "Display FILEs (or stdin) in a user specified format\n"
    "\n	-b		1-byte octal display"
    "\n	-c		1-byte character display"
    "\n	-d		2-byte decimal display"
    "\n	-o		2-byte octal display"
    "\n	-x		2-byte hex display"
    "\n	-C		hex+ASCII 16 bytes per line"
    "\n	-v		Show all (no dup folding)"
    "\n	-e FORMAT_STR	Example: '16/1 \"%02x|\"\"\\n\"'"
    "\n	-f FORMAT_FILE"
```


## hwclock

```
IF_LONG_OPTS(
      "[-swul] [--systz] [-f DEV]"
)
IF_NOT_LONG_OPTS(
      "[-swult] [-f DEV]"
)
#define hwclock_full_usage "\n\n"
      "Show or set hardware clock (RTC)\n"
```


## ipcrm

```
"[-MQS key] [-mqs id]"
#define ipcrm_full_usage "\n\n"
      "Upper-case options MQS remove an object by shmkey value.\n"
      "Lower-case options remove an object by shmid value.\n"
    "\n	-mM	Remove memory segment after last detach"
    "\n	-qQ	Remove message queue"
    "\n	-sS	Remove semaphore"
```


## ipcs

```
"[[-smq] -i SHMID] | [[-asmq] [-tcplu]]"
#define ipcs_full_usage "\n\n"
      "	-i ID	Show specific resource"
    "\nResource specification:"
    "\n	-m	Shared memory segments"
    "\n	-q	Message queues"
    "\n	-s	Semaphore arrays"
    "\n	-a	All (default)"
    "\nOutput format:"
    "\n	-t	Time"
    "\n	-c	Creator"
    "\n	-p	Pid"
    "\n	-l	Limits"
    "\n	-u	Summary"
```


## last

```
""IF_FEATURE_LAST_FANCY("[-HW] [-f FILE]")
#define last_full_usage "\n\n"
      "Show listing of the last users that logged into the system"
IF_FEATURE_LAST_FANCY( "\n"
```


## linux32

```
(no usage text; state the applet by its BusyBox documentation)
```


## linux64

```
(no usage text; state the applet by its BusyBox documentation)
```


## lspci

```
"[-mk]"
#define lspci_full_usage "\n\n"
      "List all PCI devices"
    "\n"
    "\n	-m	Parsable output"
    "\n	-k	Show driver"
```


## lsusb

```
(no usage text; state the applet by its BusyBox documentation)
```


## mesg

```
"[y|n]"
#define mesg_full_usage "\n\n"
      "Control write access to your terminal\n"
      "	y	Allow write access to your terminal\n"
      "	n	Disallow write access to your terminal"
```


## mkfs.minix

```
(no usage text; state the applet by its BusyBox documentation)
```


## mkswap

```
"[-L LBL] BLOCKDEV [KBYTES]"
#define mkswap_full_usage "\n\n"
      "Prepare BLOCKDEV to be used as swap partition\n"
    "\n	-L LBL	Label"
```


## more

```
"[FILE]..."
#define more_full_usage "\n\n"
      "View FILE (or stdin) one screenful at a time"
#define more_example_usage
      "$ dmesg | more\n"
```


## mount

```
"[OPTIONS] [-o OPT] DEVICE NODE"
#define mount_full_usage "\n\n"
      "Mount a filesystem. Filesystem autodetection requires /proc.\n"
    "\n	-a		Mount all filesystems in fstab"
IF_FEATURE_MOUNT_FAKE(
IF_FEATURE_MTAB_SUPPORT(
    "\n	-f		Update /etc/mtab, but don't mount"
)
IF_NOT_FEATURE_MTAB_SUPPORT(
    "\n	-f		Dry run"
)
)
IF_FEATURE_MOUNT_HELPERS(
    "\n	-i		Don't run mount helper"
)
IF_FEATURE_MTAB_SUPPORT(
    "\n	-n		Don't update /etc/mtab"
)
IF_FEATURE_MOUNT_VERBOSE(
    "\n	-v		Verbose"
)
```


## mountpoint

```
"[-q] { [-dn] DIR | -x DEVICE }"
#define mountpoint_full_usage "\n\n"
      "Check if DIR is a mountpoint\n"
    "\n	-q	Quiet"
    "\n	-d	Print major:minor of the filesystem"
    "\n	-n	Print device name of the filesystem"
```


## nologin

```
""
#define nologin_full_usage "\n\n"
"Politely refuse a login"
```


## nsenter

```
"[OPTIONS] [PROG ARGS]"
#define nsenter_full_usage "\n"
    "\n	-t PID		Target process to get namespaces from"
    "\n	-m[FILE]	Enter mount namespace"
    "\n	-u[FILE]	Enter UTS namespace (hostname etc)"
    "\n	-i[FILE]	Enter System V IPC namespace"
    "\n	-n[FILE]	Enter network namespace"
    "\n	-p[FILE]	Enter pid namespace"
    "\n	-U[FILE]	Enter user namespace"
    "\n	-S UID		Set uid in entered namespace"
    "\n	-G GID		Set gid in entered namespace"
IF_LONG_OPTS(
    "\n	--preserve-credentials	Don't touch uids or gids"
)
    "\n	-r[DIR]		Set root directory"
    "\n	-w[DIR]		Set working directory"
    "\n	-F		Don't fork before exec'ing PROG"
```


## pivot_root

```
"NEW_ROOT PUT_OLD"
#define pivot_root_full_usage "\n\n"
      "Move the current root file system to PUT_OLD and make NEW_ROOT\n"
      "the new root file system"
```


## rdate

```
"[-s/-p] HOST"
#define rdate_full_usage "\n\n"
      "Set and print time from HOST using RFC 868\n"
    "\n	-s	Only set system time"
    "\n	-p	Only print time"
```


## rdev

```
""
#define rdev_full_usage "\n\n"
      "Print the device node associated with the filesystem mounted at '/'"
#define rdev_example_usage
      "$ rdev\n"
      "/dev/mtdblock9 /\n"
```


## readprofile

```
"[OPTIONS]"
#define readprofile_full_usage "\n\n"
      "	-m MAPFILE	(Default: /boot/System.map)"
    "\n	-p PROFILE	(Default: /proc/profile)"
    "\n	-M NUM		Set the profiling multiplier to NUM"
    "\n	-i		Print only info about the sampling step"
    "\n	-v		Verbose"
    "\n	-a		Print all symbols, even if count is 0"
    "\n	-b		Print individual histogram-bin counts"
    "\n	-s		Print individual counters within functions"
    "\n	-r		Reset all the counters (root only)"
    "\n	-n		Disable byte order auto-detection"
```


## renice

```
"[-n] PRIORITY [[-p|g|u] ID...]..."
#define renice_full_usage "\n\n"
      "Change scheduling priority of a running process\n"
    "\n	-n	Add PRIORITY to current nice value"
    "\n		Without -n, nice value is set to PRIORITY"
    "\n	-p	Process ids (default)"
    "\n	-g	Process group ids"
    "\n	-u	Process user names"
```


## rev

```
"[FILE]..."
#define rev_full_usage "\n\n"
"Reverse lines of FILE"
```


## rtcwake

```
"[-a | -l | -u] [-d DEV] [-m MODE] [-s SEC | -t TIME]"
#define rtcwake_full_usage "\n\n"
      "Enter a system sleep state until specified wakeup time\n"
    "\n	-a	Read clock mode from "ADJTIME_PATH" (default)"
    "\n	-l	Clock is set to local time"
    "\n	-u	Clock is set to UTC time"
    "\n	-d DEV	Specify the RTC device"
    "\n	-m MODE	Set sleep state (default: standby)"
    "\n	-s SEC	Set timeout in SEC seconds from now"
    "\n	-t TIME	Set timeout to TIME seconds from epoch"
```


## script

```
"[-afq] [-t[FILE]] [-c PROG] [OUTFILE]"
#define script_full_usage "\n\n"
      "Default OUTFILE is 'typescript'"
    "\n"
    "\n	-a	Append output"
    "\n	-c PROG	Run PROG, not shell"
```


## scriptreplay

```
"TIMINGFILE [TYPESCRIPT [DIVISOR]]"
#define scriptreplay_full_usage "\n\n"
      "Play back typescripts, using timing information"
```


## setarch

```
"PERSONALITY [-R] PROG ARGS"
#define setarch_full_usage "\n\n"
      "PERSONALITY may be:"
  "\n""	linux32	Set 32bit uname emulation"
  "\n""	linux64	Set 64bit uname emulation"
  "\n"
  "\n""	-R	Disable address space randomization"
#define linux32_trivial_usage NOUSAGE_STR
#define linux32_full_usage ""
#define linux64_trivial_usage NOUSAGE_STR
#define linux64_full_usage ""
```


## setsid

```
"[-c] PROG ARGS"
#define setsid_full_usage "\n\n"
      "Run PROG in a new session. PROG will have no controlling terminal\n"
      "and will not be affected by keyboard signals (^C etc).\n"
    "\n	-c	Set controlling terminal to stdin"
```


## swapoff

```
(no usage text; state the applet by its BusyBox documentation)
```


## swapon

```
"[-a] [-e]" IF_FEATURE_SWAPON_DISCARD(" [-d[POL]]") IF_FEATURE_SWAPON_PRI(" [-p PRI]") " [DEVICE]"
#define swapon_full_usage "\n\n"
      "Start swapping on DEVICE\n"
    "\n	-a	Start swapping on all swap devices"
IF_FEATURE_SWAPON_DISCARD(
    "\n	-d[POL]	Discard blocks at swapon (POL=once),"
    "\n		as freed (POL=pages), or both (POL omitted)"
)
    "\n	-e	Silently skip devices that do not exist"
IF_FEATURE_SWAPON_PRI(
    "\n	-p PRI	Set swap device priority"
)
#define swapoff_trivial_usage
      "[-a] [DEVICE]"
#define swapoff_full_usage "\n\n"
      "Stop swapping on DEVICE\n"
    "\n	-a	Stop swapping on all swap devices"
```


## taskset

```
"[-ap] [HEXMASK"IF_FEATURE_TASKSET_CPULIST(" | -c LIST")"] { PID | PROG ARGS }"
#define taskset_full_usage "\n\n"
      "Set or get CPU affinity\n"
    "\n	-p	Operate on PID"
    "\n	-a	Operate on all threads"
    "\n	-c	Affinity is a list, not mask"
#define taskset_example_usage
      "$ taskset 0x7 ./dgemm_test&\n"
      "$ taskset -p 0x1 $!\n"
      "pid 4790's current affinity mask: 7\n"
      "pid 4790's new affinity mask: 1\n"
      "$ taskset 0x7 /bin/sh -c './taskset -p 0x1 $$'\n"
      "pid 6671's current affinity mask: 1\n"
      "pid 6671's new affinity mask: 1\n"
      "$ taskset -p 1\n"
      "pid 1's current affinity mask: 3\n"
```


## umount

```
"[-rlf"IF_FEATURE_MTAB_SUPPORT("m")IF_FEATURE_MOUNT_LOOP("d")IF_FEATURE_UMOUNT_ALL("a")"] [-t FSTYPE] FILESYSTEM|DIRECTORY"
#define umount_full_usage "\n\n"
      "Unmount filesystems\n"
IF_FEATURE_UMOUNT_ALL(
    "\n	-a	Unmount all filesystems" IF_FEATURE_MTAB_SUPPORT(" in /etc/mtab")
)
IF_FEATURE_MTAB_SUPPORT(
    "\n	-n	Don't erase /etc/mtab entries"
)
    "\n	-r	Remount devices read-only if mount is busy"
    "\n	-l	Lazy umount (detach filesystem)"
    "\n	-f	Force umount (i.e., unreachable NFS server)"
IF_FEATURE_MOUNT_LOOP(
    "\n	-d	Free loop device if it has been used"
)
    "\n	-t FSTYPE[,...]	Unmount only these filesystem type(s)"
#define umount_example_usage
      "$ umount /dev/hdc1\n"
```


## unshare

```
"[OPTIONS] [PROG ARGS]"
#define unshare_full_usage "\n"
    "\n	-m,--mount[=FILE]	Unshare mount namespace"
    "\n	-u,--uts[=FILE]		Unshare UTS namespace (hostname etc.)"
    "\n	-i,--ipc[=FILE]		Unshare System V IPC namespace"
    "\n	-n,--net[=FILE]		Unshare network namespace"
    "\n	-p,--pid[=FILE]		Unshare PID namespace"
    "\n	-U,--user[=FILE]	Unshare user namespace"
    "\n	-f			Fork before execing PROG"
    "\n	-r			Map current user to root (implies -U)"
    "\n	--mount-proc[=DIR]	Mount /proc filesystem first (implies -m)"
    "\n	--propagation slave|shared|private|unchanged"
    "\n				Modify mount propagation in mount namespace"
    "\n	--setgroups allow|deny	Control the setgroups syscall in user namespaces"
```


## xxd

```
"[-pri] [-g N] [-c N] [-l LEN] [-s OFS] [-o OFS] [FILE]"
#define xxd_full_usage "\n\n"
      "Hex dump FILE (or stdin)\n"
    "\n	-g N		Bytes per group"
    "\n	-c N		Bytes per line"
    "\n	-p		Show only hex bytes, assumes -c30"
    "\n	-i		C include file style"
```
