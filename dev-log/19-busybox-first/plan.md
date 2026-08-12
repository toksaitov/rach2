# Milestone 19: busybox first, and a home for our own tools

## The direction

Two images, two answers to the same question, and each one honest about what it is.

`os-real` is the machine built from the tools the world actually ships. Anything busybox can do there, busybox does. What stays ours in that image stays only because busybox's version cannot run on this kernel.

`os-simple` is the machine built from the tools this project wrote. It already holds our shell and our coreutils; it gains our `passwd`, `su`, `adduser` and `login`, and the small `/etc` they need in order to mean anything. Nothing written for milestone 17 is thrown away; it moves to the image where it is the point rather than a duplicate.

Size is not a consideration. Neither is defence against an attacker who does not exist: the rule is not to do anything stupid, not to do everything conceivable.

## os-real: what comes back from busybox

Five applets need no privilege and simply come back, and two of them are capabilities this system does not have at all today, since removing an account currently means editing three files by hand:

`adduser`, `addgroup`, `deluser`, `delgroup`, `chpasswd`.

Two more come back that were switched off over the strength of busybox's password salt, which is a pid and a clock through a small generator. For a teaching system that is good enough, and busybox's own comment says as much:

`cryptpw`, `mkpasswd`. The same reasoning retires the objection to `FEATURE_HTTPD_AUTH_MD5`.

Three need the set-id bit, which is what makes a program run as its owner rather than as whoever started it. They come from a second busybox, built from the same source with a config holding only them, staged as separate files carrying the bit:

`passwd`, `su`, `login`.

That is busybox's own advice, from `Config.in:216-219`: build two binaries with different applets in them and set the bit only on the one that needs it. The alternative that sounds right, `FEATURE_INDIVIDUAL`, is not available: it depends on `BUILD_LIBBUSYBOX` and needs a dynamic linker (`Config.in:424-440`), and everything here is static.

## os-real: what stays ours, and why

`ping`, because busybox's opens a raw socket and this kernel has none, so it dies before it sends anything. This is the only in-house program in that image that is there for a reason no decision can change.

`init`, for now. Ours is PID 1, execs `/bin/login` and nothing else, answers the three shutdown signals and continues a stopped console session. busybox has an `init` applet and moving to it is a real option, but PID 1 is the wrong place to take a change casually, and nothing about it is currently wrong. Left as a later question rather than folded into this milestone.

## os-simple: the tools find their home

Today that image stages our shell, our coreutils and `halt`, and has no accounts at all: no `/etc/passwd`, no `/etc/shadow`, no `/etc/group`. So its console is a root shell and there is nobody to be.

It gains a small `/etc` with two accounts, and the four programs that make an account mean something: `su`, `passwd`, `adduser` and `login`, the last through the strict init that cannot fall back to an unauthenticated shell. The result is a complete little multi-user Unix built entirely from code this project wrote and can read end to end, which is a better thing to hand a student than a second copy of busybox's.

## What has to be got right

The applet guard is the piece most likely to break quietly. `tools/check_setid.py` currently fails the build if `su`, `passwd` or `login` appear in busybox's applet table at all. After this its job inverts: the MAIN busybox must contain no applet that wants the set-id bit, the second build must contain only the three that do, and the manifest must stage those three at the right mode and owner. That check is what stops a later config edit from quietly putting a privileged applet back into the unprivileged binary.

The bug that killed the last attempt has to be re-examined rather than assumed away. Milestone 15 tried one set-id busybox and abandoned it: `check_suid` frees the parsed `/etc/busybox.conf` list without clearing the pointer, and a later in-process dispatch walks it; it hides on Linux and faults here because this kernel unmaps freed pages promptly. A single-applet binary parses no config and re-dispatches to nothing, so the path should be unreachable, and that should be confirmed with the same capstone that found it.

Somebody should read busybox's `passwd` and the `libbb` functions it calls once, while privileged, the way the in-house programs were read. It authorizes on the real uid (`loginutils/passwd.c:136`), which is the discipline that matters and which it gets right; the rest deserves one honest pass.

A build detail worth writing down because its symptom is opaque: the second build needs `CONFIG_LFS=y`, without which it stops on `BUG_off_t_size_is_misdetected`.

## Order

Re-enable the seven unprivileged applets and drop our `adduser` from os-real. Then the second busybox and the three set-id programs. Then os-simple's `/etc` and the four programs that move into it. `init` is a separate decision, later.
