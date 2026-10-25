# BusyBox uses a freed set-id configuration

BusyBox 1.36.1 frees the list parsed from `/etc/busybox.conf` in `check_suid` without clearing the global `suid_config` pointer. A later applet dispatch in the same process traverses that freed list. Rach2 found this while running `ssh test@host id` as uid 1000 during milestone 15. The command faulted; root and interactive logins worked. This record preserves the evidence and reconstruction inputs for a later upstream report. No report or patch has been submitted, and this documentation update runs no new reproduction.

## Source and historical evidence

The affected pinned source is BusyBox 1.36.1, distributed as `busybox-1.36.1.tar.bz2` from [BusyBox's release archive](https://busybox.net/downloads/busybox-1.36.1.tar.bz2). The repository retains that archive under `vendor/busybox/`; its SHA-256 is `b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314`. The unmodified `libbb/appletlib.c` has SHA-256 `d155776560dbe2d931cbed99d6aaf68bb877a624f4405f0f594d3c424d3fd853`. Both identities were checked against the retained archive on 25 October 2026 without building it.

The durable historical records are:

* `04b222add7266710b8a2d03c62313ac41e654934`, dated 9 July 2026, changes the design after the fault. Its parent, `4e6cd758d8599080475ecb0e4a600afe783648b5`, retains the failing configuration and image manifest.
* `dad27ea15082d9a3c01da343239c54aca4c51a80`, dated 10 July, records the command, page fault, successful root and interactive controls, and independent diagnosis in [milestone 15](../15-privilege/Readme.md). It identifies BusyBox's freed configuration rather than a kernel fault.
* `df3df8dda7256043e3f4d98b512343c6cc6e8bfc`, dated 12 August, records that upstream master still had the free without clearing the pointer and that reporting remained unfinished. It does not preserve the upstream commit hash. That historical check is not evidence about the current upstream branch.
* [The milestone 19 plan](plan.md) disables `CONFIG_FEATURE_SUID_CONFIG`. The current [BusyBox integration](../../vendor/busybox/Readme.md) retains that mitigation in its separate set-id binary.

The original raw fault address, complete console transcript and compiler identities are not present in these committed records. Do not invent them from the diagnosis. A submission should include a fresh, retained reproduction and an exact current upstream revision.

## Configuration and reproduction

The historical image staged the same BusyBox bytes as `/bin/busybox` with mode `04755`, owner root, and `/bin/sh` with mode `0755`. Dropbear used `/bin/sh` for the test account, whose uid and gid were both 1000. The main configuration enabled `FEATURE_SUID`, `FEATURE_SUID_CONFIG`, `FEATURE_PREFER_APPLETS`, `FEATURE_SH_STANDALONE`, `ASH`, `SH_IS_ASH` and `ID`. The standalone shell could dispatch `id` in process instead of executing a new binary.

The root-owned, mode `0644` `/etc/busybox.conf` was:

```text
[SUID]
su = sxx
busybox = --x
crontab = --x
findfs = --x
login = --x
mount = --x
passwd = --x
ping = --x
ping6 = --x
traceroute = --x
traceroute6 = --x
vlock = --x
wall = --x
```

To reconstruct the original case later, build an isolated test image from parent `4e6cd758d8599080475ecb0e4a600afe783648b5`, following that revision's build instructions and retaining its generated BusyBox configuration. Keep the current installed Rach2 image unchanged. Start its Dropbear server and invoke an unprivileged command without a pseudo-terminal:

```sh
ssh -T test@HOST id
```

Here `HOST` is the isolated guest's SSH endpoint; supply its actual port and credentials through the normal test setup. The expected result is the test account's identity and a successful exit. The recorded result was a user page fault. Repeat the root command and an interactive test-account login as the historical controls. The shell command of interest is `sh -c id`; SSH was the original way to reach it, not an established requirement of the bug. A smaller standalone reproducer has not been verified here.

## Cause and proposed correction

In the pinned `libbb/appletlib.c`, `suid_config` is the global list at line 305. `check_suid` starts at line 551 and traverses it at line 565:

```c
for (sct = suid_config; sct; sct = sct->m_next) {
    if (sct->m_applet == applet_no)
        goto found;
}
```

Its common return path frees the list at line 642:

```c
ret: ;
    llist_free((llist_t*)suid_config, NULL);
```

The pointer remains non-NULL. A subsequent in-process dispatch reaches the traversal again and reads freed nodes. Root bypasses this path through the early `ruid == 0` return. The original diagnosis attributes the visible difference to allocation behavior: the Rach2 run unmapped the freed allocation, while the Linux comparison left its bytes mapped and did not fault. Remaining mapped does not make the access valid.

The August record proposed clearing the pointer after the free:

```diff
     llist_free((llist_t*)suid_config, NULL);
+    suid_config = NULL;
```

This is a proposed correction, not an applied or validated patch. Before submitting it, check whether later dispatch must retain or reload the configuration instead. Clearing the pointer prevents the stale traversal but also makes a subsequent lookup use `check_need_suid`; the intended authorization behavior must be checked alongside memory safety. No privilege-escalation result or exploit has been established by these records.

Rach2 first avoided the problem by keeping BusyBox unprivileged and using a separate set-id `su`. Milestone 19 later built a separate BusyBox containing only `login`, `passwd` and `su`, with the faulty parser disabled. The main BusyBox remains unprivileged, and the current images stage no `/etc/busybox.conf`. These configuration changes are the shipped mitigation; no local patch to this function is applied.

## Before an upstream submission

Check the current upstream source and reporting instructions, then retain its exact commit, generated configuration, compiler and libc identities. Reproduce the freed read on the pinned source and current upstream, or record precisely where the behavior differs. Preserve the console or sanitizer output, the command's exit status, both historical controls and the behavior after the proposed correction. Review repeated dispatch and privilege-dropping behavior before calling that correction complete. The August record identified `busybox@busybox.net` as the project's preferred reporting route at that time; verify the current route before sending anything.
