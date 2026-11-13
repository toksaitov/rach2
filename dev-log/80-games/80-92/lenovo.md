# 80-92's Lenovo step

One load of Crysis's island on the Lenovo, on one trial boot of the milestone's kernel on the installed root, measured by the game's own log, beside Linux's 19.7 s on the same machine ([80-12.md](../80-12.md)). The load is the first part of Linux's flyby, `+map island`, which the release's native case `games/crysis.flyby-direct` runs: `native/crysis.sh flyby direct` on a private copy of the game, which reads the owner's files on the data partition read-only. Its line `crysis: loaded in S s` is the figure. In QEMU the load is within Linux's range ([80-92.md](../80-92.md#the-sixth-pass)).

The step takes the panel and plays the game's sound at volume 1 for about three minutes, the session's restart and the flyby's first two loops. Post the notice on #4159 ten minutes before the first reboot: "Lenovo notice: from HH:MM for about 15 minutes, two reboots and Crysis's island flyby on the panel, sound at volume 1." Every reboot the notice names waits the ten minutes too.

The kernel's change from the last kernel the Lenovo ran is 80-94 to 80-97 and this task's processor-time account. The account is arithmetic on the tick's counts and touches no device; 80-97's one-shot timer and idle tick are on the milestone and have run on no Lenovo boot this task knows of. So the boot waits for the manager's go on #4159, which names the head to build.

On the Mac, by a loop of the owner's, in the main clone, after the go. Definitions:

```
mkdir build/flow/lenovo && echo 4159 > build/flow/lenovo/issue
L() { ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o ConnectTimeout=5 -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ServerAliveInterval=5 -o ServerAliveCountMax=3 root@192.168.0.106 'test "$(cat /sys/class/net/eth0/address)" = 28:d2:44:7e:e0:af && grep -q S6P2NX0T381638Z /proc/disk && '"$*"; }
back() { i=0; until L "test \"\$(cat /proc/sys/kernel/random/boot_id)\" != \"$BOOT_ID\"" 2> /dev/null; do i=$((i + 1)); [ $i -ge 180 ] && { echo "no answer"; return 1; }; sleep 2; done; L 'mixer vol 1'; echo "$(date +%T) answered"; }
T=build/worktrees/t4159
R=build/runs/t4159; mkdir -p $R
```

`back` waits six minutes at most for SSH on a new boot, identified by `boot_id`, and sets the volume at once.

## Build

1. The kernel, on the Mac:

```
git fetch origin && git worktree add $T HEAD-OF-THE-GO
make -C $T ARCH=x86_64 rach2
ls -l $T/build/x86_64/rach2/kernel
KS=$(shasum -a 256 $T/build/x86_64/rach2/kernel | cut -c1-16)
```

   Passes: the head holds the account's fix (`git -C $T log --oneline -1 --grep 'last scheduler edge'` names a commit) and the kernel is under 4,194,304 bytes, the slot. Record its SHA-256 on the issue.

## On the Lenovo

2. The release, its record and both slots, and the menu's bytes:

```
L 'cat /etc/rach2/release; bootctl status; cat /proc/cmdline; pgrep -l weston' | tee $R/before.txt
L 'mount -t ext2 /dev/disk0p6 /data && mkdir -p /data/trial4159 && df -k /data && ls /data/crysis'
L 's=$(bootctl status | sed -n "s/^bootctl: boot.cfg: first sector \([0-9]*\),.*/\1/p"); dd if=/dev/bootpart bs=512 skip=$s count=16 of=/data/trial4159/original.cfg 2>/dev/null; sha256sum /data/trial4159/original.cfg'
```

   Then read the release's kernel out of the candidate slot into `/data/trial4159/release-kernel` and `$R/release-kernel`, with the size and digest the release record gives, and check both slots with `tools/dev/machine_identity.sh`, as steps 5 of [80-27/lenovo.md](../80-27/lenovo.md) does with `trial3513` read as `trial4159`.

   Passes: the release booted from its candidate slot with no trial armed; Weston running, which the native case needs; `/data/crysis` present; the release's kernel in both slots. If the fallback differs, `L 'bootctl fallback'` first. The menu's read needs its 16 sectors contiguous, as `bootctl status` shows; do not go on otherwise.

3. The kernel, staged and checked, and the case's files in a workspace of their own:

```
L "cat > /data/trial4159/kernel-$KS" < $T/build/x86_64/rach2/kernel
L 'cat > /dev/shm/bootctl && chmod 755 /dev/shm/bootctl' < $T/build/x86_64/utils/sbin/bootctl
L 'rm -rf /root/t4159 && mkdir -p /root/t4159/private'
for f in tests/system/games/crysis.py tests/system/games/wm.py tests/native/crysis.sh; do L "cat > /root/t4159/$(basename $f)" < $T/$f; done
L "sha256sum /data/trial4159/kernel-$KS /root/t4159/*; sync"
```

   Passes: the kernel's digest is the build's. Build bootctl with `make -C $T ARCH=x86_64 utils` if it is missing; use `/dev/shm/bootctl` for the commands below when the release's bootctl lacks `config`.

4. The netconsole receiver: read the Mac's address with `ipconfig getifaddr en0` and its station address from the Lenovo's `/proc/net/arp`, set `NET=netconsole=6665@192.168.0.106/,6666@IP/STATION`, and start `nc -lu 6666 > $R/netconsole.log` in the tool's background session, as the owner's user, keeping its session or PID until step 7.

5. Arm one boot of the kernel from the candidate slot, with the netconsole word, as the `machine` skill's Trial boots says:

```
L "bootctl kernel /data/trial4159/kernel-$KS"
CMD=$(L 'cat /proc/cmdline')
printf 'set next_entry=\nsave_env next_entry\nsearch --no-floppy --fs-uuid --set=root 1ACE-F00D\nmultiboot2 /boot/kernel %s %s\nboot\n' "$CMD" "$NET" | L 'cat > /data/trial4159/trial.cfg && bootctl config /data/trial4159/trial.cfg && umount /data && sync && bootctl status' | tee $R/armed.txt
```

   Passes: `bootctl status` names the staged kernel and `next_entry` `candidate`.

6. The boot and the load, after the notice's ten minutes:

```
BOOT_ID=$(L 'cat /proc/sys/kernel/random/boot_id'); L '(nohup sh -c "sleep 2; reboot" > /dev/null 2>&1 &)'; back
L 'cat /proc/cmdline; uname -a; bootctl status; pgrep -l weston' | tee $R/trial.txt
L 'cd /root/t4159 && unshare -m --propagation private sh crysis.sh flyby direct' 2>&1 | tee $R/flyby.out
L 'mixer; dmesg | grep -E "i915: (reset|hangcheck)|wedged|vtd: fault|vm: fault|panic"; cat /proc/stat | head -1' | tee $R/after-flyby.txt
```

   Passes: the line of step 5 with the netconsole word and the new kernel's build string; `crysis: loaded in S s`, then `crysis-case: result: pass`; the mixer at 1; no reset, wedge or fault. The figure is S beside Linux's 19.7 s and Rach2's 41.00 s on the GPU before (80-12). If SSH does not answer within six minutes, the machine waits for the owner: record the netconsole log on #4159 and stop.

## Put it back

7. Stop the receiver. Then the menu and the release in both slots:

```
BOOT_ID=$(L 'cat /proc/sys/kernel/random/boot_id'); L 'sync; (nohup sh -c "sleep 2; reboot" > /dev/null 2>&1 &)'; back
L 'mount -t ext2 /dev/disk0p6 /data && bootctl config /data/trial4159/original.cfg && bootctl kernel /data/trial4159/release-kernel && umount /data && sync'
BOOT_ID=$(L 'cat /proc/sys/kernel/random/boot_id'); L '(nohup sh -c "sleep 2; reboot" > /dev/null 2>&1 &)'; back
L 'bootctl commit && bootctl fallback && bootctl status'
RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL=$R/release-kernel sh tools/dev/machine_identity.sh $R/identity-after.txt ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@192.168.0.106
L 'rm -rf /root/t4159 && mount -t ext2 /dev/disk0p6 /data && rm -rf /data/trial4159 && ls /data && umount /data'
rm -rf build/flow/lenovo
git worktree remove $T
```

   Passes: the release in both slots and its record unchanged; `/data/crysis` still there; the case left nothing under `/data` but what it found.

8. Put `$R/flyby.out`, `trial.txt` and `after-flyby.txt` in `80-92/evidence/lenovo/`, add a section The Lenovo to [80-92.md](../80-92.md) with the load beside Linux's, and finish the task.
