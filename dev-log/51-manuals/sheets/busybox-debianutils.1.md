# busybox-debianutils(1)

NAME: busybox-debianutils - which, run-parts and start-stop-daemon: the BusyBox applets of its `debianutils` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: `which`, `run-parts`, `start-stop-daemon`, `pipe_progress`. Found at the writing: `start-stop-daemon -N` fails before starting the program, since setpriority is not implemented.


The usage text BusyBox carries, per applet:


## pipe_progress

```
(no usage text; state the applet by its BusyBox documentation)
```


## run-parts

```
"[-a ARG]... [-u UMASK] "
      IF_FEATURE_RUN_PARTS_LONG_OPTIONS("[--reverse] [--test] [--exit-on-error] "IF_FEATURE_RUN_PARTS_FANCY("[--list] "))
      "DIRECTORY"
#define run_parts_full_usage "\n\n"
      "Run a bunch of scripts in DIRECTORY\n"
    "\n	-a ARG		Pass ARG as argument to scripts"
    "\n	-u UMASK	Set UMASK before running scripts"
IF_FEATURE_RUN_PARTS_LONG_OPTIONS(
    "\n	--reverse	Reverse execution order"
    "\n	--test		Dry run"
    "\n	--exit-on-error	Exit if a script exits with non-zero"
IF_FEATURE_RUN_PARTS_FANCY(
    "\n	--list		Print names of matching files even if they are not executable"
)
)
#define run_parts_example_usage
      "$ run-parts -a start /etc/init.d\n"
      "$ run-parts -a stop=now /etc/init.d\n\n"
      "Let's assume you have a script foo/dosomething:\n"
      "#!/bin/sh\n"
      "for i in $*; do eval $i; done; unset i\n"
      "case \"$1\" in\n"
      "start*) echo starting something;;\n"
      "stop*) set -x; shutdown -h $stop;;\n"
      "esac\n\n"
      "Running this yields:\n"
      "$run-parts -a stop=+4m foo/\n"
      "+ shutdown -h +4m"
```


## start-stop-daemon

```
"[OPTIONS] [-S|-K] ... [-- ARGS...]"
#define start_stop_daemon_full_usage "\n\n"
      "Search for matching processes, and then\n"
      "-K: stop all matching processes\n"
      "-S: start a process unless a matching process is found\n"
    "\nProcess matching:"
    "\n	-u USERNAME|UID	Match only this user's processes"
    "\n	-n NAME		Match processes with NAME"
    "\n			in comm field in /proc/PID/stat"
    "\n	-x EXECUTABLE	Match processes with this command"
    "\n			in /proc/PID/cmdline"
    "\n	-p FILE		Match a process with PID from FILE"
    "\n	All specified conditions must match"
    "\n-S only:"
    "\n	-x EXECUTABLE	Program to run"
    "\n	-a NAME		Zeroth argument"
    "\n	-b		Background"
IF_FEATURE_START_STOP_DAEMON_FANCY(
    "\n	-N N		Change nice level"
)
    "\n	-c USER[:[GRP]]	Change user/group"
    "\n	-m		Write PID to pidfile specified by -p"
    "\n-K only:"
    "\n	-s SIG		Signal to send"
    "\n	-t		Match only, exit with 0 if found"
    "\nOther:"
IF_FEATURE_START_STOP_DAEMON_FANCY(
    "\n	-o		Exit with status 0 if nothing is done"
    "\n	-v		Verbose"
)
    "\n	-q		Quiet"
```


## which

```
"[-a] COMMAND..."
#define which_full_usage "\n\n"
      "Locate COMMAND\n"
    "\n	-a	Show all matches"
#define which_example_usage
      "$ which login\n"
      "/bin/login\n"
```
