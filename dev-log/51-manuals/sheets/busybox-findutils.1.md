# busybox-findutils(1)

NAME: busybox-findutils - find, grep and xargs: the BusyBox applets of its `findutils` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: `find`, `grep`, `egrep`, `fgrep`, `xargs` as built. Found at the writing: a failed `find` action does not always stop the file's evaluation; it continues through `-o`.


The usage text BusyBox carries, per applet:


## egrep

```
(no usage text; state the applet by its BusyBox documentation)
```


## fgrep

```
(no usage text; state the applet by its BusyBox documentation)
```


## find

```
"[-HL] [PATH]... [OPTIONS] [ACTIONS]"
#define find_full_usage "\n\n"
      "Search for files and perform actions on them.\n"
      "First failed action stops processing of current file.\n"
      "Defaults: PATH is current directory, action is '-print'\n"
    "\n	-L,-follow	Follow symlinks"
    "\n	-H		...on command line only"
IF_FEATURE_FIND_XDEV(
    "\n	-xdev		Don't descend directories on other filesystems"
)
IF_FEATURE_FIND_MAXDEPTH(
    "\n	-maxdepth N	Descend at most N levels. -maxdepth 0 applies"
    "\n			actions to command line arguments only"
    "\n	-mindepth N	Don't act on first N levels"
)
IF_FEATURE_FIND_DEPTH(
    "\n	-depth		Act on directory *after* traversing it"
)
    "\n"
    "\nActions:"
IF_FEATURE_FIND_PAREN(
    "\n	( ACTIONS )	Group actions for -o / -a"
)
IF_FEATURE_FIND_NOT(
    "\n	! ACT		Invert ACT's success/failure"
)
    "\n	ACT1 [-a] ACT2	If ACT1 fails, stop, else do ACT2"
    "\n	ACT1 -o ACT2	If ACT1 succeeds, stop, else do ACT2"
    "\n			Note: -a has higher priority than -o"
    "\n	-name PATTERN	Match file name (w/o directory name) to PATTERN"
    "\n	-iname PATTERN	Case insensitive -name"
IF_FEATURE_FIND_PATH(
    "\n	-path PATTERN	Match path to PATTERN"
    "\n	-ipath PATTERN	Case insensitive -path"
)
IF_FEATURE_FIND_REGEX(
    "\n	-regex PATTERN	Match path to regex PATTERN"
)
IF_FEATURE_FIND_TYPE(
    "\n	-type X		File type is X (one of: f,d,l,b,c,s,p)"
)
IF_FEATURE_FIND_EXECUTABLE(
    "\n	-executable	File is executable"
)
IF_FEATURE_FIND_PERM(
    "\n	-perm MASK	At least one mask bit (+MASK), all bits (-MASK),"
    "\n			or exactly MASK bits are set in file's mode"
)
IF_FEATURE_FIND_MTIME(
    "\n	-mtime DAYS	mtime is greater than (+N), less than (-N),"
    "\n			or exactly N days in the past"
)
IF_FEATURE_FIND_ATIME(
    "\n	-atime DAYS	atime +N/-N/N days in the past"
)
IF_FEATURE_FIND_CTIME(
    "\n	-ctime DAYS	ctime +N/-N/N days in the past"
)
IF_FEATURE_FIND_MMIN(
    "\n	-mmin MINS	mtime is greater than (+N), less than (-N),"
    "\n			or exactly N minutes in the past"
)
IF_FEATURE_FIND_AMIN(
    "\n	-amin MINS	atime +N/-N/N minutes in the past"
)
IF_FEATURE_FIND_CMIN(
    "\n	-cmin MINS	ctime +N/-N/N minutes in the past"
)
IF_FEATURE_FIND_NEWER(
    "\n	-newer FILE	mtime is more recent than FILE's"
)
IF_FEATURE_FIND_INUM(
    "\n	-inum N		File has inode number N"
)
IF_FEATURE_FIND_SAMEFILE(
    "\n	-samefile FILE	File is same as FILE"
)
IF_FEATURE_FIND_USER(
    "\n	-user NAME/ID	File is owned by given user"
)
IF_FEATURE_FIND_GROUP(
    "\n	-group NAME/ID	File is owned by given group"
)
IF_FEATURE_FIND_SIZE(
    "\n	-size N[bck]	File size is N (c:bytes,k:kbytes,b:512 bytes(def.))"
    "\n			+/-N: file size is bigger/smaller than N"
)
IF_FEATURE_FIND_LINKS(
    "\n	-links N	Number of links is greater than (+N), less than (-N),"
    "\n			or exactly N"
)
IF_FEATURE_FIND_CONTEXT(
    "\n	-context CTX	File has specified security context"
)
IF_FEATURE_FIND_EMPTY(
    "\n	-empty		Match empty file/directory"
)
IF_FEATURE_FIND_PRUNE(
    "\n	-prune		If current file is directory, don't descend into it"
)
    "\nIf none of the following actions is specified, -print is assumed"
    "\n	-print		Print file name"
IF_FEATURE_FIND_PRINT0(
    "\n	-print0		Print file name, NUL terminated"
)
IF_FEATURE_FIND_EXEC(
    "\n	-exec CMD ARG ;	Run CMD with all instances of {} replaced by"
    "\n			file name. Fails if CMD exits with nonzero"
)
IF_FEATURE_FIND_EXEC_PLUS(
    "\n	-exec CMD ARG + Run CMD with {} replaced by list of file names"
)
IF_FEATURE_FIND_DELETE(
    "\n	-delete		Delete current file/directory. Turns on -depth option"
)
IF_FEATURE_FIND_QUIT(
    "\n	-quit		Exit"
)
#define find_example_usage
      "$ find / -name passwd\n"
      "/etc/passwd\n"
```


## grep

```
"[-HhnlLoqvsrRiwFE"
IF_EXTRA_COMPAT("z")
      "] [-m N] "
IF_FEATURE_GREP_CONTEXT("[-A|B|C N] ")
      "{ PATTERN | -e PATTERN... | -f FILE... } [FILE]..."
#define grep_full_usage "\n\n"
      "Search for PATTERN in FILEs (or stdin)\n"
    "\n	-H	Add 'filename:' prefix"
    "\n	-h	Do not add 'filename:' prefix"
    "\n	-n	Add 'line_no:' prefix"
    "\n	-l	Show only names of files that match"
    "\n	-L	Show only names of files that don't match"
    "\n	-c	Show only count of matching lines"
    "\n	-o	Show only the matching part of line"
    "\n	-q	Quiet. Return 0 if PATTERN is found, 1 otherwise"
    "\n	-v	Select non-matching lines"
    "\n	-s	Suppress open and read errors"
    "\n	-r	Recurse"
    "\n	-R	Recurse and dereference symlinks"
    "\n	-i	Ignore case"
    "\n	-w	Match whole words only"
    "\n	-x	Match whole lines only"
    "\n	-F	PATTERN is a literal (not regexp)"
    "\n	-E	PATTERN is an extended regexp"
IF_EXTRA_COMPAT(
    "\n	-z	NUL terminated input"
)
    "\n	-m N	Match up to N times per file"
IF_FEATURE_GREP_CONTEXT(
    "\n	-A N	Print N lines of trailing context"
    "\n	-B N	Print N lines of leading context"
    "\n	-C N	Same as '-A N -B N'"
)
    "\n	-e PTRN	Pattern to match"
    "\n	-f FILE	Read pattern from file"
#define grep_example_usage
      "$ grep root /etc/passwd\n"
      "root:x:0:0:root:/root:/bin/bash\n"
      "$ grep ^[rR]oo. /etc/passwd\n"
      "root:x:0:0:root:/root:/bin/bash\n"
#define egrep_trivial_usage NOUSAGE_STR
#define egrep_full_usage ""
#define fgrep_trivial_usage NOUSAGE_STR
#define fgrep_full_usage ""
```


## xargs

```
"[OPTIONS] [PROG ARGS]"
#define xargs_full_usage "\n\n"
      "Run PROG on every item given by stdin\n"
IF_FEATURE_XARGS_SUPPORT_ZERO_TERM(
    "\n	-0	NUL terminated input"
)
IF_FEATURE_XARGS_SUPPORT_ARGS_FILE(
    "\n	-a FILE	Read from FILE instead of stdin"
)
    "\n	-o	Reopen stdin as /dev/tty"
    "\n	-r	Don't run command if input is empty"
    "\n	-t	Print the command on stderr before execution"
IF_FEATURE_XARGS_SUPPORT_CONFIRMATION(
    "\n	-p	Ask user whether to run each command"
)
    "\n	-E STR,-e[STR]	STR stops input processing"
IF_FEATURE_XARGS_SUPPORT_REPL_STR(
    "\n	-I STR	Replace STR within PROG ARGS with input line"
)
    "\n	-n N	Pass no more than N args to PROG"
    "\n	-s N	Pass command line of no more than N bytes"
IF_FEATURE_XARGS_SUPPORT_PARALLEL(
    "\n	-P N	Run up to N PROGs in parallel"
)
IF_FEATURE_XARGS_SUPPORT_TERMOPT(
    "\n	-x	Exit if size is exceeded"
)
#define xargs_example_usage
      "$ ls | xargs gzip\n"
      "$ find . -name '*.c' -print | xargs rm\n"
```
