# busybox-editors(1)

NAME: busybox-editors - the editors and text filters: the BusyBox applets of its `editors` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: `vi`, `sed`, `awk`, `ed`, `diff`, `cmp`, `patch` as BusyBox builds them; `vi` needs the terminal in raw mode, which the console and a pty give.


The usage text BusyBox carries, per applet:


## awk

```
"[OPTIONS] [AWK_PROGRAM] [FILE]..."
#define awk_full_usage "\n\n"
      "	-v VAR=VAL	Set variable"
    "\n	-F SEP		Use SEP as field separator"
    "\n	-f FILE		Read program from FILE"
IF_FEATURE_AWK_GNU_EXTENSIONS(
    "\n	-e AWK_PROGRAM"
)
```


## cmp

```
"[-ls] [-n NUM] FILE1 [FILE2" IF_DESKTOP(" [SKIP1 [SKIP2]]") "]"
#define cmp_full_usage "\n\n"
      "Compare FILE1 with FILE2 (or stdin)\n"
    "\n	-l	Write the byte numbers (decimal) and values (octal)"
    "\n		for all differing bytes"
    "\n	-s	Quiet"
    "\n	-n NUM	Compare at most NUM bytes"
```


## diff

```
"[-abBdiNqrTstw] [-L LABEL] [-S FILE] [-U LINES] FILE1 FILE2"
#define diff_full_usage "\n\n"
      "Compare files line by line and output the differences between them.\n"
      "This implementation supports unified diffs only.\n"
    "\n	-a	Treat all files as text"
    "\n	-b	Ignore changes in the amount of whitespace"
    "\n	-B	Ignore changes whose lines are all blank"
    "\n	-d	Try hard to find a smaller set of changes"
    "\n	-i	Ignore case differences"
    "\n	-L	Use LABEL instead of the filename in the unified header"
    "\n	-N	Treat absent files as empty"
    "\n	-q	Output only whether files differ"
    "\n	-r	Recurse"
    "\n	-S	Start with FILE when comparing directories"
    "\n	-T	Make tabs line up by prefixing a tab when necessary"
    "\n	-s	Report when two files are the same"
    "\n	-t	Expand tabs to spaces in output"
    "\n	-U	Output LINES lines of context"
    "\n	-w	Ignore all whitespace"
```


## ed

```
(no usage text; state the applet by its BusyBox documentation)
```


## patch

```
"[-RNE] [-p N] [-i DIFF] [ORIGFILE [PATCHFILE]]"
#define patch_full_usage "\n\n"
      "	-p N	Strip N leading components from file names"
    "\n	-i DIFF	Read DIFF instead of stdin"
    "\n	-R	Reverse patch"
    "\n	-N	Ignore already applied patches"
    "\n	-E	Remove output files if they become empty"
IF_LONG_OPTS(
    "\n	--dry-run	Don't actually change files"
)
```


## sed

```
"[-i[SFX]] [-nrE] [-f FILE]... [-e CMD]... [FILE]...\n"
      "or: sed [-i[SFX]] [-nrE] CMD [FILE]..."
#define sed_full_usage "\n\n"
      "	-e CMD	Add CMD to sed commands to be executed"
    "\n	-f FILE	Add FILE contents to sed commands to be executed"
    "\n	-i[SFX]	Edit files in-place (otherwise write to stdout)"
    "\n		Optionally back files up, appending SFX"
    "\n	-n	Suppress automatic printing of pattern space"
    "\n	-r,-E	Use extended regex syntax"
    "\n"
    "\nIf no -e or -f, the first non-option argument is the sed command string."
    "\nRemaining arguments are input files (stdin if none)."
#define sed_example_usage
      "$ echo \"foo\" | sed -e 's/f[a-zA-Z]o/bar/g'\n"
      "bar\n"
```


## vi

```
IF_FEATURE_VI_COLON("[-c CMD] ")IF_FEATURE_VI_READONLY("[-R] ")"[-H] [FILE]..."
#define vi_full_usage "\n\n"
      "Edit FILE\n"
IF_FEATURE_VI_COLON(
    "\n	-c CMD	Initial command to run ($EXINIT and ~/.exrc also available)"
)
IF_FEATURE_VI_READONLY(
    "\n	-R	Read-only"
)
    "\n	-H	List available features"
```
