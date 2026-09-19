# busybox-archival(1)

NAME: busybox-archival - the archivers and compressors: the BusyBox applets of its `archival` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: The compressors and archivers; `tar` on the file systems here; no `dpkg`/`rpm` databases exist on the images.


The usage text BusyBox carries, per applet:


## bunzip2

```
"[-cfk] [FILE]..."
#define bunzip2_full_usage "\n\n"
      "Decompress FILEs (or stdin)\n"
    "\n	-c	Write to stdout"
    "\n	-f	Force"
    "\n	-k	Keep input files"
    "\n	-t	Test integrity"
#define bzcat_trivial_usage
      "[FILE]..."
#define bzcat_full_usage "\n\n"
      "Decompress to stdout"
```


## bzcat

```
(no usage text; state the applet by its BusyBox documentation)
```


## bzip2

```
"[-cfk" IF_FEATURE_BZIP2_DECOMPRESS("dt") "123456789] [FILE]..."
#define bzip2_full_usage "\n\n"
      "Compress FILEs (or stdin) with bzip2 algorithm\n"
    "\n	-1..9	Compression level"
IF_FEATURE_BZIP2_DECOMPRESS(
    "\n	-d	Decompress"
)
    "\n	-c	Write to stdout"
    "\n	-f	Force"
    "\n	-k	Keep input files"
IF_FEATURE_BZIP2_DECOMPRESS(
    "\n	-t	Test integrity"
)
```


## cpio

```
"[-dmvu] [-F FILE] [-R USER[:GRP]]" IF_FEATURE_CPIO_O(" [-H newc]")
      " [-ti"IF_FEATURE_CPIO_O("o")"]" IF_FEATURE_CPIO_P(" [-p DIR]")
      " [EXTR_FILE]..."
#define cpio_full_usage "\n\n"
      "Extract (-i) or list (-t) files from a cpio archive"
IF_FEATURE_CPIO_O(", or"
    "\ntake file list from stdin and create an archive (-o)"
               IF_FEATURE_CPIO_P(" or copy files (-p)")
)
    "\n"
    "\nMain operation mode:"
    "\n	-t	List"
    "\n	-i	Extract EXTR_FILEs (or all)"
IF_FEATURE_CPIO_O(
    "\n	-o	Create (requires -H newc)"
)
IF_FEATURE_CPIO_P(
    "\n	-p DIR	Copy files to DIR"
)
    "\nOptions:"
IF_FEATURE_CPIO_O(
    "\n	-H newc	Archive format"
)
    "\n	-d	Make leading directories"
    "\n	-m	Restore mtime"
    "\n	-v	Verbose"
    "\n	-u	Overwrite"
    "\n	-F FILE	Input (-t,-i,-p) or output (-o) file"
    "\n	-R USER[:GRP]	Set owner of created files"
    "\n	-L	Dereference symlinks"
    "\n	-0	NUL terminated input"
IF_FEATURE_CPIO_IGNORE_DEVNO(
    "\n	--ignore-devno"
)
IF_FEATURE_CPIO_RENUMBER_INODES(
    "\n	--renumber-inodes"
)
```


## dpkg

```
"[-ilCPru] [-F OPT] PACKAGE"
#define dpkg_full_usage "\n\n"
      "Install, remove and manage Debian packages\n"
IF_LONG_OPTS(
    "\n	-i,--install	Install the package"
    "\n	-l,--list	List of installed packages"
    "\n	--configure	Configure an unpackaged package"
    "\n	-P,--purge	Purge all files of a package"
    "\n	-r,--remove	Remove all but the configuration files for a package"
    "\n	--unpack	Unpack a package, but don't configure it"
    "\n	--force-depends	Ignore dependency problems"
    "\n	--force-confnew	Overwrite existing config files when installing"
    "\n	--force-confold	Keep old config files when installing"
)
IF_NOT_LONG_OPTS(
    "\n	-i		Install the package"
    "\n	-l		List of installed packages"
    "\n	-C		Configure an unpackaged package"
    "\n	-P		Purge all files of a package"
    "\n	-r		Remove all but the configuration files for a package"
    "\n	-u		Unpack a package, but don't configure it"
    "\n	-F depends	Ignore dependency problems"
    "\n	-F confnew	Overwrite existing config files when installing"
    "\n	-F confold	Keep old config files when installing"
)
```


## dpkg-deb

```
"[-cefxX] FILE [DIR]"
#define dpkg_deb_full_usage "\n\n"
      "Perform actions on Debian packages (.deb)\n"
    "\n	-c	List files"
    "\n	-f	Print control fields"
    "\n	-e	Extract control files to DIR (default: ./DEBIAN)"
    "\n	-x	Extract files to DIR (no default)"
    "\n	-X	Verbose extract"
#define dpkg_deb_example_usage
      "$ dpkg-deb -X ./busybox_0.48-1_i386.deb /tmp\n"
```


## gunzip

```
"[-cfkt] [FILE]..."
#define gunzip_full_usage "\n\n"
      "Decompress FILEs (or stdin)\n"
    "\n	-c	Write to stdout"
    "\n	-f	Force"
    "\n	-k	Keep input files"
    "\n	-t	Test integrity"
#define gunzip_example_usage
      "$ ls -la /tmp/BusyBox*\n"
      "-rw-rw-r--    1 andersen andersen   557009 Apr 11 10:55 /tmp/BusyBox-0.43.tar.gz\n"
      "$ gunzip /tmp/BusyBox-0.43.tar.gz\n"
      "$ ls -la /tmp/BusyBox*\n"
      "-rw-rw-r--    1 andersen andersen  1761280 Apr 14 17:47 /tmp/BusyBox-0.43.tar\n"
#define zcat_trivial_usage
      "[FILE]..."
#define zcat_full_usage "\n\n"
      "Decompress to stdout"
```


## gzip

```
"[-cfk" IF_FEATURE_GZIP_DECOMPRESS("dt") IF_FEATURE_GZIP_LEVELS("123456789") "] [FILE]..."
#define gzip_full_usage "\n\n"
      "Compress FILEs (or stdin)\n"
IF_FEATURE_GZIP_LEVELS(
    "\n	-1..9	Compression level"
)
IF_FEATURE_GZIP_DECOMPRESS(
    "\n	-d	Decompress"
)
    "\n	-c	Write to stdout"
    "\n	-f	Force"
    "\n	-k	Keep input files"
IF_FEATURE_GZIP_DECOMPRESS(
    "\n	-t	Test integrity"
)
#define gzip_example_usage
      "$ ls -la /tmp/busybox*\n"
      "-rw-rw-r--    1 andersen andersen  1761280 Apr 14 17:47 /tmp/busybox.tar\n"
      "$ gzip /tmp/busybox.tar\n"
      "$ ls -la /tmp/busybox*\n"
      "-rw-rw-r--    1 andersen andersen   554058 Apr 14 17:49 /tmp/busybox.tar.gz\n"
```


## lzcat

```
(no usage text; state the applet by its BusyBox documentation)
```


## lzma

```
(no usage text; state the applet by its BusyBox documentation)
```


## lzop

```
"[-cfUvd123456789CF] [FILE]..."
#define lzop_full_usage "\n\n"
      "	-1..9	Compression level"
    "\n	-d	Decompress"
    "\n	-c	Write to stdout"
    "\n	-f	Force"
    "\n	-U	Delete input files"
```


## rpm

```
"-i PACKAGE.rpm; rpm -qp[ildc] PACKAGE.rpm"
#define rpm_full_usage "\n\n"
      "Manipulate RPM packages\n"
    "\nCommands:"
    "\n	-i	Install package"
    "\n	-qp	Query package"
    "\n	-qpi	Show information"
    "\n	-qpl	List contents"
    "\n	-qpd	List documents"
    "\n	-qpc	List config files"
```


## rpm2cpio

```
"PACKAGE.rpm"
#define rpm2cpio_full_usage "\n\n"
      "Output a cpio archive of the rpm file"
```


## tar

```
IF_FEATURE_TAR_CREATE("c|") "x|t [-"
IF_FEATURE_SEAMLESS_Z("Z")
IF_FEATURE_SEAMLESS_GZ("z")
IF_FEATURE_SEAMLESS_XZ("J")
IF_FEATURE_SEAMLESS_BZ2("j")
"a"
IF_FEATURE_TAR_CREATE("h")
IF_FEATURE_TAR_NOPRESERVE_TIME("m")
"vokO] "
"[-f TARFILE] [-C DIR] "
IF_FEATURE_TAR_FROM("[-T FILE] [-X FILE] "IF_FEATURE_TAR_LONG_OPTIONS("[LONGOPT]... "))
"[FILE]..."
#define tar_full_usage "\n\n"
IF_FEATURE_TAR_CREATE("Create, extract, ")
IF_NOT_FEATURE_TAR_CREATE("Extract ")
"or list files from a tar file"
    "\n"
IF_FEATURE_TAR_CREATE(
    "\n	c	Create"
)
    "\n	x	Extract"
    "\n	t	List"
    "\n	-f FILE	Name of TARFILE ('-' for stdin/out)"
    "\n	-C DIR	Change to DIR before operation"
    "\n	-v	Verbose"
    "\n	-O	Extract to stdout"
IF_FEATURE_TAR_NOPRESERVE_TIME(
    "\n	-m	Don't restore mtime"
)
    "\n	-o	Don't restore user:group"
```


## unlzma

```
"[-cfk] [FILE]..."
#define unlzma_full_usage "\n\n"
      "Decompress FILEs (or stdin)\n"
    "\n	-c	Write to stdout"
    "\n	-f	Force"
    "\n	-k	Keep input files"
    "\n	-t	Test integrity"
#define lzma_trivial_usage
      "-d [-cfk] [FILE]..."
#define lzma_full_usage "\n\n"
      "Decompress FILEs (or stdin)\n"
    "\n	-d	Decompress"
    "\n	-c	Write to stdout"
    "\n	-f	Force"
    "\n	-k	Keep input files"
    "\n	-t	Test integrity"
#define lzcat_trivial_usage
      "[FILE]..."
#define lzcat_full_usage "\n\n"
      "Decompress to stdout"
```


## unxz

```
"[-cfk] [FILE]..."
#define unxz_full_usage "\n\n"
      "Decompress FILEs (or stdin)\n"
    "\n	-c	Write to stdout"
    "\n	-f	Force"
    "\n	-k	Keep input files"
    "\n	-t	Test integrity"
#define xz_trivial_usage
      "-d [-cfk] [FILE]..."
#define xz_full_usage "\n\n"
      "Decompress FILEs (or stdin)\n"
    "\n	-d	Decompress"
    "\n	-c	Write to stdout"
    "\n	-f	Force"
    "\n	-k	Keep input files"
    "\n	-t	Test integrity"
#define xzcat_trivial_usage
      "[FILE]..."
#define xzcat_full_usage "\n\n"
      "Decompress to stdout"
```


## unzip

```
"[-lnojpqK] FILE[.zip] [FILE]... [-x FILE]... [-d DIR]"
#define unzip_full_usage "\n\n"
      "Extract FILEs from ZIP archive\n"
    "\n	-l	List contents (with -q for short form)"
    "\n	-n	Never overwrite files (default: ask)"
    "\n	-o	Overwrite"
    "\n	-j	Do not restore paths"
    "\n	-p	Write to stdout"
    "\n	-t	Test"
    "\n	-q	Quiet"
    "\n	-K	Do not clear SUID bit"
    "\n	-x FILE	Exclude FILEs"
    "\n	-d DIR	Extract into DIR"
```


## xz

```
(no usage text; state the applet by its BusyBox documentation)
```


## xzcat

```
(no usage text; state the applet by its BusyBox documentation)
```


## zcat

```
(no usage text; state the applet by its BusyBox documentation)
```
