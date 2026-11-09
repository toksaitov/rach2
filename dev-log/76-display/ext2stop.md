# The forced ext2 test, against Debian 13

Task 76-15a, 2026-11-09, on the testing PC's WSL: Rach2 at the head of `task/2583`, `tests/run ext2stop` on i686 and x86-64 under TCG; Debian 13's generic cloud image (`debian-13-generic-amd64.qcow2`, SHA-512 `1207e6e3...57fee4b`, the image the second run used) under KVM, kernel `6.12.115+deb13.1-amd64`, in QEMU 10.2.1.

## What ran

Both systems got the same volume and the same commands. The volume is one MBR partition of 64 MiB at sector 2048, made by the host's mke2fs 1.47.2 with the installed root's features (`tools/image/mkext2.sh`), a fixed identifier and hash seed:

```
mkmbr --lba --id 1a26e015 DISK 2048:131072:83
mke2fs -q -F -t ext2 -b 4096 -I 256 \
    -O sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,extra_isize \
    -E offset=1048576,hash_seed=52616368-3200-4578-7432-000000000015 \
    -U 52616368-3200-4578-7432-000000000015 -L stop DISK 64M
```

The workload is the one `tests/cases/machine/ext2stop_scenario.c` types: the mount; eight sentinels of 64 KiB, synced; four files of 40000 bytes opened by the shell, their inode numbers printed, removed and synced; then two writers that each make a file of 20000 bytes, append it to a log and remove the file four before it, and a loop of `sync`. The stop came when both writers had printed their hundredth file: QMP's `quit` on both systems. The control ran the same and powered off with everything running.

Linux does not mount this volume as `ext2`: `extra_isize` is not among the features its ext2 mode takes (`EXT4-fs (vdb1): couldn't mount as ext2 due to feature incompatibilities`). Debian mounts it by type detection as `ext4` without a journal, which is how it treats the installed root's volume; Rach2 mounts it as `ext2`.

| | Rach2 | Debian 13 |
|---|---|---|
| Mount | `/dev/disk0p1 /mnt/s ext2 rw 0 0` | `/dev/vdb1 /mnt/s ext4 rw,relatime 0 0` |
| Held inodes | 21, 22, 23, 24 | 21, 22, 23, 24 |

## The stop

`e2fsck -fn` on a copy of each stopped volume, taken before anything mounted it:

| Finding | Rach2, i686, first run | Debian 13 |
|---|---|---|
| `Deleted inode N has zero dtime` | 21, 22, 23, 24 | 21, 22, 23, 24 |
| `Inode bitmap differences` | `-(21--24)` | `-(21--24)` |
| `Block bitmap differences` | `-(1174--1213) -(3193--3197)` | `-(1664--1703)` |
| Free counts wrong | none | blocks 15347 against 13750 counted, inodes 16373 against 16348 |
| Exit status | 4 | 4 |

Each system leaves the documented state of an unclean stop (`rach2/sys/fs/ext2.Readme.md`, The check, and the strict mount's list in Design): the four held inodes with no name, zero deletion time and their bits, and their 40 blocks. Rach2 left five more lost blocks, marked used with no pointer to them, which is its allocation order (a block's bit goes before the pointer); Debian left none this time but its free counts stale, since it keeps them in memory between commits. Neither left a block that a file names and the bitmap calls free, an entry naming a free inode, or a sentinel changed.

On x86-64 the stop left only the held inodes and their 40 blocks: the check said `ext2: check: 40 lost blocks, 4 orphan inodes, kept for e2fsck`, and e2fsck -fn passed the verdict.

Rach2's next writable mount came before e2fsck. Its check said `ext2: check: 45 lost blocks, 4 orphan inodes, kept for e2fsck` (the 40 blocks of the held inodes and the 5), mounted the volume writable with the error bit set, read every sentinel back, took a write and unmounted; e2fsck -fn then found the same as before the mount.

## The control, and the fault it found

Debian's control powered off clean: systemd failed its first unmount of `/mnt/s` while the workload held it, then `systemd-shutdown` ended every process, remounted it read-only and unmounted it, and e2fsck -fn exited 0.

Rach2's control did not. Its `poweroff` left the volume's superblock state 0, the valid bit clear, and the four held inodes orphaned: the next mount checked it (`40 lost blocks, 4 orphan inodes`) and e2fsck -fn exited 4. Two limits caused it, both of which Linux does not have:

* init stopped the machine without ending any process (`utils/init.c`, `shutdown_handler`), so the files they held stayed open and a removed one was never freed, and `kill(2)` refused a pid of -1, which an init uses to end them.
* The kernel's shutdown wrote out and marked clean the root alone (`kernel_shutdown` in `rach2/kernel.c`); a volume placed by mount(2) kept whatever the last sync wrote, with its valid bit clear.

The fix: `kill(-1, sig)` posts to every process the caller may signal but its own, its PID namespace's init and the kernel's threads, as Linux's does; init sends SIGTERM and then SIGKILL that way and reaps until none is left, waiting at most 1 s and then 5 s; and `kernel_shutdown` prepares each writable mount on a disk, newest first, as its unmount would, before the root, logging `shutdown: SOURCE: unmounted`. With it the control's power off says `shutdown: /dev/disk0p1: unmounted`, leaves state 1, needs no check at the next mount, and passes e2fsck -fn, as Debian's does.

## Running the Debian side again

The two scripts below ran in a scratch directory under `build/tmp`; `run.sh stop` and `run.sh control` each make the volume and boot the image through a throwaway overlay, with a NoCloud seed whose `runcmd` runs `workload.sh`. Debian's GRUB resets for ever without a VGA device, so the machine has `-vga std`. 76-16a makes this guest a fixture.

`workload.sh`:

```
#!/bin/sh
exec > /dev/ttyS0 2>&1
echo EXT2STOP BEGIN $(uname -r)
mkdir -p /mnt/s && mount /dev/vdb1 /mnt/s || { echo MOUNT FAILED; exit 1; }; grep ' /mnt/s ' /proc/mounts; echo MOUNT""END
mkdir /mnt/s/keep && i=0 && while [ $i -lt 8 ]; do yes keep$i | head -c 65536 > /mnt/s/keep/f$i; i=$((i+1)); done && sync && echo KEEP""OK; echo KEEP""END
i=0; while [ $i -lt 4 ]; do yes held$i | head -c 40000 > /mnt/s/held$i; i=$((i+1)); done; exec 3</mnt/s/held0 4</mnt/s/held1 5</mnt/s/held2 6</mnt/s/held3; stat -c HELD=%i /mnt/s/held0 /mnt/s/held1 /mnt/s/held2 /mnt/s/held3 && rm /mnt/s/held0 /mnt/s/held1 /mnt/s/held2 /mnt/s/held3 && sync && echo HELD""OK; echo HELD""END
w() { n=0; while :; do yes $1$n | head -c 20000 > /mnt/s/w$1$n; cat /mnt/s/w$1$n >> /mnt/s/log$1; rm -f /mnt/s/w$1$((n-4)); n=$((n+1)); [ $n -eq 100 ] && echo WRITING$1; done; }; w a & w b & while :; do sync; done & echo WRITERS""UP
if [ "$1" = control ]; then
    until [ "$(stat -c %s /mnt/s/loga 2>/dev/null || echo 0)" -ge 2000000 ] &&
        [ "$(stat -c %s /mnt/s/logb 2>/dev/null || echo 0)" -ge 2000000 ]; do sleep 0.1; done
    echo CONTROL POWEROFF
    systemctl poweroff
fi
wait
```

`run.sh MODE`, from the clone's `build/tmp/NAME`:

```
#!/bin/sh
set -e
mode=$1
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
base=$repo/build/cache/debian13/debian-13-generic-amd64.qcow2
cd "$here"
rm -f disk-$mode.img ov-$mode.qcow2 seed-$mode.iso serial-$mode.log qmp-$mode.sock
truncate -s 68157440 disk-$mode.img
# mkmbr and mke2fs as above, on disk-$mode.img
qemu-img create -q -f qcow2 -b "$base" -F qcow2 ov-$mode.qcow2
# seed: meta-data, network-config `network: {config: disabled}`, and user-data
# writing workload.sh to /root and running `sh /root/workload.sh MODE`,
# made with `xorriso -as genisoimage -volid cidata -joliet -rock`
qemu-system-x86_64 -accel kvm -cpu host -m 2048 -smp 2 -nodefaults -vga std -display none -no-reboot \
    -serial file:serial-$mode.log -qmp unix:qmp-$mode.sock,server=on,wait=off \
    -drive file=ov-$mode.qcow2,if=virtio,format=qcow2 \
    -drive file=disk-$mode.img,if=virtio,format=raw \
    -drive file=seed-$mode.iso,if=virtio,format=raw,readonly=on &
qemu=$!
if [ "$mode" = stop ]; then
    until grep -q WRITINGa serial-$mode.log && grep -q WRITINGb serial-$mode.log; do sleep 0.2; done
    # QMP: qmp_capabilities, then quit
fi
wait $qemu || true
```

Then `e2fsck -fn 'disk-MODE.img?offset=1048576'` on a copy of the disk.
