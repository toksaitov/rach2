# busybox-init(8)

NAME: busybox-init - halt, poweroff and reboot: the BusyBox applets of its `init` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: `init` here is not BusyBox's: /sbin/init is this project's init(8). `halt`, `poweroff` and `reboot` call reboot(2) and are the way to stop the machine on os-real; the standalone shell dispatches them before the file system. `bootchartd` and `linuxrc` have no use here.


The usage text BusyBox carries, per applet:


## bootchartd

```
"start [PROG ARGS]|stop|init"
#define bootchartd_full_usage "\n\n"
      "Create /var/log/bootchart.tgz with boot chart data\n"
    "\nstart: start background logging; with PROG, run PROG, then kill logging with USR1"
    "\nstop: send USR1 to all bootchartd processes"
    "\ninit: start background logging; stop when getty/xdm is seen (for init scripts)"
    "\nUnder PID 1: as init, then exec $bootchart_init, /init, /sbin/init"
```


## halt

```
"[-d DELAY] [-nf"IF_FEATURE_WTMP("w")"]"
#define halt_full_usage "\n\n"
      "Halt the system\n"
    "\n	-d SEC	Delay interval"
    "\n	-n	Do not sync"
    "\n	-f	Force (don't go through init)"
IF_FEATURE_WTMP(
    "\n	-w	Only write a wtmp record"
)
#define poweroff_trivial_usage
      "[-d DELAY] [-nf]"
#define poweroff_full_usage "\n\n"
      "Halt and shut off power\n"
    "\n	-d SEC	Delay interval"
    "\n	-n	Do not sync"
    "\n	-f	Force (don't go through init)"
#define reboot_trivial_usage
      "[-d DELAY] [-nf]"
#define reboot_full_usage "\n\n"
      "Reboot the system\n"
    "\n	-d SEC	Delay interval"
    "\n	-n	Do not sync"
    "\n	-f	Force (don't go through init)"
```


## init

```
""
#define init_full_usage "\n\n"
      "Init is the first process started during boot. It never exits."
IF_FEATURE_USE_INITTAB(
  "\n""It (re)spawns children according to /etc/inittab."
  "\n""Signals:"
  "\n""HUP: reload /etc/inittab"
)
IF_NOT_FEATURE_USE_INITTAB(
  "\n""This version of init doesn't use /etc/inittab,"
  "\n""has fixed set of processed to run."
  "\n""Signals:"
)
  "\n""TSTP: stop respawning until CONT"
  "\n""QUIT: re-exec another init"
  "\n""USR1/TERM/USR2/INT: run halt/reboot/poweroff/Ctrl-Alt-Del script"
#define init_notes_usage
"This version of init is designed to be run only by the kernel.\n"
"\n"
"BusyBox init doesn't support multiple runlevels. The runlevels field of\n"
"the /etc/inittab file is completely ignored by BusyBox init. If you want\n"
"runlevels, use sysvinit.\n"
"\n"
"BusyBox init works just fine without an inittab. If no inittab is found,\n"
"it has the following default behavior:\n"
"\n"
"	::sysinit:/etc/init.d/rcS\n"
"	::askfirst:/bin/sh\n"
"	::ctrlaltdel:/sbin/reboot\n"
"	::shutdown:/sbin/swapoff -a\n"
"	::shutdown:/bin/umount -a -r\n"
"	::restart:/sbin/init\n"
"	tty2::askfirst:/bin/sh\n"
"	tty3::askfirst:/bin/sh\n"
"	tty4::askfirst:/bin/sh\n"
"\n"
"If you choose to use an /etc/inittab file, the inittab entry format is as follows:\n"
"\n"
"	<id>:<runlevels>:<action>:<process>\n"
"\n"
"	<id>:\n"
"\n"
"		WARNING: This field has a non-traditional meaning for BusyBox init!\n"
"		The id field is used by BusyBox init to specify the controlling tty for\n"
"		the specified process to run on. The contents of this field are\n"
"		appended to \"/dev/\" and used as-is. There is no need for this field to\n"
"		be unique, although if it isn't you may have strange results. If this\n"
"		field is left blank, then the init's stdin/out will be used.\n"
"\n"
"	<runlevels>:\n"
"\n"
"		The runlevels field is completely ignored.\n"
"\n"
"	<action>:\n"
"\n"
"		Valid actions include: sysinit, respawn, askfirst, wait,\n"
"		once, restart, ctrlaltdel, and shutdown.\n"
"\n"
"		The available actions can be classified into two groups: actions\n"
"		that are run only once, and actions that are re-run when the specified\n"
"		process exits.\n"
"\n"
"		Run only-once actions:\n"
"\n"
"			'sysinit' is the first item run on boot. init waits until all\n"
"			sysinit actions are completed before continuing. Following the\n"
"			completion of all sysinit actions, all 'wait' actions are run.\n"
"			'wait' actions, like 'sysinit' actions, cause init to wait until\n"
"			the specified task completes. 'once' actions are asynchronous,\n"
"			therefore, init does not wait for them to complete. 'restart' is\n"
"			the action taken to restart the init process. By default this should\n"
"			simply run /sbin/init, but can be a script which runs pivot_root or it\n"
"			can do all sorts of other interesting things. The 'ctrlaltdel' init\n"
"			actions are run when the system detects that someone on the system\n"
"			console has pressed the CTRL-ALT-DEL key combination. Typically one\n"
"			wants to run 'reboot' at this point to cause the system to reboot.\n"
"			Finally the 'shutdown' action specifies the actions to taken when\n"
"			init is told to reboot. Unmounting filesystems and disabling swap\n"
"			is a very good here.\n"
"\n"
"		Run repeatedly actions:\n"
"\n"
"			'respawn' actions are run after the 'once' actions. When a process\n"
"			started with a 'respawn' action exits, init automatically restarts\n"
"			it. Unlike sysvinit, BusyBox init does not stop processes from\n"
"			respawning out of control. The 'askfirst' actions acts just like\n"
"			respawn, except that before running the specified process it\n"
"			displays the line \"Please press Enter to activate this console.\"\n"
"			and then waits for the user to press enter before starting the\n"
"			specified process.\n"
"\n"
"		Unrecognized actions (like initdefault) will cause init to emit an\n"
"		error message, and then go along with its business. All actions are\n"
"		run in the order they appear in /etc/inittab.\n"
"\n"
"	<process>:\n"
"\n"
"		Specifies the process to be executed and its command line.\n"
"\n"
"Example /etc/inittab file:\n"
"\n"
"	# This is run first except when booting in single-user mode\n"
"	#\n"
"	::sysinit:/etc/init.d/rcS\n"
"	\n"
"	# /bin/sh invocations on selected ttys\n"
"	#\n"
"	# Start an \"askfirst\" shell on the console (whatever that may be)\n"
"	::askfirst:-/bin/sh\n"
"	# Start an \"askfirst\" shell on /dev/tty2-4\n"
"	tty2::askfirst:-/bin/sh\n"
"	tty3::askfirst:-/bin/sh\n"
"	tty4::askfirst:-/bin/sh\n"
"	\n"
"	# /sbin/getty invocations for selected ttys\n"
"	#\n"
"	tty4::respawn:/sbin/getty 38400 tty4\n"
"	tty5::respawn:/sbin/getty 38400 tty5\n"
"	\n"
"	\n"
"	# Example of how to put a getty on a serial line (for a terminal)\n"
"	#\n"
"	#::respawn:/sbin/getty -L ttyS0 9600 vt100\n"
"	#::respawn:/sbin/getty -L ttyS1 9600 vt100\n"
"	#\n"
"	# Example how to put a getty on a modem line\n"
"	#::respawn:/sbin/getty 57600 ttyS2\n"
"	\n"
"	# Stuff to do when restarting the init process\n"
"	::restart:/sbin/init\n"
"	\n"
"	# Stuff to do before rebooting\n"
"	::ctrlaltdel:/sbin/reboot\n"
"	::shutdown:/bin/umount -a -r\n"
"	::shutdown:/sbin/swapoff -a\n"
```


## linuxrc

```
(no usage text; state the applet by its BusyBox documentation)
```


## poweroff

```
(no usage text; state the applet by its BusyBox documentation)
```


## reboot

```
(no usage text; state the applet by its BusyBox documentation)
```
