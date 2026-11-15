# Lenovo native acceptance

All 250 eligible native cases have complete passing results on the exact kernel SHA-256 `5c922e7da6ba5849ae830f5e05b289d37405c71957baf79caae710d0bf149e80`, with owned cleanup and normal-system restoration independently checked. The complete selection ran on 25 October 2026. It passed 249 cases and failed `gnu/procps` because PID 1 retained the trial init's executable name. Booting the same kernel with the restored ordinary init made that unchanged case pass. The failed attempt remains in the record; this is complete same-kernel coverage across a whole selection and one affected retry, not one uninterrupted passing run.

[The exact final index](native-final-results.tsv) names every case, run, original result digest, archive member, kernel and boot identity. It is SHA-256 `0d8ac5e97ecda70ec0da07f6fab15bc918c564074db2c05d8f6114cdb2c5c569`. The earlier [diagnostic record](native-diagnostic.md) and its [index](native-results.tsv) remain separate. No diagnostic result substitutes for a result in the final index.

## Source and preparation

The kernel was built from `320daa6a59a732ed1db084a17e6a4b79996dc069`, 1,767,808 bytes. Its ext2 repair removes the fixed hard-link table capacity without weakening corruption checks. The preserved workload production is `403f6d99b04615013f78ec031636ba25803989ae`; responder and root repairs are from `41f0fcd66c8b3813b56abbe58392b4f90ccd62e8`, and the mounted bind-holder repair is `35bd60fc5`. These are distinct source events, not a claim that every binary was recompiled at one later commit.

* Controller: actual Lenovo GCC 16.2.0 `-O2` build from `1a23727bb16f987e03d8c93f45332a2959633505`, SHA-256 `668f5fadae4a8a6df4a0a397844ef36a26d6eeca2853a4fa9512bcfb3be05cde`.
* Responder: SHA-256 `c1e38d3739f28c9c5b9b747234a86b2b9922979314bd8f363ebdd71285b35fc9`. Its per-attempt owner reaps detached descendants before confirming cleanup.
* Bind-holder probe: SHA-256 `643ce569962bf6d7ad4ea7bf674bb8c12546cae8496d71d1602190ac4a544ae9`; private KRFFS root: `896ab7179e116db0541db6109debb4c83fb146d5be2ed81964d0ea09fc7110b9`.
* Prepared view: archive SHA-256 `c822931056e56fbd54238edbaeceb62f1a1190af3d392798633e648df666dbea`, 3,359,855 bytes. It reuses the existing immutable payload by relative symbolic links, introduces no hard links, and changes only the repaired probe/root and exact inventory metadata. All 420 declared files were checked before publication. It does not redeploy the unchanged large volumes.

The public deployment helper staged that view at `/var/native/p-c822931056e56fbd`. Its controller is native Rach2 C; the Mac supplies SSH deployment and the declared network peers. The actual helper binary is SHA-256 `4c35ec1bf71cfeecbeeb6a24364e8000eba51d821ff9b967bbcc768f909b99b0`.

## Commands and results

The whole command used the existing public interface:

```text
native-deploy run PREPARED 192.168.0.106 candidate KERNEL KEY /var/native t93-final250-k1153-01 LOGS --hostpeer HOSTPEER --peer 192.168.0.100 --echo-port 55090 --network 192.168.0.106/24 --interface eth0 --gateway 192.168.0.1 --far 1.1.1.1 --empty 192.168.0.250 --silent 192.168.0.252 --purpose phase_first_run --
```

`PREPARED`, `KERNEL`, `KEY`, `LOGS` and `HOSTPEER` are the absolute paths in the retained `full-run.command.json`. An empty trailing selection means all 250 declared eligible cases. The retry uses the same paths and bytes, run `t93-procps-normal-init-01`, purpose `focused` and the sole trailing case `gnu/procps`.

The whole operator ran from 20:39:59 to 21:37:06 local time, 3427.073911 seconds including initial validation and cleanup. It returned 1 with 249 passing cases and one failed case, no incomplete case. Its archive is SHA-256 `391785781625ccc5036a150dd51ae63a88383f6fb9ed0d2acd90be5b3dec27d0`, 998,086 bytes. The exact failure was:

```text
GNUROW FAIL procps: ps -p: got [ 1 init.trial-1153], want [ 1 init]
```

The original init file had been restored, but the running PID 1 kept the name of the executable opened during the guarded trial. After terminal collection cleanup and account restoration, an ordinary reboot used the already selected candidate without changing kernel, boot configuration or workload. Fresh reads confirmed PID 1 `init` and the same kernel bytes. The affected retry returned 0 at 21:40:27, attempt `b122dad046e5811a`, process 64, with its workspace removed and cleanup confirmed. Its archive is SHA-256 `3aac40d5ead48c40740d88f97531a05d5cbf79bb0d9192193f66394c1494ce2d`, 719,599 bytes.

## Cleanup and restored system

Independent checks after each run compared exact process and kernel-start identities with the recorded system services and the checking SSH connection. They found no unknown identities or surviving test processes, private mounts, loop attachments, controller claim or collection lock. Every final passing case has a complete structured result, confirmed cleanup and a removed workspace. Both histories and their referenced blobs are retained separately; the failed whole history is unchanged.

The Lenovo is identified by MAC `xx:xx:xx:xx:xx:xx` and Samsung SSD serial `XXXXXXXXXXXXXXX`. At 21:40:43 the normal candidate was booted and saved, with an empty next entry, PID 1 `init`, writable ext2 root, SSH at 192.168.0.106/24 and default route through 192.168.0.1. The original init SHA-256 `d0dca971335fce8cd606b529ba5a614e51610eea106377ad52d0c10e3a1d6b11` and DHCP configuration SHA-256 `0a4efeacd7510762536856dc79ec6f26d79f62689796e65abdb748ca6619de36` match. No trial watchdog remains. The fallback's full slot remains SHA-256 `b6a68b9451d4c7d09e165e7ee041dbb0959067a5840b5be9ad0c5d52401e7a52`.

Temporary nobody/nogroup entries, needed by the GNU account check, were removed after confirming no process used UID or GID 65534. Every account file's exact original bytes, mode, owner and group were restored. The installed `test` account remains UID/GID 60000. Immutable payloads, failed workspaces and result evidence remain deliberately retained.

The complete local packet is `build/tests/t93-final-remaining/final-coverage.json`, SHA-256 `bdb8d7751226bb545184f6c0ad1d01fa92bbc0aa6757a13bcd5febd60fce3555`, in the native-attempt-session worktree. It binds 60 raw command, output, result, cleanup, account and boot proof files. Issues #682, #1131 and #1148 retain the result and restoration evidence. The earlier pre-reboot network outage remains unexplained; the verified ext2 and init restoration repairs do not establish its cause.

This fulfills T93's native coverage and restoration condition. The owner explicitly released hardware diagnostics before a complete desktop pass; no such pass is claimed here. The smaller native selection is excluded from desktop performance ranking. T24, T90 to T92, T94, T95, final review and release retain their separate obligations.
