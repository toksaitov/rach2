# busybox-coreutils(1)

NAME: busybox-coreutils - the file and text utilities: the BusyBox applets of its `coreutils` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: On os-simple, which stages this project's own utilities instead of BusyBox, `true`, `false`, `echo`, `yes`, `cat`, `pwd`, `touch`, `mkdir`, `rm` (with `-r` its only option), `mv`, `cp` and `ls` are small teaching implementations (utils/Readme.md 'Portable file utilities'): no options except `rm -r`, `cat` suppresses errors, `ls` lists one directory without `stat`. State that in a paragraph; the applets are what os-real and the installed root run. `sleep` and `usleep` sleep in ticks of 10 ms. `stat` shows the /dev nodes as disk(4) and console(4) say. `sync` is sync(2). `nice`, `chroot`, `nohup` work as the kernel allows: no `setpriority`, so `nice` runs the command at the same priority (check the code's syscall use against syscalls(2)). Found at the writing: `nice` exits when the priority change fails and never runs the command (no getpriority or setpriority here); `du -d N` includes depth N; `hostid` prints zero (musl's gethostid); the options are `ln -T` and `stat -L`, upper case; `fsync -d` and `sync -d` flush metadata too, since fdatasync is fsync here; time setting, `chroot`, `mkfifo`, `mknod` and `sync -f` need calls this kernel does not implement, and `sync -f` ignores that failure.


The usage text BusyBox carries, per applet:


## arch

```
""
#define arch_full_usage "\n\n"
      "Print system architecture"
```


## base32

```
"[-d] [-w COL] [FILE]"
#define base32_full_usage "\n\n"
      "Base32 encode or decode FILE to standard output\n"
    "\n	-d	Decode data"
    "\n	-w COL	Wrap lines at COL (default 76, 0 disables)"
```


## base64

```
"[-d] [-w COL] [FILE]"
#define base64_full_usage "\n\n"
      "Base64 encode or decode FILE to standard output\n"
    "\n	-d	Decode data"
    "\n	-w COL	Wrap lines at COL (default 76, 0 disables)"
```


## basename

```
"FILE [SUFFIX] | -a FILE... | -s SUFFIX FILE..."
#define basename_full_usage "\n\n"
      "Strip directory path and SUFFIX from FILE\n"
    "\n	-a		All arguments are FILEs"
    "\n	-s SUFFIX	Remove SUFFIX (implies -a)"
#define basename_example_usage
      "$ basename /usr/local/bin/foo\n"
      "foo\n"
      "$ basename /usr/local/bin/\n"
      "bin\n"
      "$ basename /foo/bar.txt .txt\n"
      "bar"
```


## cat

```
"[-" IF_FEATURE_CATN("nb") IF_FEATURE_CATV("vteA") "] [FILE]..."
#else
#define cat_trivial_usage
      "[FILE]..."
#endif
#define cat_full_usage "\n\n"
      "Print FILEs to stdout\n"
IF_FEATURE_CATN(
    "\n	-n	Number output lines"
    "\n	-b	Number nonempty lines"
)
IF_FEATURE_CATV(
    "\n	-v	Show nonprinting characters as ^x or M-x"
    "\n	-t	...and tabs as ^I"
    "\n	-e	...and end lines with $"
    "\n	-A	Same as -vte"
)
```


## chgrp

```
"[-Rh"IF_DESKTOP("LHPcvf")"]... GROUP FILE..."
#define chgrp_full_usage "\n\n"
      "Change the group membership of FILEs to GROUP"
    "\n"
    "\n	-h	Affect symlinks instead of symlink targets"
IF_DESKTOP(
    "\n	-L	Traverse all symlinks to directories"
    "\n	-H	Traverse symlinks on command line only"
    "\n	-P	Don't traverse symlinks (default)"
)
```


## chmod

```
"[-R"IF_DESKTOP("cvf")"] MODE[,MODE]... FILE..."
#define chmod_full_usage "\n\n"
      "MODE is octal number (bit pattern sstrwxrwxrwx) or [ugoa]{+|-|=}[rwxXst]"
    "\n"
```


## chown

```
"[-Rh"IF_DESKTOP("LHPcvf")"]... USER[:[GRP]] FILE..."
#define chown_full_usage "\n\n"
      "Change the owner and/or group of FILEs to USER and/or GRP"
    "\n"
    "\n	-h	Affect symlinks instead of symlink targets"
IF_DESKTOP(
    "\n	-L	Traverse all symlinks to directories"
    "\n	-H	Traverse symlinks on command line only"
    "\n	-P	Don't traverse symlinks (default)"
)
```


## chroot

```
"NEWROOT [PROG ARGS]"
#define chroot_full_usage "\n\n"
      "Run PROG with root directory set to NEWROOT"
#define chroot_example_usage
      "$ ls -l /bin/ls\n"
      "lrwxrwxrwx    1 root     root          12 Apr 13 00:46 /bin/ls -> /BusyBox\n"
      "# mount /dev/hdc1 /mnt -t minix\n"
      "# chroot /mnt\n"
      "# ls -l /bin/ls\n"
      "-rwxr-xr-x    1 root     root        40816 Feb  5 07:45 /bin/ls*\n"
```


## cksum

```
"FILE..."
#define cksum_full_usage "\n\n"
      "Calculate CRC32 checksum of FILEs"
```


## comm

```
"[-123] FILE1 FILE2"
#define comm_full_usage "\n\n"
      "Compare FILE1 with FILE2\n"
    "\n	-1	Suppress lines unique to FILE1"
    "\n	-2	Suppress lines unique to FILE2"
    "\n	-3	Suppress lines common to both files"
```


## cp

```
"[-arPLHpfinlsTu] SOURCE DEST\n"
      "or: cp [-arPLHpfinlsu] SOURCE... { -t DIRECTORY | DIRECTORY }"
#define cp_full_usage "\n\n"
      "Copy SOURCEs to DEST\n"
    "\n	-a	Same as -dpR"
IF_SELINUX(
    "\n	-c	Preserve security context"
)
    "\n	-R,-r	Recurse"
    "\n	-d,-P	Preserve symlinks (default if -R)"
    "\n	-L	Follow all symlinks"
    "\n	-H	Follow symlinks on command line"
    "\n	-p	Preserve file attributes if possible"
    "\n	-f	Overwrite"
    "\n	-i	Prompt before overwrite"
    "\n	-n	Don't overwrite"
    "\n	-l,-s	Create (sym)links"
    "\n	-T	Refuse to copy if DEST is a directory"
    "\n	-t DIR	Copy all SOURCEs into DIR"
    "\n	-u	Copy only newer files"
```


## crc32

```
(no usage text; state the applet by its BusyBox documentation)
```


## cut

```
"[OPTIONS] [FILE]..."
#define cut_full_usage "\n\n"
      "Print selected fields from FILEs to stdout\n"
    "\n	-b LIST	Output only bytes from LIST"
    "\n	-c LIST	Output only characters from LIST"
    "\n	-d SEP	Field delimiter for input (default -f TAB, -F run of whitespace)"
    "\n	-O SEP	Field delimeter for output (default = -d for -f, one space for -F)"
    "\n	-D	Don't sort/collate sections or match -fF lines without delimeter"
    "\n	-f LIST	Print only these fields (-d is single char)"
    IF_FEATURE_CUT_REGEX(
    "\n	-F LIST	Print only these fields (-d is regex)"
    )
    "\n	-s	Output only lines containing delimiter"
    "\n	-n	Ignored"
```


## date

```
"[OPTIONS] [+FMT] [[-s] TIME]"
#define date_full_usage "\n\n"
      "Display time (using +FMT), or set time\n"
    "\n	-u		Work in UTC (don't convert to local time)"
    "\n	[-s] TIME	Set time to TIME"
    "\n	-d TIME		Display TIME, not 'now'"
IF_FEATURE_DATE_ISOFMT(
    "\n	-D FMT		FMT (strptime format) for -s/-d TIME conversion"
```


## dd

```
"[if=FILE] [of=FILE] [" IF_FEATURE_DD_IBS_OBS("ibs=N obs=N/") "bs=N] [count=N] [skip=N] [seek=N]"
IF_FEATURE_DD_IBS_OBS("\n"
      "	[conv=notrunc|noerror|sync|fsync]\n"
      "	[iflag=skip_bytes|count_bytes|fullblock|direct] [oflag=seek_bytes|append|direct]"
)
#define dd_full_usage "\n\n"
      "Copy a file with converting and formatting\n"
    "\n	if=FILE		Read from FILE instead of stdin"
    "\n	of=FILE		Write to FILE instead of stdout"
    "\n	bs=N		Read and write N bytes at a time"
IF_FEATURE_DD_IBS_OBS(
    "\n	ibs=N		Read N bytes at a time"
)
IF_FEATURE_DD_IBS_OBS(
    "\n	obs=N		Write N bytes at a time"
)
    "\n	count=N		Copy only N input blocks"
    "\n	skip=N		Skip N input blocks"
    "\n	seek=N		Skip N output blocks"
IF_FEATURE_DD_IBS_OBS(
    "\n	conv=notrunc	Don't truncate output file"
    "\n	conv=noerror	Continue after read errors"
    "\n	conv=sync	Pad blocks with zeros"
    "\n	conv=fsync	Physically write data out before finishing"
    "\n	conv=swab	Swap every pair of bytes"
    "\n	iflag=skip_bytes	skip=N is in bytes"
    "\n	iflag=count_bytes	count=N is in bytes"
    "\n	oflag=seek_bytes	seek=N is in bytes"
    "\n	iflag=direct	O_DIRECT input"
    "\n	oflag=direct	O_DIRECT output"
    "\n	iflag=fullblock	Read full blocks"
    "\n	oflag=append	Open output in append mode"
)
IF_FEATURE_DD_STATUS(
    "\n	status=noxfer	Suppress rate output"
    "\n	status=none	Suppress all output"
)
    "\n"
    "\nN may be suffixed by c (1), w (2), b (512), kB (1000), k (1024), MB, M, GB, G"
#define dd_example_usage
      "$ dd if=/dev/zero of=/dev/ram1 bs=1M count=4\n"
      "4+0 records in\n"
      "4+0 records out\n"
```


## df

```
"[-Pk"
IF_FEATURE_HUMAN_READABLE("mh")
"T"
IF_FEATURE_DF_FANCY("ai] [-B SIZE")
"] [-t TYPE] [FILESYSTEM]..."
#define df_full_usage "\n\n"
      "Print filesystem usage statistics\n"
    "\n	-P	POSIX output format"
    "\n	-k	1024-byte blocks (default)"
IF_FEATURE_HUMAN_READABLE(
    "\n	-m	1M-byte blocks"
    "\n	-h	Human readable (e.g. 1K 243M 2G)"
)
    "\n	-T	Print filesystem type"
    "\n	-t TYPE	Print only mounts of this type"
IF_FEATURE_DF_FANCY(
    "\n	-a	Show all filesystems"
    "\n	-i	Inodes"
    "\n	-B SIZE	Blocksize"
)
#define df_example_usage
      "$ df\n"
      "Filesystem           1K-blocks      Used Available Use% Mounted on\n"
      "/dev/sda3              8690864   8553540    137324  98% /\n"
      "/dev/sda1                64216     36364     27852  57% /boot\n"
      "$ df /dev/sda3\n"
      "Filesystem           1K-blocks      Used Available Use% Mounted on\n"
      "/dev/sda3              8690864   8553540    137324  98% /\n"
      "$ POSIXLY_CORRECT=sure df /dev/sda3\n"
      "Filesystem         512B-blocks      Used Available Use% Mounted on\n"
      "/dev/sda3             17381728  17107080    274648  98% /\n"
      "$ POSIXLY_CORRECT=yep df -P /dev/sda3\n"
      "Filesystem          512-blocks      Used Available Capacity Mounted on\n"
      "/dev/sda3             17381728  17107080    274648      98% /\n"
```


## dirname

```
"FILENAME"
#define dirname_full_usage "\n\n"
      "Strip non-directory suffix from FILENAME"
#define dirname_example_usage
      "$ dirname /tmp/foo\n"
      "/tmp\n"
      "$ dirname /tmp/foo/\n"
      "/tmp\n"
```


## dos2unix

```
"[-ud] [FILE]"
#define dos2unix_full_usage "\n\n"
      "Convert FILE in-place from DOS to Unix format.\n"
      "When no file is given, use stdin/stdout.\n"
    "\n	-u	dos2unix"
    "\n	-d	unix2dos"
#define unix2dos_trivial_usage
      "[-ud] [FILE]"
#define unix2dos_full_usage "\n\n"
      "Convert FILE in-place from Unix to DOS format.\n"
      "When no file is given, use stdin/stdout.\n"
    "\n	-u	dos2unix"
    "\n	-d	unix2dos"
```


## du

```
"[-aHLdclsx" IF_FEATURE_HUMAN_READABLE("hm") "k] [FILE]..."
#define du_full_usage "\n\n"
      "Summarize disk space used for FILEs (or directories)\n"
    "\n	-a	Show file sizes too"
    "\n	-b	Apparent size (including holes)"
    "\n	-L	Follow all symlinks"
    "\n	-H	Follow symlinks on command line"
    "\n	-d N	Limit output to directories (and files with -a) of depth < N"
    "\n	-c	Show grand total"
    "\n	-l	Count sizes many times if hard linked"
    "\n	-s	Display only a total for each argument"
    "\n	-x	Skip directories on different filesystems"
IF_FEATURE_HUMAN_READABLE(
    "\n	-h	Sizes in human readable format (e.g., 1K 243M 2G)"
    "\n	-m	Sizes in megabytes"
)
    "\n	-k	Sizes in kilobytes" IF_FEATURE_DU_DEFAULT_BLOCKSIZE_1K(" (default)")
IF_NOT_FEATURE_DU_DEFAULT_BLOCKSIZE_1K(
    "\n		Default unit is 512 bytes"
)
#define du_example_usage
      "$ du\n"
      "16      ./CVS\n"
      "12      ./kernel-patches/CVS\n"
      "80      ./kernel-patches\n"
      "12      ./tests/CVS\n"
      "36      ./tests\n"
      "12      ./scripts/CVS\n"
      "16      ./scripts\n"
      "12      ./docs/CVS\n"
      "104     ./docs\n"
      "2417    .\n"
```


## echo

```
IF_FEATURE_FANCY_ECHO("[-neE] ") "[ARG]..."
#define echo_full_usage "\n\n"
      "Print ARGs to stdout"
IF_FEATURE_FANCY_ECHO( "\n"
    "\n	-n	No trailing newline"
    "\n	-e	Interpret backslash escapes (\\t=tab etc)"
    "\n	-E	Don't interpret backslash escapes (default)"
)
#define echo_example_usage
      "$ echo \"Erik is cool\"\n"
      "Erik is cool\n"
IF_FEATURE_FANCY_ECHO("$ echo -e \"Erik\\nis\\ncool\"\n"
      "Erik\n"
      "is\n"
      "cool\n"
      "$ echo \"Erik\\nis\\ncool\"\n"
      "Erik\\nis\\ncool\n")
```


## env

```
"[-i0] [-u NAME]... [-] [NAME=VALUE]... [PROG ARGS]"
```


## expand

```
"[-i] [-t N] [FILE]..."
#define expand_full_usage "\n\n"
      "Convert tabs to spaces, writing to stdout\n"
    "\n	-i	Don't convert tabs after non blanks"
    "\n	-t	Tabstops every N chars"
```


## expr

```
"EXPRESSION"
#define expr_full_usage "\n\n"
      "Print the value of EXPRESSION\n"
   "\n"
      "EXPRESSION may be:\n"
      "	ARG1 | ARG2	ARG1 if it is neither null nor 0, otherwise ARG2\n"
      "	ARG1 & ARG2	ARG1 if neither argument is null or 0, otherwise 0\n"
      "	ARG1 < ARG2	1 if ARG1 is less than ARG2, else 0. Similarly:\n"
      "	ARG1 <= ARG2\n"
      "	ARG1 = ARG2\n"
      "	ARG1 != ARG2\n"
      "	ARG1 >= ARG2\n"
      "	ARG1 > ARG2\n"
      "	ARG1 + ARG2	Sum of ARG1 and ARG2. Similarly:\n"
      "	ARG1 - ARG2\n"
      "	ARG1 * ARG2\n"
      "	ARG1 / ARG2\n"
      "	ARG1 % ARG2\n"
      "	STRING : REGEXP		Anchored pattern match of REGEXP in STRING\n"
      "	match STRING REGEXP	Same as STRING : REGEXP\n"
      "	substr STRING POS LEN	Substring of STRING, POS counts from 1\n"
      "	index STRING CHARS	Index in STRING where any CHARS is found, or 0\n"
      "	length STRING		Length of STRING\n"
      "	quote TOKEN		Interpret TOKEN as a string, even if\n"
      "				it is a keyword like 'match' or an\n"
      "				operator like '/'\n"
      "	(EXPRESSION)		Value of EXPRESSION\n"
      "\n"
      "Beware that many operators need to be escaped or quoted for shells.\n"
      "Comparisons are arithmetic if both ARGs are numbers, else\n"
      "lexicographical. Pattern matches return the string matched between\n"
      "\\( and \\) or null; if \\( and \\) are not used, they return the number\n"
      "of characters matched or 0."
```


## factor

```
"[NUMBER]..."
#define factor_full_usage "\n\n"
      "Print prime factors"
```


## false

```
(no usage text; state the applet by its BusyBox documentation)
```


## fold

```
"[-bs] [-w WIDTH] [FILE]..."
#define fold_full_usage "\n\n"
      "Wrap input lines in FILEs (or stdin), writing to stdout\n"
    "\n	-b	Count bytes rather than columns"
    "\n	-s	Break at spaces"
    "\n	-w	Use WIDTH columns instead of 80"
```


## fsync

```
"[-d] FILE..."
#define fsync_full_usage "\n\n"
      "Write all buffered blocks in FILEs to disk\n"
    "\n	-d	Avoid syncing metadata"
```


## groups

```
"[USER]"
#define groups_full_usage "\n\n"
      "Print the groups USER is in"
#define groups_example_usage
      "$ groups\n"
      "andersen lp dialout cdrom floppy\n"
```


## head

```
"[OPTIONS] [FILE]..."
#define head_full_usage "\n\n"
      "Print first 10 lines of FILEs (or stdin).\n"
      "With more than one FILE, precede each with a filename header.\n"
    "\n	-n N[bkm]	Print first N lines"
IF_FEATURE_FANCY_HEAD(
    "\n	-n -N[bkm]	Print all except N last lines"
    "\n	-c [-]N[bkm]	Print first N bytes"
)
    "\n			(b:*512 k:*1024 m:*1024^2)"
IF_FEATURE_FANCY_HEAD(
    "\n	-q		Never print headers"
    "\n	-v		Always print headers"
)
#define head_example_usage
      "$ head -n 2 /etc/passwd\n"
      "root:x:0:0:root:/root:/bin/bash\n"
      "daemon:x:1:1:daemon:/usr/sbin:/bin/sh\n"
```


## hostid

```
""
#define hostid_full_usage "\n\n"
      "Print out a unique 32-bit identifier for the machine"
```


## id

```
"[-ugGnr"IF_SELINUX("Z")"] [USER]"
#define id_full_usage "\n\n"
      "Print information about USER or the current user\n"
IF_SELINUX(
    "\n	-Z	Security context"
)
    "\n	-u	User ID"
    "\n	-g	Group ID"
    "\n	-G	Supplementary group IDs"
    "\n	-n	Print names instead of numbers"
    "\n	-r	Print real ID instead of effective ID"
#define id_example_usage
      "$ id\n"
      "uid=1000(andersen) gid=1000(andersen)\n"
```


## install

```
"[-cdDsp] [-o USER] [-g GRP] [-m MODE] [-t DIR] [SOURCE]... DEST"
#define install_full_usage "\n\n"
      "Copy files and set attributes\n"
    "\n	-c	Just copy (default)"
    "\n	-d	Create directories"
    "\n	-D	Create leading target directories"
    "\n	-s	Strip symbol table"
    "\n	-p	Preserve date"
    "\n	-o USER	Set ownership"
    "\n	-g GRP	Set group ownership"
    "\n	-m MODE	Set permissions"
    "\n	-t DIR	Install to DIR"
IF_SELINUX(
    "\n	-Z	Set security context"
)
```


## link

```
"FILE LINK"
#define link_full_usage "\n\n"
      "Create hard LINK to FILE"
```


## ln

```
"[-sfnbtv] [-S SUF] TARGET... LINK|DIR"
#define ln_full_usage "\n\n"
      "Create a link LINK or DIR/TARGET to the specified TARGET(s)\n"
    "\n	-s	Make symlinks instead of hardlinks"
    "\n	-f	Remove existing destinations"
    "\n	-n	Don't dereference symlinks - treat like normal file"
    "\n	-b	Make a backup of the target (if exists) before link operation"
    "\n	-S SUF	Use suffix instead of ~ when making backup files"
    "\n	-T	Treat LINK as a file, not DIR"
    "\n	-v	Verbose"
#define ln_example_usage
      "$ ln -s BusyBox /tmp/ls\n"
      "$ ls -l /tmp/ls\n"
      "lrwxrwxrwx    1 root     root            7 Apr 12 18:39 ls -> BusyBox*\n"
```


## logname

```
""
#define logname_full_usage "\n\n"
      "Print the name of the current user"
#define logname_example_usage
      "$ logname\n"
      "root\n"
```


## ls

```
"[-1AaCxd"
IF_FEATURE_LS_FOLLOWLINKS("LH")
IF_FEATURE_LS_RECURSIVE("R")
IF_FEATURE_LS_FILETYPES("Fp") "lins"
IF_FEATURE_HUMAN_READABLE("h")
IF_FEATURE_LS_SORTFILES("rSXv")
IF_FEATURE_LS_TIMESTAMPS("ctu")
IF_SELINUX("kZ") "]"
IF_FEATURE_LS_WIDTH(" [-w WIDTH]") " [FILE]..."
#define ls_full_usage "\n\n"
      "List directory contents\n"
    "\n	-1	One column output"
    "\n	-a	Include names starting with ."
    "\n	-A	Like -a, but exclude . and .."
```


## md5sum

```
IF_FEATURE_MD5_SHA1_SUM_CHECK("[-c[sw]] ")"[FILE]..."
#define md5sum_full_usage "\n\n"
      "Print" IF_FEATURE_MD5_SHA1_SUM_CHECK(" or check") " MD5 checksums"
IF_FEATURE_MD5_SHA1_SUM_CHECK( "\n"
    "\n	-c	Check sums against list in FILEs"
    "\n	-s	Don't output anything, status code shows success"
    "\n	-w	Warn about improperly formatted checksum lines"
)
#define md5sum_example_usage
      "$ md5sum < busybox\n"
      "6fd11e98b98a58f64ff3398d7b324003\n"
      "$ md5sum busybox\n"
      "6fd11e98b98a58f64ff3398d7b324003  busybox\n"
      "$ md5sum -c -\n"
      "6fd11e98b98a58f64ff3398d7b324003  busybox\n"
      "busybox: OK\n"
      "^D\n"
#define sha1sum_trivial_usage
IF_FEATURE_MD5_SHA1_SUM_CHECK("[-c[sw]] ")"[FILE]..."
#define sha1sum_full_usage "\n\n"
      "Print" IF_FEATURE_MD5_SHA1_SUM_CHECK(" or check") " SHA1 checksums"
IF_FEATURE_MD5_SHA1_SUM_CHECK( "\n"
    "\n	-c	Check sums against list in FILEs"
    "\n	-s	Don't output anything, status code shows success"
    "\n	-w	Warn about improperly formatted checksum lines"
)
#define sha256sum_trivial_usage
IF_FEATURE_MD5_SHA1_SUM_CHECK("[-c[sw]] ")"[FILE]..."
#define sha256sum_full_usage "\n\n"
      "Print" IF_FEATURE_MD5_SHA1_SUM_CHECK(" or check") " SHA256 checksums"
IF_FEATURE_MD5_SHA1_SUM_CHECK( "\n"
    "\n	-c	Check sums against list in FILEs"
    "\n	-s	Don't output anything, status code shows success"
    "\n	-w	Warn about improperly formatted checksum lines"
)
#define sha512sum_trivial_usage
IF_FEATURE_MD5_SHA1_SUM_CHECK("[-c[sw]] ")"[FILE]..."
#define sha512sum_full_usage "\n\n"
      "Print" IF_FEATURE_MD5_SHA1_SUM_CHECK(" or check") " SHA512 checksums"
IF_FEATURE_MD5_SHA1_SUM_CHECK( "\n"
    "\n	-c	Check sums against list in FILEs"
    "\n	-s	Don't output anything, status code shows success"
    "\n	-w	Warn about improperly formatted checksum lines"
)
#define sha3sum_trivial_usage
IF_FEATURE_MD5_SHA1_SUM_CHECK("[-c[sw]] ")"[-a BITS] [FILE]..."
#define sha3sum_full_usage "\n\n"
      "Print" IF_FEATURE_MD5_SHA1_SUM_CHECK(" or check") " SHA3 checksums"
IF_FEATURE_MD5_SHA1_SUM_CHECK( "\n"
    "\n	-c	Check sums against list in FILEs"
    "\n	-s	Don't output anything, status code shows success"
    "\n	-w	Warn about improperly formatted checksum lines"
)
    "\n	-a BITS	224 (default), 256, 384, 512"
```


## mkdir

```
"[-m MODE] [-p] DIRECTORY..."
#define mkdir_full_usage "\n\n"
      "Create DIRECTORY\n"
    "\n	-m MODE	Mode"
    "\n	-p	No error if exists; make parent directories as needed"
IF_SELINUX(
    "\n	-Z	Set security context"
)
#define mkdir_example_usage
      "$ mkdir /tmp/foo\n"
      "$ mkdir /tmp/foo\n"
      "/tmp/foo: File exists\n"
      "$ mkdir /tmp/foo/bar/baz\n"
      "/tmp/foo/bar/baz: No such file or directory\n"
      "$ mkdir -p /tmp/foo/bar/baz\n"
```


## mkfifo

```
"[-m MODE] " IF_SELINUX("[-Z] ") "NAME"
#define mkfifo_full_usage "\n\n"
      "Create named pipe\n"
    "\n	-m MODE	Mode (default a=rw)"
IF_SELINUX(
    "\n	-Z	Set security context"
)
```


## mknod

```
"[-m MODE] " IF_SELINUX("[-Z] ") "NAME TYPE [MAJOR MINOR]"
#define mknod_full_usage "\n\n"
      "Create a special file (block, character, or pipe)\n"
    "\n	-m MODE	Creation mode (default a=rw)"
IF_SELINUX(
    "\n	-Z	Set security context"
)
    "\nTYPE:"
    "\n	b	Block device"
    "\n	c or u	Character device"
    "\n	p	Named pipe (MAJOR MINOR must be omitted)"
#define mknod_example_usage
      "$ mknod /dev/fd0 b 2 0\n"
      "$ mknod -m 644 /tmp/pipe p\n"
```


## mktemp

```
"[-dt] [-p DIR] [TEMPLATE]"
#define mktemp_full_usage "\n\n"
      "Create a temporary file with name based on TEMPLATE and print its name.\n"
      "TEMPLATE must end with XXXXXX (e.g. [/dir/]nameXXXXXX).\n"
      "Without TEMPLATE, -t tmp.XXXXXX is assumed.\n"
    "\n	-d	Make directory, not file"
    "\n	-q	Fail silently on errors"
    "\n	-t	Prepend base directory name to TEMPLATE"
    "\n	-p DIR	Use DIR as a base directory (implies -t)"
    "\n	-u	Do not create anything; print a name"
    "\n"
    "\nBase directory is: -p DIR, else $TMPDIR, else /tmp"
#define mktemp_example_usage
      "$ mktemp /tmp/temp.XXXXXX\n"
      "/tmp/temp.mWiLjM\n"
      "$ ls -la /tmp/temp.mWiLjM\n"
      "-rw-------    1 andersen andersen        0 Apr 25 17:10 /tmp/temp.mWiLjM\n"
```


## mv

```
"[-finT] SOURCE DEST\n"
      "or: mv [-fin] SOURCE... { -t DIRECTORY | DIRECTORY }"
#define mv_full_usage "\n\n"
      "Rename SOURCE to DEST, or move SOURCEs to DIRECTORY\n"
    "\n	-f	Don't prompt before overwriting"
    "\n	-i	Interactive, prompt before overwrite"
    "\n	-n	Don't overwrite an existing file"
    "\n	-T	Refuse to move if DEST is a directory"
    "\n	-t DIR	Move all SOURCEs into DIR"
#define mv_example_usage
      "$ mv /tmp/foo /bin/bar\n"
```


## nice

```
"[-n ADJUST] [PROG ARGS]"
#define nice_full_usage "\n\n"
      "Change scheduling priority, run PROG\n"
    "\n	-n ADJUST	Adjust priority by ADJUST"
```


## nl

```
"[OPTIONS] [FILE]..."
#define nl_full_usage "\n\n"
      "Write FILEs to standard output with line numbers added\n"
    "\n	-b STYLE	Which lines to number - a: all, t: nonempty, n: none"
```


## nohup

```
"PROG ARGS"
#define nohup_full_usage "\n\n"
      "Run PROG immune to hangups, with output to a non-tty"
#define nohup_example_usage
      "$ nohup make &"
```


## nproc

```
""IF_LONG_OPTS("[--all] [--ignore=N]")
#define nproc_full_usage "\n\n"
"Print number of available CPUs"
IF_LONG_OPTS(
    "\n"
    "\n	--all		Number of installed CPUs"
    "\n	--ignore=N	Exclude N CPUs"
)
```


## od

```
"[-abcdfhilovxs] [-t TYPE] [-A RADIX] [-N SIZE] [-j SKIP] [-S MINSTR] [-w WIDTH] [FILE]..."
```


## paste

```
"[-d LIST] [-s] [FILE]..."
#define paste_full_usage "\n\n"
      "Paste lines from each input file, separated with tab\n"
    "\n	-d LIST	Use delimiters from LIST, not tab"
    "\n	-s      Serial: one file at a time"
#define paste_example_usage
      "# write out directory in four columns\n"
      "$ ls | paste - - - -\n"
      "# combine pairs of lines from a file into single lines\n"
      "$ paste -s -d '\\t\\n' file\n"
```


## printenv

```
"[VARIABLE]..."
#define printenv_full_usage "\n\n"
      "Print environment VARIABLEs.\n"
      "If no VARIABLE specified, print all."
```


## printf

```
"FORMAT [ARG]..."
#define printf_full_usage "\n\n"
      "Format and print ARG(s) according to FORMAT (a-la C printf)"
#define printf_example_usage
      "$ printf \"Val=%d\\n\" 5\n"
      "Val=5\n"
```


## pwd

```
""
#define pwd_full_usage "\n\n"
      "Print the full filename of the current working directory"
#define pwd_example_usage
      "$ pwd\n"
      "/root\n"
```


## readlink

```
IF_FEATURE_READLINK_FOLLOW("[-fnv] ") "FILE"
#define readlink_full_usage "\n\n"
      "Display the value of a symlink"
IF_FEATURE_READLINK_FOLLOW( "\n"
    "\n	-f	Canonicalize by following all symlinks"
    "\n	-n	Don't add newline"
    "\n	-v	Verbose"
)
```


## realpath

```
"FILE..."
#define realpath_full_usage "\n\n"
      "Print absolute pathnames of FILEs"
```


## rm

```
"[-irf] FILE..."
#define rm_full_usage "\n\n"
      "Remove (unlink) FILEs\n"
    "\n	-i	Always prompt before removing"
    "\n	-f	Never prompt"
    "\n	-R,-r	Recurse"
#define rm_example_usage
      "$ rm -rf /tmp/foo\n"
```


## rmdir

```
"[-p] DIRECTORY..."
#define rmdir_full_usage "\n\n"
      "Remove DIRECTORY if it is empty\n"
    "\n	-p	Include parents"
IF_LONG_OPTS(
    "\n	--ignore-fail-on-non-empty"
)
#define rmdir_example_usage
      "# rmdir /tmp/foo\n"
```


## seq

```
"[-w] [-s SEP] [FIRST [INC]] LAST"
#define seq_full_usage "\n\n"
      "Print numbers from FIRST to LAST, in steps of INC.\n"
      "FIRST, INC default to 1.\n"
    "\n	-w	Pad to last with leading zeros"
    "\n	-s SEP	String separator"
```


## sha1sum

```
(no usage text; state the applet by its BusyBox documentation)
```


## sha256sum

```
(no usage text; state the applet by its BusyBox documentation)
```


## sha3sum

```
(no usage text; state the applet by its BusyBox documentation)
```


## sha512sum

```
(no usage text; state the applet by its BusyBox documentation)
```


## shred

```
"[-fuz] [-n N] [-s SIZE] FILE..."
#define shred_full_usage "\n\n"
      "Overwrite/delete FILEs\n"
    "\n	-f	Chmod to ensure writability"
    "\n	-s SIZE	Size to write"
    "\n	-n N	Overwrite N times (default 3)"
    "\n	-z	Final overwrite with zeros"
    "\n	-u	Remove file"
```


## shuf

```
"[-n NUM] [-o FILE] [-z] [FILE | -e [ARG...] | -i L-H]"
#define shuf_full_usage "\n\n"
      "Randomly permute lines\n"
    "\n	-n NUM	Output at most NUM lines"
    "\n	-o FILE	Write to FILE, not standard output"
    "\n	-z	NUL terminated output"
    "\n	-e	Treat ARGs as lines"
    "\n	-i L-H	Treat numbers L-H as lines"
```


## sleep

```
IF_FEATURE_FANCY_SLEEP("[") "N" IF_FEATURE_FANCY_SLEEP("]...")
#define sleep_full_usage "\n\n"
IF_NOT_FEATURE_FANCY_SLEEP("Pause for N seconds")
IF_FEATURE_FANCY_SLEEP(
      "Pause for a time equal to the total of the args given, where each arg can\n"
      "have an optional suffix of (s)econds, (m)inutes, (h)ours, or (d)ays")
#define sleep_example_usage
      "$ sleep 2\n"
      "[2 second delay results]\n"
IF_FEATURE_FANCY_SLEEP(
      "$ sleep 1d 3h 22m 8s\n"
      "[98528 second delay results]\n")
```


## sort

```
"[-nru"
IF_FEATURE_SORT_BIG("ghMcszbdfiokt] [-o FILE] [-k START[.OFS][OPTS][,END[.OFS][OPTS]] [-t CHAR")
      "] [FILE]..."
#define sort_full_usage "\n\n"
      "Sort lines of text\n"
IF_FEATURE_SORT_BIG(
    "\n	-o FILE	Output to FILE"
    "\n	-c	Check whether input is sorted"
    "\n	-b	Ignore leading blanks"
    "\n	-f	Ignore case"
    "\n	-i	Ignore unprintable characters"
    "\n	-d	Dictionary order (blank or alphanumeric only)"
)
```


## split

```
"[OPTIONS] [INPUT [PREFIX]]"
#define split_full_usage "\n\n"
      "	-b N[k|m]	Split by N (kilo|mega)bytes"
    "\n	-l N		Split by N lines"
    "\n	-a N		Use N letters as suffix"
#define split_example_usage
      "$ split TODO foo\n"
      "$ cat TODO | split -a 2 -l 2 TODO_\n"
```


## stat

```
"[-lt"IF_FEATURE_STAT_FILESYSTEM("f")"] "IF_FEATURE_STAT_FORMAT("[-c FMT] ")"FILE..."
#define stat_full_usage "\n\n"
      "Display file"
           IF_FEATURE_STAT_FILESYSTEM(" (default) or filesystem")
           " status\n"
IF_FEATURE_STAT_FORMAT(
    "\n	-c FMT	Use the specified format"
)
IF_FEATURE_STAT_FILESYSTEM(
    "\n	-f	Display filesystem status"
)
    "\n	-L	Follow links"
    "\n	-t	Terse display"
IF_SELINUX(
    "\n	-Z	Print security context"
)
IF_FEATURE_STAT_FORMAT(
      "\n\nFMT sequences"IF_FEATURE_STAT_FILESYSTEM(" for files")":\n"
      " %a	Access rights in octal\n"
      " %A	Access rights in human readable form\n"
      " %b	Number of blocks allocated (see %B)\n"
      " %B	Size in bytes of each block reported by %b\n"
      " %d	Device number in decimal\n"
      " %D	Device number in hex\n"
      " %f	Raw mode in hex\n"
      " %F	File type\n"
      " %g	Group ID\n"
      " %G	Group name\n"
      " %h	Number of hard links\n"
      " %i	Inode number\n"
      " %n	File name\n"
      " %N	File name, with -> TARGET if symlink\n"
      " %o	I/O block size\n"
      " %s	Total size in bytes\n"
      " %t	Major device type in hex\n"
      " %T	Minor device type in hex\n"
      " %u	User ID\n"
      " %U	User name\n"
      " %x	Time of last access\n"
      " %X	Time of last access as seconds since Epoch\n"
      " %y	Time of last modification\n"
      " %Y	Time of last modification as seconds since Epoch\n"
      " %z	Time of last change\n"
      " %Z	Time of last change as seconds since Epoch\n"
IF_FEATURE_STAT_FILESYSTEM(
      "\nFMT sequences for file systems:\n"
      " %a	Free blocks available to non-superuser\n"
      " %b	Total data blocks\n"
      " %c	Total file nodes\n"
      " %d	Free file nodes\n"
      " %f	Free blocks\n"
IF_SELINUX(
      " %C	Security context in selinux\n"
)
      " %i	File System ID in hex\n"
      " %l	Maximum length of filenames\n"
      " %n	File name\n"
      " %s	Block size (for faster transfer)\n"
      " %S	Fundamental block size (for block counts)\n"
      " %t	Type in hex\n"
      " %T	Type in human readable form"
)
)
```


## stty

```
"[-a|g] [-F DEVICE] [SETTING]..."
#define stty_full_usage "\n\n"
      "Without arguments, prints baud rate, line discipline,\n"
      "and deviations from stty sane\n"
    "\n	-F DEVICE	Open device instead of stdin"
    "\n	-a		Print all current settings in human-readable form"
    "\n	-g		Print in stty-readable form"
    "\n	[SETTING]	See manpage"
```


## sum

```
"[-rs] [FILE]..."
#define sum_full_usage "\n\n"
      "Checksum and count the blocks in a file\n"
    "\n	-r	Use BSD sum algorithm (1K blocks)"
    "\n	-s	Use System V sum algorithm (512byte blocks)"
```


## sync

```
""IF_FEATURE_SYNC_FANCY("[-df] [FILE]...")
#define sync_full_usage "\n\n"
   IF_NOT_FEATURE_SYNC_FANCY(
      "Write all buffered blocks to disk"
   )
   IF_FEATURE_SYNC_FANCY(
      "Write all buffered blocks (in FILEs) to disk"
    "\n	-d	Avoid syncing metadata"
    "\n	-f	Sync filesystems underlying FILEs"
   )
```


## tac

```
"[FILE]..."
#define tac_full_usage "\n\n"
"Concatenate FILEs and print them in reverse"
```


## tail

```
"[OPTIONS] [FILE]..."
#define tail_full_usage "\n\n"
      "Print last 10 lines of FILEs (or stdin) to.\n"
      "With more than one FILE, precede each with a filename header.\n"
    "\n	-c [+]N[bkm]	Print last N bytes"
    "\n	-n N[bkm]	Print last N lines"
    "\n	-n +N[bkm]	Start on Nth line and print the rest"
    "\n			(b:*512 k:*1024 m:*1024^2)"
IF_FEATURE_FANCY_TAIL(
    "\n	-q		Never print headers"
    "\n	-v		Always print headers"
)
    "\n	-f		Print data as file grows"
IF_FEATURE_FANCY_TAIL(
    "\n	-F		Same as -f, but keep retrying"
    "\n	-s SECONDS	Wait SECONDS between reads with -f"
)
#define tail_example_usage
      "$ tail -n 1 /etc/resolv.conf\n"
      "nameserver 10.0.0.1\n"
```


## tee

```
"[-ai] [FILE]..."
#define tee_full_usage "\n\n"
      "Copy stdin to each FILE, and also to stdout\n"
    "\n	-a	Append to the given FILEs, don't overwrite"
    "\n	-i	Ignore interrupt signals (SIGINT)"
#define tee_example_usage
      "$ echo \"Hello\" | tee /tmp/foo\n"
      "$ cat /tmp/foo\n"
      "Hello\n"
```


## test

```
(no usage text; state the applet by its BusyBox documentation)
```


## timeout

```
"[-s SIG] [-k KILL_SECS] SECS PROG ARGS"
#define timeout_full_usage "\n\n"
      "Run PROG. Send SIG to it if it is not gone in SECS seconds.\n"
      "Default SIG: TERM."
      "If it still exists in KILL_SECS seconds, send KILL.\n"
```


## touch

```
"[-ch" IF_FEATURE_TOUCH_SUSV3("am") "]"
      IF_FEATURE_TOUCH_SUSV3(" [-d DATE] [-t DATE] [-r FILE]")
      " FILE..."
#define touch_full_usage "\n\n"
      "Update mtime of FILEs\n"
    "\n	-c	Don't create files"
    "\n	-h	Don't follow links"
IF_FEATURE_TOUCH_SUSV3(
    "\n	-a	Change only atime"
    "\n	-m	Change only mtime"
    "\n	-d DT	Date/time to use"
    "\n	-t DT	Date/time to use"
    "\n	-r FILE	Use FILE's date/time"
)
#define touch_example_usage
      "$ ls -l /tmp/foo\n"
      "/bin/ls: /tmp/foo: No such file or directory\n"
      "$ touch /tmp/foo\n"
      "$ ls -l /tmp/foo\n"
      "-rw-rw-r--    1 andersen andersen        0 Apr 15 01:11 /tmp/foo\n"
```


## tr

```
"[-cds] STRING1 [STRING2]"
#define tr_full_usage "\n\n"
      "Translate, squeeze, or delete characters from stdin, writing to stdout\n"
    "\n	-c	Take complement of STRING1"
    "\n	-d	Delete input characters coded STRING1"
    "\n	-s	Squeeze multiple output characters of STRING2 into one character"
#define tr_example_usage
      "$ echo \"gdkkn vnqkc\" | tr [a-y] [b-z]\n"
      "hello world\n"
```


## true

```
(no usage text; state the applet by its BusyBox documentation)
```


## truncate

```
"[-c] -s SIZE FILE..."
#define truncate_full_usage "\n\n"
"Truncate FILEs to SIZE\n"
"\n	-c	Do not create files"
"\n	-s SIZE"
#define truncate_example_usage
"$ truncate -s 1G foo"
```


## tsort

```
"[FILE]"
#define tsort_full_usage "\n\n"
      "Topological sort"
#define tsort_example_usage
      "$ echo -e \"a b\\nb c\" | tsort\n"
      "a\n"
      "b\n"
      "c\n"
```


## tty

```
"" IF_INCLUDE_SUSv2("[-s]")
#define tty_full_usage "\n\n"
      "Print file name of stdin's terminal"
IF_INCLUDE_SUSv2( "\n"
    "\n	-s	Print nothing, only return exit status"
)
#define tty_example_usage
      "$ tty\n"
      "/dev/tty2\n"
```


## uname

```
"[-amnrspvio]"
#define uname_full_usage "\n\n"
      "Print system information\n"
    "\n	-a	Print all"
    "\n	-m	Machine (hardware) type"
    "\n	-n	Hostname"
    "\n	-r	Kernel release"
    "\n	-s	Kernel name (default)"
    "\n	-p	Processor type"
    "\n	-v	Kernel version"
    "\n	-i	Hardware platform"
    "\n	-o	OS name"
#define uname_example_usage
      "$ uname -a\n"
      "Linux debian 2.4.23 #2 Tue Dec 23 17:09:10 MST 2003 i686 GNU/Linux\n"
```


## unexpand

```
"[-fa][-t N] [FILE]..."
#define unexpand_full_usage "\n\n"
      "Convert spaces to tabs, writing to stdout\n"
    "\n	-a	Convert all blanks"
    "\n	-f	Convert only leading blanks"
    "\n	-t N	Tabstops every N chars"
```


## uniq

```
"[-cduiz] [-f,s,w N] [FILE [OUTFILE]]"
#define uniq_full_usage "\n\n"
      "Discard duplicate lines\n"
    "\n	-c	Prefix lines by the number of occurrences"
    "\n	-d	Only print duplicate lines"
    "\n	-u	Only print unique lines"
    "\n	-i	Ignore case"
    "\n	-z	NUL terminated output"
    "\n	-f N	Skip first N fields"
    "\n	-s N	Skip first N chars (after any skipped fields)"
    "\n	-w N	Compare N characters in line"
#define uniq_example_usage
      "$ echo -e \"a\\na\\nb\\nc\\nc\\na\" | sort | uniq\n"
      "a\n"
      "b\n"
      "c\n"
```


## unix2dos

```
(no usage text; state the applet by its BusyBox documentation)
```


## unlink

```
"FILE"
#define unlink_full_usage "\n\n"
"Delete FILE by calling unlink()"
```


## users

```
""
#define users_full_usage "\n\n"
      "Print the users currently logged on"
```


## usleep

```
"N"
#define usleep_full_usage "\n\n"
      "Pause for N microseconds"
#define usleep_example_usage
      "$ usleep 1000000\n"
      "[pauses for 1 second]\n"
```


## uudecode

```
"[-o OUTFILE] [INFILE]"
#define uudecode_full_usage "\n\n"
      "Uudecode a file\n"
      "Finds OUTFILE in uuencoded source unless -o is given"
#define uudecode_example_usage
      "$ uudecode -o busybox busybox.uu\n"
      "$ ls -l busybox\n"
      "-rwxr-xr-x   1 ams      ams        245264 Jun  7 21:35 busybox\n"
```


## uuencode

```
"[-m] [FILE] STORED_FILENAME"
#define uuencode_full_usage "\n\n"
      "Uuencode FILE (or stdin) to stdout\n"
    "\n	-m	Use base64 encoding per RFC1521"
#define uuencode_example_usage
      "$ uuencode busybox busybox\n"
      "begin 755 busybox\n"
      "<encoded file snipped>\n"
      "$ uudecode busybox busybox > busybox.uu\n"
      "$\n"
```


## w

```
""
#define w_full_usage "\n\n"
      "Show who is logged on"
```


## wc

```
"[-c"IF_UNICODE_SUPPORT("m")"lwL] [FILE]..."
#define wc_full_usage "\n\n"
      "Count lines, words, and bytes for FILEs (or stdin)\n"
    "\n	-c	Count bytes"
IF_UNICODE_SUPPORT(
    "\n	-m	Count characters"
)
    "\n	-l	Count newlines"
    "\n	-w	Count words"
    "\n	-L	Print longest line length"
#define wc_example_usage
      "$ wc /etc/passwd\n"
      "     31      46    1365 /etc/passwd\n"
```


## who

```
"[-aH]"
#define who_full_usage "\n\n"
      "Show who is logged on\n"
    "\n	-a	Show all"
    "\n	-H	Print column headers"
```


## whoami

```
""
#define whoami_full_usage "\n\n"
      "Print the user name associated with the current effective user id"
```


## yes

```
"[STRING]"
#define yes_full_usage "\n\n"
      "Repeatedly print a line with STRING, or 'y'"
```
