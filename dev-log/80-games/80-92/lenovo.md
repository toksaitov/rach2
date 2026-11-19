# 80-92's Lenovo step

One load of Crysis's island on the Lenovo, on one trial boot of the milestone's kernel on the installed root, measured by the game's own log, beside Linux's 19.7 s on the same machine ([80-12.md](../80-12.md)). The load is the first part of Linux's flyby, `+map island`, which the release's native case `games/crysis.flyby-direct` runs: `native/crysis.sh flyby direct` on a private copy of the game, which reads the owner's files on the data partition read-only. Its line `crysis: loaded in S s` is the figure. In QEMU the load is within Linux's range ([80-92.md](../80-92.md#the-sixth-pass)).

The step takes the panel and plays the game's sound at volume 1 during the session's restart and the flyby's first two loops. Post the notice on #4159 after the QEMU rehearsal: "Lenovo notice: from HH:MM for about 15 minutes, three reboots and Crysis's island flyby on the panel, sound at volume 1." The first reboot waits at least ten minutes after the notice.

The QEMU measurement included 80-94 to 80-97 and this task's processor-time account fix. The account is arithmetic on the tick's counts and touches no device. The measurement also exercised 80-97's one-shot APIC timer and idle tick. The manager's go on #4159 names the revision to build. Boot that kernel in QEMU first, using the Lenovo's GPT root selector, and run through the first flyby loop before taking the Lenovo's lock. Power the guest off after the loop.

On the Mac, by a loop of the owner's, in the main clone, after the go. Fill in the Lenovo's station address and disk serial in `L` from the owner's machine records. Definitions:

```
L() { ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o ConnectTimeout=5 -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ServerAliveInterval=5 -o ServerAliveCountMax=3 root@192.168.0.106 'test "$(cat /sys/class/net/eth0/address)" = xx:xx:xx:xx:xx:xx && grep -Fq XXXXXXXXXXXXXXX /proc/disk || exit 1; '"$*"; }
back() { i=0; until L "test \"\$(cat /proc/sys/kernel/random/boot_id)\" != \"$BOOT_ID\"" 2> /dev/null; do i=$((i + 1)); [ $i -ge 180 ] && { echo "no answer"; return 1; }; sleep 2; done; L 'mixer vol 1' && echo "$(date +%T) answered"; }
T=build/worktrees/t4159
R=build/runs/t4159; mkdir -p $R
```

`back` checks for SSH on a new boot, identified by `boot_id`, up to 180 times, with two seconds between attempts. It sets the volume once the new boot answers.

## Build

1. The kernel, on the Mac:

```
git fetch origin && git worktree add $T HEAD-OF-THE-GO
make -C $T ARCH=x86_64 rach2 utils
ls -l $T/build/x86_64/rach2/kernel
KS=$(shasum -a 256 $T/build/x86_64/rach2/kernel | cut -c1-16)
```

   Passes: the head holds the account's fix (`git -C $T log --oneline -1 --grep 'last scheduler edge'` names a commit) and the kernel fits the installed slot. `bootctl status` gives its size. Record the kernel's size and SHA-256 on the issue.

## On the Lenovo

2. Take the lock after the QEMU boot and before the first Lenovo command. Stop if the lock is held. Check the release, its record, both slots and the menu's bytes:

```
mkdir build/flow/lenovo && echo 4159 > build/flow/lenovo/issue
L 'cat /etc/rach2/release; bootctl status; cat /proc/cmdline; pgrep -l weston' | tee $R/before.txt
L 'mount -t ext2 /dev/disk0p6 /data && mkdir -p /data/trial4159 && df -k /data && ls /data/crysis'
L 's=$(bootctl status | sed -n "s/^bootctl: boot.cfg: first sector \([0-9]*\),.*/\1/p"); test -n "$s" && dd if=/dev/bootpart bs=512 skip="$s" count=16 of=/data/trial4159/original.cfg 2>/dev/null && sha256sum /data/trial4159/original.cfg'
```

   Use the release build's kernel at `$R/release-kernel`, checked against the release record, and stage it at `/data/trial4159/release-kernel`. Check both slots with `tools/dev/machine_identity.sh`. If reading the kernel from a slot, concatenate its sector runs in the order `bootctl status` gives before taking the release kernel's bytes. The installed slots may each have several runs.

   Passes: the release booted from its candidate slot with no trial armed, the desktop supervisor running, `/data/crysis` present, and the release's kernel in both slots. The case can start Weston through that supervisor. If the fallback differs, `L 'bootctl fallback'` first. The menu's read needs its 16 sectors contiguous, as `bootctl status` shows.

3. The kernel, staged and checked, and the case's files in a workspace of their own:

```
L "cat > /data/trial4159/kernel-$KS" < $T/build/x86_64/rach2/kernel
L 'mkdir -m 755 /var/lib/rach2-trial-4159 && mkdir /var/lib/rach2-trial-4159/private'
L 'cat > /var/lib/rach2-trial-4159/bootctl && chmod 700 /var/lib/rach2-trial-4159/bootctl' < $T/build/x86_64/utils/sbin/bootctl
for f in tests/system/games/crysis.py tests/system/games/wm.py tests/native/crysis.sh; do L "cat > /var/lib/rach2-trial-4159/$(basename $f)" < $T/$f; done
L "sha256sum /data/trial4159/kernel-$KS /var/lib/rach2-trial-4159/bootctl /var/lib/rach2-trial-4159/*.py /var/lib/rach2-trial-4159/crysis.sh && sync"
```

   Passes: the kernel and files have the local digests. The workspace on the installed ext2 root stays through reboots and lets the desktop account reach the flyby's output directory. Its copy of `bootctl` supplies `config`. Use the installed `bootctl` for slot commands, since it matches the installed slot size.

4. Read the Mac's address with `ipconfig getifaddr en0` and its station address on `eth0` from the Lenovo's `/proc/net/arp`. Set `NET=netconsole=6665@192.168.0.106/,6667@IP/STATION` and start `nc -d -lu 6667 > $R/netconsole.log` in the tool's background session, as the owner's user. Keep its session or PID and stop it after the release returns, or on a failed boot once the log has been saved.

5. Arm one boot of the kernel from the candidate slot with the netconsole word. Each command must succeed before the next:

```
L "bootctl kernel /data/trial4159/kernel-$KS"
CMD=$(L 'cat /proc/cmdline')
printf 'if [ "${next_entry}" = "candidate" ]; then\nset next_entry=\nsave_env next_entry\nsearch --no-floppy --fs-uuid --set=root 1ACE-F00D\nmultiboot2 /boot/kernel %s %s\nboot\nfi\n' "$CMD" "$NET" | L 'cat > /data/trial4159/trial.cfg && /var/lib/rach2-trial-4159/bootctl config /data/trial4159/trial.cfg && umount /data && sync && bootctl status' | tee $R/armed.txt
```

   Passes: `bootctl status` reports `next_entry` as `candidate`. GRUB loads the environment before sourcing the staged block. The block consumes `next_entry` before booting, so a reset can still take the fallback.

6. The boot and the load, after the notice's ten minutes:

```
BOOT_ID=$(L 'cat /proc/sys/kernel/random/boot_id') && L '(nohup sh -c "sleep 2; reboot" > /dev/null 2>&1 &)' && back
L 'cat /proc/cmdline; uname -a; bootctl status; pgrep -l weston' | tee $R/trial.txt
RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL=$T/build/x86_64/rach2/kernel sh tools/dev/machine_identity.sh $R/identity-trial.txt ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@192.168.0.106
L 'cd /var/lib/rach2-trial-4159 && unshare -m --propagation private sh ./crysis.sh flyby direct' 2>&1 | tee $R/flyby.out
L 'mixer; dmesg | grep -E "i915: (reset|hangcheck)|wedged|vtd: fault|vm: fault|panic"; cat /proc/stat | head -1' | tee $R/after-flyby.txt
```

   Before the flyby, check the netconsole word, the new kernel's build string and the candidate digest. The flyby passes with `crysis: loaded in S s`, then `crysis-case: result: pass`, the mixer at 1, and no reset, wedge or fault. The figure is S beside Linux's 19.7 s and Rach2's 41.00 s on the GPU before (80-12). If `back` fails, record the netconsole log on #4159 and stop.

## Put it back

7. Clear the staged menu before the reboot to the release's fallback slot. Keep it clear while restoring the candidate, then restore the saved menu after the candidate boots and is committed. Check each command before going on:

```
L 'echo "# trial ended" > /var/lib/rach2-trial-4159/empty.cfg && /var/lib/rach2-trial-4159/bootctl config /var/lib/rach2-trial-4159/empty.cfg && sync'
BOOT_ID=$(L 'cat /proc/sys/kernel/random/boot_id') && L '(nohup sh -c "sleep 2; reboot" > /dev/null 2>&1 &)' && back
L 'mount -t ext2 /dev/disk0p6 /data && bootctl kernel /data/trial4159/release-kernel && umount /data && sync'
BOOT_ID=$(L 'cat /proc/sys/kernel/random/boot_id') && L '(nohup sh -c "sleep 2; reboot" > /dev/null 2>&1 &)' && back
L 'bootctl commit && bootctl fallback && mount -t ext2 /dev/disk0p6 /data && /var/lib/rach2-trial-4159/bootctl config /data/trial4159/original.cfg && umount /data && sync && bootctl status'
RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL=$R/release-kernel sh tools/dev/machine_identity.sh $R/identity-after.txt ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@192.168.0.106
L 'rm -r /var/lib/rach2-trial-4159 && mount -t ext2 /dev/disk0p6 /data && rm -r /data/trial4159 && test -f /data/crysis/game/Bin64/Crysis.exe && umount /data'
rm -rf build/flow/lenovo
git worktree remove $T
```

   Passes: the release in both slots and its record unchanged; `/data/crysis` still there; the case left nothing under `/data` but what it found. Stop the netconsole receiver.

8. Put `$R/flyby.out`, `trial.txt` and `after-flyby.txt` in `80-92/evidence/lenovo/`, add a section The Lenovo to [80-92.md](../80-92.md) with the load beside Linux's, and finish the task.
