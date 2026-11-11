# 80-9's Lenovo step

The step of 80-9.3 to 80-9.6: Crysis accepted on the Lenovo's GPU through [`crysis.py`](../../../tests/system/games/crysis.py), on trial boots of the milestone branch's kernel and a trial root from the data partition, with the game already installed in `/data/crysis` (80-6). The first boot runs the flyby through umu and directly, the suit's reboot from a save, the 32-bit game and the saves; a second boot of the same root is the restart; a third, after the root image is written again from its archive, as an update writes it, is the update. [80-9.md](../80-9.md) has the rehearsal in QEMU and the results.

It takes the screen and plays sound: the boot session's Weston drives the panel, and the game plays its sound through PulseAudio at volume 1. Post the notice of step 4 on #2665 and wait ten minutes before the first reboot.

On the Mac, by a loop of the owner's under the Lenovo's lock, in the main clone, after the manager's go on #2665, which names the head to build. Definitions:

```
mkdir build/flow/lenovo && echo 2665 > build/flow/lenovo/issue
L() { ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o ConnectTimeout=5 -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ServerAliveInterval=5 -o ServerAliveCountMax=3 root@192.168.0.106 "$@"; }
up() { while :; do if ping -c 1 -t 1 192.168.0.106 > /dev/null 2>&1; then echo "$(date +%T) up"; else echo "$(date +%T) down"; fi; sleep 1; done; }
back() { i=0; until L true 2> /dev/null; do i=$((i + 1)); [ $i -ge 180 ] && { echo "no answer"; return 1; }; sleep 2; done; L 'mixer vol 1'; echo "$(date +%T) answered"; }
reboot_now() { L 'sync; (nohup sh -c "sleep 2; reboot" > /dev/null 2>&1 &)'; }
T=build/worktrees/t2665
R=build/runs/t2665; mkdir -p $R
C='python3 /data/trial2665/probes/crysis.py'
```

`back` waits six minutes at most for SSH and sets the volume at once.

## Build

1. The kernel and bootctl on the Mac; the trial root and the bundle, which only an x86-64 Linux host makes, on the testing host in a copy of the loop's own, from the shared caches; the rehearsal's builds came from the caches in seconds, and a build after the go's changes is not measured:

```
git fetch origin && git worktree add $T HEAD-OF-THE-GO
make -C $T ARCH=x86_64 rach2 utils
(cd $T && export RACH2_TESTING_COPY=/home/testing/rach2/build/worktrees/t2665 &&
  sh tools/dev/testing.sh run 'make ARCH=x86_64 build/x86_64/images/gnu-test.ext2 build/tests/system/games/bundle.tar && gzip -1 -c build/x86_64/images/gnu-test.ext2 > build/t2665-root.ext2.gz' &&
  sh tools/dev/testing.sh get build/t2665-root.ext2.gz build/tests/system/games/bundle.tar)
shasum -a 256 $T/build/x86_64/rach2/kernel $T/build/x86_64/utils/sbin/bootctl $T/build/tests/system/games/bundle.tar
ls -l $T/build/x86_64/rach2/kernel
KS=$(shasum -a 256 $T/build/x86_64/rach2/kernel | cut -c1-16)
IS=$(gzip -dc $T/build/t2665-root.ext2.gz | shasum -a 256 | cut -c1-16)
tar -tf $T/build/tests/system/games/bundle.tar | grep -E 'crysis.py|wm.py|xplace'
```

   Passes: the kernel is under the slot, 4,194,304 bytes, or the larger slot the release has moved to (`bootctl status`); the bundle holds `crysis.py`, `wm.py` and `probes/xplace`. Record the four hashes and the image's whole SHA-256 on the issue.

## From the release

2. Read the release's kernel out of its slot, check both slots and keep the menu's bytes, as 80-5's step did; `KSIZE` and `KDIGEST` are the kernel's size and SHA-256 from the release's record, `/etc/rach2/release`:

```
L 'cat /etc/rach2/release; bootctl status; cat /proc/cmdline' | tee $R/before.txt
L 'mount -t ext2 /dev/disk0p6 /data && mkdir -p /data/trial2665 && df -k /data && ls /data/crysis /data/crysis/saves/SaveGames'
L "s=\$(bootctl status | sed -n 's/^bootctl: candidate: first sector \([0-9]*\),.*/\1/p'); dd if=/dev/bootpart bs=512 skip=\$s count=8192 2>/dev/null | head -c KSIZE > /data/trial2665/release-kernel && sha256sum /data/trial2665/release-kernel"
L 'cat /data/trial2665/release-kernel' > $R/release-kernel; shasum -a 256 $R/release-kernel
L 's=$(bootctl status | sed -n "s/^bootctl: boot.cfg: first sector \([0-9]*\),.*/\1/p"); dd if=/dev/bootpart bs=512 skip=$s count=16 of=/data/trial2665/original.cfg 2>/dev/null; sha256sum /data/trial2665/original.cfg'
RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL=$R/release-kernel sh tools/dev/machine_identity.sh $R/identity-before.txt ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@192.168.0.106
```

   Passes: the release booted from its committed candidate, no trial armed; `/data` has more than 30 GiB free; both digests `KDIGEST`, and machine_identity finds the kernel in both slots (if the fallback differs, `L 'bootctl fallback'` first); `/data/crysis` holds `game`, `compat` and `saves`. The menu's read needs its 16 sectors contiguous, as `bootctl status` shows; do not go on otherwise. Note which checkpoint saves `SaveGames` lists: `default_Contact_crysis.CRYSISJMSF` is the first level's start, before the suit's reboot.
3. Stage the files on the data partition and check each:

```
L "cat > /data/trial2665/kernel-$KS" < $T/build/x86_64/rach2/kernel
L 'cat > /dev/shm/bootctl && chmod 755 /dev/shm/bootctl' < $T/build/x86_64/utils/sbin/bootctl
L "gunzip > /data/trial2665/root-$IS.img" < $T/build/t2665-root.ext2.gz
L 'mkdir -p /data/trial2665/probes && tar -xf - -C /data/trial2665/probes' < $T/build/tests/system/games/bundle.tar
L "sha256sum /data/trial2665/kernel-$KS /data/trial2665/root-$IS.img /dev/shm/bootctl /data/trial2665/probes/crysis.py; df -k /data; sync"
```

   Passes: the digests of the build, the image's whole, not its first 16 digits.
4. Post on #2665 "Lenovo notice:" with the time of the first reboot, ten minutes on, and the end: three trial boots, each with the boot session's Weston on the panel, the flyby four times through umu and four times directly, the first level loaded six times, the 32-bit game's flyby once, the game's sound at volume 1 throughout, ending by about three hours after the first reboot. Wait the ten minutes; everything below, the reboots among them, comes after them.

## The first boot: 80-9.3 to 80-9.6's first half

5. Stage the kernel and arm one boot of the trial root with [`78-10d/trialroot.sh`](../../78-intel/78-10d/trialroot.sh), the driver with its display, the trace of missing calls, the IOMMU's translation off as Linux's runs had it, and the kernel's log to the Mac. `IP` is the Mac's `ipconfig getifaddr en0`, `STATION` the station address the Lenovo's `/proc/net/arp` holds for it:

```
nc -lu 6666 > build/runs/netconsole-2665-$(date +%Y%m%dT%H%M).log &   # with the tool's notice; stop it after step 15
L 'cat /proc/net/arp'
W="rach2.i915 rach2.trace=enosys intel_iommu=off intremap=off netconsole=6665@192.168.0.106/,6666@IP/STATION"
L "bootctl kernel /data/trial2665/kernel-$KS && umount /data && sync"
L "sh -s /trial2665/root-$IS.img $W" < $T/dev-log/78-intel/78-10d/trialroot.sh | tee $R/armed-1.txt
```

   Passes: the block's two `multiboot2` lines are `rach2.rootimage=GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,6:/trial2665/root-IS.img rach2.slot=trialroot`, the release's other words and the words of `W`; `armed: the next boot tries trialroot once`; `next_entry: trialroot`.
6. `up > $R/ping-1.txt &`, `reboot_now`, `back`, then:

```
L 'cat /proc/cmdline; bootctl status; grep -e " / " -e " /data " /proc/mounts; cat /opt/proton/version; weston --version' | tee $R/trial-1.txt
L dmesg > $R/dmesg-1.txt; grep -E 'i915|vtd: fault|root: image' $R/dmesg-1.txt
L 'grep -E "GL renderer|Output .* video modes" /var/log/services | tail -3'
L 'su -s /bin/sh desktop -c "crysis.sh verify"' | tail -1
L "$C prepare /data/trial2665/out" | tee $R/prepare.out
```

   Passes: `booted: trialroot`; `/dev/loop0 / ext2 rw` and `/dev/disk0p6` on `/data`; GE-Proton11-12; the rebuilt Weston, `14.0.2-1+rach2.1`; `root: image: /trial2665/root-IS.img on /dev/disk0p6` and `i915: registered` in the log, with no `wedged`, `timeout after` or `vtd: fault`; Weston's `GL renderer: Mesa Intel(R) HD Graphics 4400 (HSW GT2)`; `crysis: result: all 2358 files match`; `crysis: session: output eDP-1, renderer Mesa Intel(R) HD Graphics 4400 (HSW GT2)`, the output's name as the log gives it, `volume: vol 1:1` and `CRYSIS PREPARED`. `prepare` records the game's owner, `1000 1000`, in `/data/crysis/accept-owner` and gives the tree to `desktop`, whose boot session runs the game; step 14 gives it back. Kill `up`. If SSH does not answer within 6 minutes, the machine waits for the owner; record what `ping-1.txt` shows on this issue and stop.
7. 80-9.3, the flyby through umu and directly, four loops each, each run printed as it goes:

```
L "$C flyby umu umu-1" 2>&1 | tee $R/umu-1.out
L "$C flyby direct direct-1" 2>&1 | tee $R/direct-1.out
L 'mixer vol; dmesg | grep -E "i915: (reset|hangcheck)|wedged|vtd: fault|vm: fault"'
```

   Passes, 80-9.3's check: `check: loop0` to `loop3` ok on both, each at 24 frames a second or more, printed beside Linux's (umu 45.02, 46.49, 45.78, 45.66; direct 45.17, 46.54, 45.97, 45.92); `check: wined3d` and `check: fsync` ok; `crysis: result: ... ok`; the mixer at 1 (`volume.txt` in each run's directory lists any change the game made, with its time); no reset, wedge or fault. A run that writes nothing for ten minutes is ended by the script and fails; read its `game.out` before going on.
8. 80-9.4, the suit's reboot from the checkpoint before it, through umu. `SAVE` is `default_Contact_crysis.CRYSISJMSF` when step 2 listed it; otherwise first run `L "$C save s0"` and use `default_accept_s0.CRYSISJMSF`:

```
L "$C hud hud-1 SAVE umu" 2>&1 | tee $R/hud-1.out
L 'cat /data/trial2665/out/hud-1/after.png' > $R/hud-1-after.png
```

   Passes, 80-9.4's check: `check: load`, `check: lines` and `check: gamerules` ok (`hudShow="1"`, no `hudBroken`), the dialog line printing Show some 4.7 s after the boot sequence, as Linux's 4,693 ms; and `$R/hud-1-after.png`, read on the Mac, shows the HUD: the minimap, the ammunition, the energy bar and the crosshair.
9. 80-9.5, the 32-bit game's flyby, one loop:

```
L "$C flyby 32 bits32-1 1" 2>&1 | tee $R/bits32-1.out
```

   Passes, 80-9.5's check: `check: loop0` and `check: loops` ok, `1 of 1 loops finished`, and `crysis.sh run --32` in the run's `command.txt`. The loop's rate is not gated by the plan; it is recorded beside the 64-bit game's.
10. 80-9.6's first half: a save made through umu, loaded through the existing prefix and through a new one:

```
L "$C save s1" 2>&1 | tee $R/save-s1.out
L "$C load load-1 default_accept_s1.CRYSISJMSF umu" 2>&1 | tee $R/load-1.out
L "$C load load-new default_accept_s1.CRYSISJMSF direct /data/crysis/compat-new" 2>&1 | tee $R/load-new.out
```

   Passes: `check: saved` with the save's SHA-256, recorded in `/data/crysis/accept.json`; each load's `check: bytes`, `check: load` and `check: prefix` ok, the first `used as it was`, the second `made new`.

## The restart: 80-9.6

11. The boot after the trial is the release's; arm the trial root once more and boot it, as a restart of the trial root is:

```
up > $R/ping-2.txt &
reboot_now; back; L 'cat /proc/cmdline; bootctl status' | tee $R/between-1.txt
L "sh -s /trial2665/root-$IS.img $W" < $T/dev-log/78-intel/78-10d/trialroot.sh | tee $R/armed-2.txt
reboot_now; back
L 'cat /proc/cmdline; bootctl status' | tee $R/trial-2.txt
L "$C prepare /data/trial2665/out"
L "$C load load-restart default_accept_s1.CRYSISJMSF umu" 2>&1 | tee $R/load-restart.out
```

   Passes: `rach2.slot=fallback` or the release's candidate between the trials, the trial line again after; the load's three checks ok, the prefix `used as it was`. Kill `up`.

## The update: 80-9.6

12. Write the root image again from its archive, as an update writes the root, and boot it: the root is new, and only `/data` holds the prefix, the save and the game. umu's runtime, which lives in the root's home, is unpacked again:

```
reboot_now; back; L 'cat /proc/cmdline' | tee $R/between-2.txt
L "mount -t ext2 /dev/disk0p6 /data && gunzip > /data/trial2665/root-$IS.img && sha256sum /data/trial2665/root-$IS.img && umount /data && sync" < $T/build/t2665-root.ext2.gz
L "sh -s /trial2665/root-$IS.img $W" < $T/dev-log/78-intel/78-10d/trialroot.sh | tee $R/armed-3.txt
reboot_now; back
L 'cat /proc/cmdline; bootctl status; ls /home/desktop/.local/share/umu 2>&1' | tee $R/trial-3.txt
L "$C prepare /data/trial2665/out"
L "$C load load-update default_accept_s1.CRYSISJMSF umu" 2>&1 | tee $R/load-update.out
L "$C load load-update-new default_accept_s1.CRYSISJMSF umu /data/crysis/compat-new2" 2>&1 | tee $R/load-update-new.out
```

   Passes: the image's whole SHA-256 as in step 1; `/home/desktop/.local/share/umu` absent before the loads, so the root is new; `rach2-game: umu: unpacked the Steam Linux Runtime` in the first load's `game.out`; both loads' checks ok, the first `used as it was`, the second `made new`. 80-9.6's check is then whole: the same save, by its SHA-256, loaded after the restart and after the update, through the existing prefix and through new ones.

## Put it back

13. Fetch the outputs and the missing calls:

```
L 'tar -cf - -C /data/trial2665 out' | tar -xf - -C $R
L 'cat /data/trial2665/out/*/missing.txt | sort -u' | tee $R/missing.txt
```

14. Give the game back to its owner and remove the step's prefixes:

```
L "$C restore && rm -rf /data/crysis/compat-new /data/crysis/compat-new2 && ls -ln /data/crysis"
```

   Passes: `crysis: restored: /data/crysis to 1000:1000`, and `ls -ln` shows uid 1000.
15. The release in both slots, as 80-5's step did:

```
reboot_now; back; L 'cat /proc/cmdline; bootctl status'
L 'mount -t ext2 /dev/disk0p6 /data && bootctl config /data/trial2665/original.cfg && bootctl kernel /data/trial2665/release-kernel && umount /data && sync'
reboot_now; back
L 'bootctl commit && bootctl fallback && bootctl status'
RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL=$R/release-kernel sh tools/dev/machine_identity.sh $R/identity-after.txt ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@192.168.0.106
L 'mount -t ext2 /dev/disk0p6 /data && rm -rf /data/trial2665 && ls /data && umount /data; mixer vol'
rm -rf build/flow/lenovo
(cd $T && RACH2_TESTING_COPY=/home/testing/rach2/build/worktrees/t2665 sh tools/dev/testing.sh run 'rm -f build/t2665-root.ext2.gz')
```

   Passes: the release in both slots and its record unchanged; `/data/crysis` and `/data/kept` still there; the volume 1. Stop the `nc` of step 5.

16. Put the outputs in `80-9.md` and `80-9/evidence/`, each number beside Linux's; post on #2665 the missing calls sorted by the owner's rule (the program goes on by its own fallback, or it fails or loses a function); close 80-9.3 to 80-9.6 with their results; then rebase and merge the task. A check that fails is a fault to fix in this task, with the step run again from the boot it needs.
