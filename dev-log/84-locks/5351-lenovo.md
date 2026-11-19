# 84-5351: the prepared Lenovo trial of vhost-net

The task is ready for the manager's go once Codex has reviewed the fixes of its first review, not booted. One trial boot of the task's x86-64 kernel on the 84-58 trial root, whose QEMU 10.0.13 runs the KVM suite's Debian guest with `-netdev tap,vhost=on`. The step is silent and takes no screen; it needs no notice.

## What it proves

* `/dev/vhost-net` exists as 10:238 and `vhostmusl` drives it on the Lenovo as on QEMU: `VHOSTMUSL OK`, 26 checks.
* QEMU's `vhost=on`, unchanged, runs a Linux guest whose virtio-net frames go through the kernel: the guest loads `virtio_net`, takes 10.9.0.2/24 and pings the tap's 10.9.0.1 ten times with 56-byte and ten times with 1400-byte payloads, and prints `GUESTNET OK` only when both batches had all ten answered.
* The kernel moved those frames, not QEMU. QEMU opens `/dev/vhost-net` at its start but falls back to its own relay, with `unable to start vhost net` or `falling back on userspace virtio` on its stderr, when the device refuses the queues. So the step requires both that QEMU's stderr has no such line and that the device's own count, which it logs when QEMU lets the tap go, `vhost-net: detached: tap5351: N frames to the guest, M from it`, has N and M of at least 20 each, the pings' replies and requests.

## Readiness for the go

vhost-net touches no device register, DMA, interrupt or mapping that gives a program a device. It is software: a character device, one kernel thread at ordinary priority, and the receive path the tap already calls under `tap_lock` with interrupts off. Every loop in it is bounded: a descriptor chain by the ring's size, a transmit pass by 128 frames, the drain of the tap's queue by its 1024 frames, a refill by the changes of the page under way. The device's lock, `vnet`, nests inside `tap_lock` and outside eventfd's, as the lock-order table of `rach2/sys/proc/Readme.md` gives in row 11; nothing under it sleeps. The guest's memory is held through pins that follow QEMU's address space (`PIN_FOLLOW`): each holds a page from the thread's refill to the next change of that page, a balloon's discard, a write-back's write-protection, an unmap, and lets it go after taking `vnet` once, so the device never writes a frame QEMU no longer maps. A guest's bad ring stops that ring and signals its error eventfd; it cannot reach the host's memory beyond the table's regions of QEMU's own memory. Booting the kernel only starts the thread and registers the node; nothing runs until a program opens the device.

The host test `tests/host/vhost_net_hosttest.c` runs the requests, the memory table, receive, transmit and teardown under ASan and UBSan, 0 failed. QEMU on WSL ran `vhostmusl` to `VHOSTMUSL OK`, 26 of 26, on the i686 kernel, the x86-64 kernel, and the i686 image under the x86-64 kernel, which passes vhost's requests from an i386 program untranslated. The Debian guest's `guestnet=` check ran to `GUESTNET OK` under QEMU's own user network on WSL, 20 of 20 pings answered.

Codex reviewed the task at b290c9a51 and found seven faults, all fixed on `task/5351` since: torn ring indices, pins left stale by a discard, a shared disk-backed RAM refused, a tap or kick closed by the program stopping a ring, `VHOST_SET_VRING_BUSYLOOP_TIMEOUT` refused, this step's want of proof that the kernel moved the frames, and the guest's ping check passing on one reply in ten. The owner's order of 2026-11-19 is one Codex review of those fixes, then this trial on a clean answer; the fixes have had no test run, as AGENTS.md's Testing section holds a commit under review, only the worker's trial boot in QEMU on WSL, where `vhostmusl` ran twice on each kernel at 799a4483a to `VHOSTMUSL OK`, 26 of 26 each time, and the device logged `3 frames to the guest, 1 from it` at each detach.

Codex's review of those fixes at a5a9c6a54 closed all seven and found four new faults, fixed on `task/5351` since and waiting for one Codex review: `VHOST_SET_VRING_KICK` took the eventfd and its descriptor from two lookups, between which a close could leave the second NULL; the thread read a ring's refill request without the lock the receive path writes it under; a refill request of a replaced memory table was resolved against the new one, and a refill the table outran broke the ring; and `vhostmusl` could fail on a call signalled just after the used index, or pass the second datagram on an earlier frame's call. The thread now reads each request under `vnet` with the table's generation it was made under, lets a request of an older table go and kicks the ring to look again, and breaks a ring only for a refill that failed against the table still in force; the probe counts the receive ring's calls and passes a frame only once they cover every used entry it has seen, since the device signals once a frame. These change no path to a device, so the readiness above holds.

Codex's review of `a5a9c6a54..9e22f50d1` settled three of the four and the node's mode, and found three points, fixed on `task/5351` since: `vhostmusl` stopped at the matching datagram and took calls only up to the used entries it had seen, so a later frame's call could stand in for the datagram's own, and now reads the call until the calls read cover the used index read after them, which a missing call never lets them reach; this step expected the node's mode 0600; and the host test's eventfd stand-in found an entry by a range test of pointers, undefined for an entry outside its array, and now compares each by equality. None touches the kernel.

Codex's reviews of `9e22f50d1..69d81165e` and of the fixes after it found that `vhostmusl` could still pass with a call missing: an extra call for one frame, read with another frame's used entry, could cover it, whenever another frame reached the ring while the probe counted. The probe now offers one receive buffer at a time; after each frame's call it sets the receive call again, which takes the lock the device signals under, reads what the eventfd holds without waiting, and requires the calls read to equal the used entries before it offers the next buffer, so a missing or extra call fails the frame it belongs to; after the detach the counts must still be equal. It passes over any number of the stack's other frames, and a frame or call that never comes leaves it waiting, for the suite's watchdog, as the suites set no clock. None touches the kernel.

## Build

On the owner's Mac, before taking the lock. `HEAD5351` is the head of `task/5351` the go names; the trial builds it rebased onto `milestone/76-display` as it stands.

```
cd /Users/toksaitov/Desktop/rach2/build/worktrees/codex
T=$PWD/build/worktrees/5351-trial
R=$PWD/build/runs/5351
mkdir -p "$R"
git fetch origin
git worktree add --detach "$T" origin/task/5351
git -C "$T" rebase origin/milestone/76-display
make -C "$T" ARCH=x86_64 rach2 utils tests
make -C "$T" build/kvm/guests/debian/initrd
cp "$T/build/x86_64/rach2/kernel" "$R/trial-kernel"
cp "$T/build/x86_64/rach2/kernel.debug" "$R/trial-kernel.debug"
cp "$T/build/x86_64/utils/sbin/bootctl" "$R/bootctl"
mkdir -p "$R/bundle"
cp "$T/build/x86_64/tests/musl/vhostmusl" "$T/tests/kvm/kvm-boot.sh" \
   "$T/build/kvm/guests/debian/vmlinuz" "$T/build/kvm/guests/debian/initrd" "$R/bundle/"
cat > "$R/bundle/ifup" <<'EOF'
#!/bin/sh
# ifup: QEMU runs this with the tap's name once it has made it.
ip addr add 10.9.0.1/24 dev "$1" && ip link set "$1" up
EOF
chmod 755 "$R/bundle/ifup"
tar -cf "$R/bundle.tar" -C "$R/bundle" .
shasum -a 256 "$R/trial-kernel" "$R/bootctl" "$R/bundle"/*
KS=$(shasum -a 256 "$R/trial-kernel" | cut -c1-16)
IS=d4d1da30b4f6cb50
```

The root is 84-58's `gnu-test.ext2`, 24 GiB, whole-image SHA-256 `d4d1da30b4f6cb509d2e9e8122aea4749cd80917438a012ed0228cdbeab95a8c`, which `4893-lenovo.md` names with the Mac's retained `root.ext2.gz` and the command that fetches it from the testing host. Use the Mac's copy when its decompressed digest matches, or the image already on the Lenovo when it reads the same digest whole there; fetch it otherwise. The kernel must fit the candidate slot that `bootctl status` gives.

Before the lock, boot the trial kernel and this root in QEMU by the Lenovo's path, `rach2.rootimage=` on a loop device on an ext2 partition, as `4893-lenovo.md`'s preparation did, and run steps 2 and 3 of Run below there with `-a tcg` added to `kvm-boot.sh`: both must pass.

## Take the machine, stage and boot

As `4893-lenovo.md`'s sections "Take the machine and retain the release" and "Stage, arm and boot", with `5351` for `4893`, this task's `$R`, and on the trial root's line no driver words: `L "sh -s /trial5351/root-$IS.img intel_iommu=off intremap=off $NET" < "$T/dev-log/78-intel/78-10d/trialroot.sh"`. Unpack the bundle with `L 'mkdir -p /data/trial5351/b && tar -xf - -C /data/trial5351/b' < "$R/bundle.tar"` and check its digests on the machine against the build's. Start the netconsole receiver before arming, as the machine skill says.

After the boot require `/dev/loop0 / ext2 rw`, `booted: trialroot`, `/dev/disk0p6` on `/data`, and no fault, lockup or panic in `dmesg`.

## Run

```
L 'cat /proc/version; ls -l /dev/vhost-net /dev/kvm /dev/net/tun; qemu-system-x86_64 --version | head -1; dmesg | grep -e "^kvm" -e vhost' | tee "$R/trial-root.txt"
L 'cd /data/trial5351/b && ./vhostmusl' 2>&1 | tee "$R/vhostmusl.out"
L 'cd /data/trial5351/b && sh kvm-boot.sh -t "GUESTNET OK" -l guestnet=10.9.0.2:10.9.0.1 -k vmlinuz -i initrd -o /data/trial5351/vhost-on.log -- -netdev tap,id=n0,ifname=tap5351,script=/data/trial5351/b/ifup,downscript=no,vhost=on -device virtio-net-pci,netdev=n0; echo STATUS=$?; echo "--- qemu stderr"; cat /data/trial5351/vhost-on.log.err; echo "--- end"; dmesg | grep "vhost-net: detached: tap5351"; dmesg | tail -40' 2>&1 | tee "$R/vhost-on.out"
```

1. `trial-root.txt`: the trial kernel's build string, `/dev/vhost-net` `crw-rw---- root kvm 10, 238`, the group and mode the GNU root's udev gives it as it gives `/dev/kvm`, where the kernel alone makes it `crw-rw-rw-`, QEMU 10.0.13 and `kvm: ready: vmx`.
2. `vhostmusl.out`: `VHOSTMUSL OK`, after its 26 checks.
3. `vhost-on.out`: `kvm-boot: result: pass: GUESTNET OK` and `STATUS=0`; both of the guest's ping summaries `10 packets transmitted, 10 packets received, 0% packet loss`; between `--- qemu stderr` and `--- end` no line with `vhost`; the line `vhost-net: detached: tap5351: N frames to the guest, M from it` with N and M each 20 or more; and no fault, warning or lockup in the log's tail. A pass without the detached line, or with a count under 20, is a fail: QEMU moved the frames itself.

If step 3 fails, run it once more with `vhost=off` in place of `vhost=on` and `vhost-off` in the names: a pass there puts the fault in vhost-net, a failure in the tap or KVM. Save both and stop; the fix is this task's.

## Restore and record

As `4893-lenovo.md`'s "Restore and record", with `5351` for `4893`: empty the menu before the boot back, put the release in both slots, check them against the release record, remove `/data/trial5351` (the root image too, unless another open task names it), give up the lock and stop the netconsole receiver by its pid. Save the outputs, the netconsole log and the guest's console logs under `5351/lenovo/`, write the result in this file, close subtasks 5353 and 5356, and hand the task back for its merge.
