# Milestone 19: busybox first

## The direction

Take as much as possible from busybox and keep as little in-house code as possible. A student learns more from the tools a real system ships than from a second implementation of them, and every program this tree writes itself is one more thing to maintain and to get wrong.

Two decisions are already made and are not reopened here.

The applets that need no privilege come back: `adduser`, `addgroup`, `chpasswd`, `deluser` and `delgroup`. Two of those, `deluser` and `delgroup`, are capabilities this system does not have at all today, so removing an account currently means editing three files by hand.

`fcntl` record locking is not built. The reason it was proposed was to make busybox's `update_passwd` exclude our writers, and reading the code shows it would not: the lock there is non-blocking, a failure only prints `warning: can't lock` (`libbb/update_passwd.c:173-174`), and it is released at `:314` before the `fsync` and the `rename` at `:318-319`, so the moment that matters is outside it. What busybox actually excludes with is an `O_CREAT|O_EXCL` temp file at `:142`, retried at 0.1 s intervals, which is the same technique this tree's own lock file uses and which works on this kernel today. Real `fcntl` would be a week of kernel work buying nothing for this.

## The open question

`passwd` and `su` are in-house because they carry the set-id bit, which makes a program run as its owner rather than as whoever started it. busybox here is one binary holding about two hundred applets, and setting that bit on it would run all two hundred as root. So the question is whether busybox can be built into small single-applet binaries, one per set-id applet, each carrying the bit.

It can, and busybox says so itself. From its own `Config.in:216-219`:

> If you are really paranoid and don't want even initial busybox code to run under root for every applet, build two busybox binaries with different applets in them (and the appropriate symlinks pointing to each binary), and only set the suid bit on the one that needs it.

The same block names the applets that require root and refuse to run without it: `crontab`, `login`, `passwd`, `su`, `vlock`, `wall`. And those that use root if they have it: `findfs`, `ping[6]`, `traceroute[6]`, `mount`.

## What was established before writing this

`FEATURE_INDIVIDUAL` is not the mechanism, although its name suggests it. It depends on `BUILD_LIBBUSYBOX` (`Config.in:424-440`), produces a shared `libbusybox.so` with a tiny executable per applet linked against it, and its help text ends with "You need to have a working dynamic linker." Rach2 has none: every binary here is static non-PIE, and the loader cannot relocate a dynamic one, which is why dropbear must be configured `--disable-harden`. So that route is closed for a reason that has nothing to do with this milestone.

The second-binary route was measured rather than assumed. A pristine 1.36.1 tree configured with `allnoconfig` plus `CONFIG_PASSWD`, `CONFIG_STATIC`, `CONFIG_LFS`, the shadow options and `USE_BB_CRYPT`, cross-built with the repo's own `muslgcc` wrapper, produces a working link of 148,236 bytes holding two applets, `busybox` and `passwd`. The in-house `passwd` it would replace is 99,688 bytes. `CONFIG_LFS=y` is required and its absence is not obvious: without it the build stops on `BUG_off_t_size_is_misdetected`, because `off_t` and `uoff_t` disagree.

busybox's `passwd` decides what its caller may do from the REAL uid (`loginutils/passwd.c:136`), which is the discipline a set-id program must follow and the same rule this tree already states for its own. It raises to root only where it writes (`:213`).

The architecture would not change, and this is the part most likely to be got wrong. busybox's standalone shell dispatches applets in-process before it consults the file system, so a separate `/bin/passwd` is only reached if `CONFIG_PASSWD` is OFF in the main busybox. That is exactly the shape shipped today: a main busybox without the applet, plus a small set-id binary beside it. Only the source of the small binary changes, from four hundred lines of ours to a hundred and fifty kilobytes of busybox's.

KRFFS has no symlinks, so the "appropriate symlinks" of busybox's advice become a second staged file. That costs nothing: dispatch is on `argv[0]`, and a file staged as `/bin/passwd` is already named `passwd`.

## What has to be settled before this is built

The history that killed the last attempt has to be re-examined against this one. Milestone 15 tried a single set-id busybox and abandoned it: `check_suid` frees the parsed `/etc/busybox.conf` list and never clears the global, and a later in-process re-dispatch walks the dangling pointer. It hides on Linux, where the freed page usually stays mapped, and faults here because this kernel's `munmap` unmaps promptly. A single-applet binary never parses that file and never re-dispatches, so on the face of it the bug is unreachable; that has to be confirmed rather than assumed, because it is the exact failure this replaces.

How much code runs as root is the other question, and size is the wrong measure of it. What matters is how much is reachable while privileged: for a one-applet build that is the applet plus the parts of `libbb` it calls. Someone should read that path once, the way the in-house programs were read, rather than trading a hundred audited lines for a hundred and fifty kilobytes on trust.

The set-id inventory is a build-time guard here (`tools/check_setid.py`), and it would need to learn about a second busybox artifact: a new set-id binary that the allowlist does not declare fails the build, which is the behaviour wanted, but the allowlist has to name the new one and the manifest has to stage it at the right mode and owner.

Whether `su` follows `passwd` is a separate decision from whether `passwd` moves. `su` is the one program privilege climbs through in this system, it is a hundred lines, and it was audited twice; the case for replacing it is weaker than the case for `passwd`.

## What this milestone would be

Nothing here is implemented yet. In rough order: re-enable the five unprivileged applets and delete the in-house `adduser`, then build the second busybox and move `passwd` to it, then decide about `su` on what the first two teach. Each step keeps the existing gate, and the shutdown, login and credential suites already exercise most of what would change.
