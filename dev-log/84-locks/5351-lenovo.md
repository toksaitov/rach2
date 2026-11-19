# 84-5351: the prepared Lenovo trial of vhost-net

The task is ready for the manager's go, not booted. One trial boot of the task's x86-64 kernel on the 84-58 trial root, whose QEMU 10.0.13 runs the KVM suite's Debian guest with `-netdev tap,vhost=on`. The step is silent and takes no screen; it needs no notice.

## What it proves

* `/dev/vhost-net` exists as 10:238 and `vhostmusl` drives it on the Lenovo as on QEMU: `VHOSTMUSL OK`, 26 checks.
* QEMU's `vhost=on`, unchanged, runs a Linux guest whose virtio-net frames go through the kernel: the guest loads `virtio_net`, takes 10.9.0.2/24 and pings the tap's 10.9.0.1 ten times with 56-byte and ten times with 1400-byte payloads, then prints `GUESTNET OK`. QEMU refuses to start when `vhost=on` cannot set the device up, so a pass is the kernel's path, kick through KVM's ioeventfd and call through its irqfd, or QEMU's own relay of them where KVM lacks a route.

## Readiness for the go

vhost-net touches no device register, DMA, interrupt or mapping that gives a program a device. It is software: a character device, one kernel thread at ordinary priority, and the receive path the tap already calls under `tap_lock` with interrupts off. Every loop in it is bounded: a descriptor chain by the ring's size, a transmit pass by 128 frames, the drain of the tap's queue by its 1024 frames. The device's lock, `vnet`, nests inside `tap_lock` and outside eventfd's, as the lock-order table of `rach2/sys/proc/Readme.md` gives in row 11; nothing under it sleeps, and the pins it holds are taken and dropped outside it. A guest's bad ring stops that ring and signals its error eventfd; it cannot reach the host's memory beyond the table's pinned regions. Booting the kernel only starts the thread and registers the node; nothing runs until a program opens the device.

The host test `tests/host/vhost_net_hosttest.c` runs the requests, the memory table, receive, transmit and teardown under ASan and UBSan, 0 failed. QEMU on WSL ran `vhostmusl` to `VHOSTMUSL OK`, 26 of 26, on the i686 kernel, the x86-64 kernel, and the i686 image under the x86-64 kernel, which passes vhost's requests from an i386 program untranslated. The Debian guest's `guestnet=` check ran to `GUESTNET OK` under QEMU's own user network on WSL, 20 of 20 pings answered.

The task's diff has had no Codex review: the team account that wrote it cannot run Codex. A trial waits for no review; the review is owed before the task merges.

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
L 'cd /data/trial5351/b && sh kvm-boot.sh -t "GUESTNET OK" -l guestnet=10.9.0.2:10.9.0.1 -k vmlinuz -i initrd -o /data/trial5351/vhost-on.log -- -netdev tap,id=n0,ifname=tap5351,script=/data/trial5351/b/ifup,downscript=no,vhost=on -device virtio-net-pci,netdev=n0; echo STATUS=$?; cat /proc/stat | grep -e kvm -e eventfd; dmesg | tail -40' 2>&1 | tee "$R/vhost-on.out"
```

1. `trial-root.txt`: the trial kernel's build string, `/dev/vhost-net` `crw------- 10, 238`, QEMU 10.0.13 and `kvm: ready: vmx`.
2. `vhostmusl.out`: `VHOSTMUSL OK`, after its 26 checks.
3. `vhost-on.out`: `kvm-boot: result: pass: GUESTNET OK` and `STATUS=0`, both of the guest's ping summaries `10 packets transmitted, 10 packets received, 0% packet loss`, and no fault, warning or lockup in the log's tail.

If step 3 fails, run it once more with `vhost=off` in place of `vhost=on` and `vhost-off` in the names: a pass there puts the fault in vhost-net, a failure in the tap or KVM. Save both and stop; the fix is this task's.

## Restore and record

As `4893-lenovo.md`'s "Restore and record", with `5351` for `4893`: empty the menu before the boot back, put the release in both slots, check them against the release record, remove `/data/trial5351` (the root image too, unless another open task names it), give up the lock and stop the netconsole receiver by its pid. Save the outputs, the netconsole log and the guest's console logs under `5351/lenovo/`, write the result in this file, close subtasks 5353 and 5356, and hand the task back for its Codex review and merge.
