# busybox-modutils(8)

NAME: busybox-modutils - the module tools: the BusyBox applets of its `modutils` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: There are no loadable modules in this kernel: `insmod`, `rmmod`, `lsmod`, `modprobe`, `depmod` and `modinfo` have nothing to act on and report so. Found at the writing: the build is modprobe-small, whose `rmmod` takes a module name, ignores `-w` and `-f` and rejects `-a`; the full implementation's options in the usage text below are not built.


The usage text BusyBox carries, per applet:


## depmod

```
(no usage text; state the applet by its BusyBox documentation)
```


## insmod

```
IF_FEATURE_2_4_MODULES("[-fkvqLx] MODULE")
IF_NOT_FEATURE_2_4_MODULES("FILE")
IF_FEATURE_CMDLINE_MODULE_OPTIONS(" [SYMBOL=VALUE]...")
#define insmod_full_usage "\n\n"
      "Load kernel module"
IF_FEATURE_2_4_MODULES( "\n"
    "\n	-f	Force module to load into the wrong kernel version"
    "\n	-k	Make module autoclean-able"
    "\n	-v	Verbose"
    "\n	-q	Quiet"
    "\n	-L	Lock: prevent simultaneous loads"
IF_FEATURE_INSMOD_LOAD_MAP(
    "\n	-m	Output load map to stdout"
)
    "\n	-x	Don't export externs"
)
#endif
```


## lsmod

```
""
#define lsmod_full_usage "\n\n"
      "List loaded kernel modules"
#endif
```


## modinfo

```
"[-adlpn0] [-F keyword] MODULE"
#define modinfo_full_usage "\n\n"
      "	-a		Shortcut for '-F author'"
    "\n	-d		Shortcut for '-F description'"
    "\n	-l		Shortcut for '-F license'"
    "\n	-p		Shortcut for '-F parm'"
```


## modprobe

```
"[-rq] MODULE" IF_FEATURE_CMDLINE_MODULE_OPTIONS(" [SYMBOL=VALUE]...")
#define modprobe_full_usage "\n\n"
      "	-r	Remove MODULE"
    "\n	-q	Quiet"
```


## rmmod

```
"[-wfa] [MODULE]..."
#define rmmod_full_usage "\n\n"
      "Unload kernel modules\n"
    "\n	-w	Wait until the module is no longer used"
    "\n	-f	Force unload"
    "\n	-a	Remove all unused modules (recursively)"
#define rmmod_example_usage
      "$ rmmod tulip\n"
#endif
```
