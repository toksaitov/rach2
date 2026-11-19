# Network gateway guests

The netguest suite gives Rach2's wired card to an unmodified gateway guest through VFIO and VT-d, reaches the network through that guest over a tap link, and takes the card back. The guests are FreeBSD 15.1 with pf and Debian 13 with nftables, each prepared once from its project's own cloud image; the tap link runs with QEMU moving its frames and with `/dev/vhost-net` moving them in the kernel. Every case is judged by what the kernel and the guest count: bytes on the tap, frames vhost-net moved, the guest's count of denied packets, and the VFIO, KVM, IOMMU and interrupt-vector counts before and after. The suite has a QEMU form, which etude runs on a Linux host, and a native form for the Lenovo, run from the Mac.

## Running

`ARCH=x86_64 tests/run netguest` runs the six cases of `tests/cases/machine.netguest.json`; `RACH2_CASES=machine.netguest.debian` selects one. Each case boots the GNU root of the assignment suite ([assign](../assign/Readme.md)) under KVM on q35 with QEMU's VT-d unit, the patched QEMU that suite builds, four processors and 6 GiB, and gives away an e1000 at `0000:01:00.0`, behind the PCIe root port at `0000:00:06.0`, on the user network `10.0.3.0/24`; Rach2 keeps a second e1000 on `10.0.2.0/24`. The guest runs under Rach2's KVM, a third level of virtualization on the testing host, so its times there mean nothing and none is reported. The case's peer is `nettput -s` on the host's 127.0.0.1 at the case's port, which the machine and its guest reach at `10.0.3.2`.

On the Lenovo, from the Mac, with the machine taken and booted as the [machine skill](../../.agents/skills/machine/SKILL.md) says, a kernel with the tap device, vhost-net and VFIO on the GNU root:

```sh
make ARCH=x86_64 build/tests/cases/netguest/nettput build/tests/cases/netguest/nettput-host
sh tests/netguest/lenovo.sh -s STAGE stage
sh tests/netguest/lenovo.sh run
sh tests/netguest/lenovo.sh clean
```

STAGE holds `nettput` and the two gateway images; the images are made on Linux (Building, below), so STAGE is a copy of the testing host's `build/tests/cases/netguest`. `run` takes the machine's address and wired interface from `tools/flow/machines` (`lenovo network`, `lenovo interface`) and no address, station address or serial of the machine is written in this tree; `RACH2_MACHINE_MAC`, when set, is checked against the interface before anything runs. Start it from a shell with macOS's Local Network permission, the release's `tmux -L rach2-lan`, since the Mac's nettput is charged to the program that started it. The script's opening comment has every option.

## Results

`run.sh` prints netguest.sh's lines, among them `netguest: counts: STEP NAME=VALUE...` at the start, once the card is given, once the guest is ready, after the transfers and once the card is back, then `netguest: case: CASE: pass` or `netguest: case: CASE: fail: REASON` for each case and last `netguest: result: pass: N cases` or `netguest: result: fail: F of N cases failed`. Each case's directory under the run's scratch keeps the guest's console and QEMU's messages. On the Lenovo the run's directory is fetched into `build/runs/netguest-lenovo-TIME/run`, with the whole output in its `log`.

## Interfaces

* `run.sh [-D] [-B] [-W] [-c BDF | -n IFACE] [-a ADDR/PREFIX] [-d SECONDS] [-o OUT] [-A kvm|tcg] PEER PORT CASE...` runs the cases as root on Rach2. The cases are `cycle`, `debian`, `freebsd`, `debian-vhost`, `freebsd-vhost` and `kill`, as Tests below says. `-D` detaches the run with its output in `OUT/log` and its exit status in `OUT/status`, `-W` takes every other interface's IPv4 address off for the run, `-B` reboots a machine whose network does not answer once the card is back, and `-d` gives netguest.sh its deadline for each boot.
* `netguest.sh -c BDF -i IMAGE [-V] [-F 0|1] [-p PEER] [-P PORT] [-k|-q] [-d SECONDS] [-- COMMAND...]` does one boot: the interface's address off, the card from its driver to vfio-pci, QEMU with the card through VFIO and a `virtio-net-pci` on `tap0`, with `vhost=on` under `-V`, the guest's ready line, PEER's first answer through it, COMMAND, the guest's end by its power button, by SIGTERM under `-q` or by SIGKILL under `-k`, and the card back with its address. It fails a boot after which a VFIO or KVM count or the free vectors differ from the start, or an IOMMU fault was counted.
* `traffic.sh transfers|deny|kill` is the COMMAND of the cases' boots.
* `nettput` is one TCP tool for Rach2, Linux and macOS whose tests end at a count: `send` and `recv` of a number of bytes, `rr` exchanges and `crr` connections, and `wait`, the attempts the path takes to answer.
* The guest takes its parameters from the first sector of its second virtio disk, `rules=N opt=none|basic state=1|0 fwd=PORT filter=1|0`, and writes `NETGUEST READY ...` or `NETGUEST FAILED reason` on its serial console, then `NETGUEST STATS outer_in=N outer_out=N deny9=N states=N` every two seconds, as each system's `netguest.sh` says. Rach2 has `10.77.0.2/24` on the tap and the guest `10.77.0.1`; the guest asks DHCP for the card's address, translates `10.77.0.0/24` out of it and forwards TCP port 22 of it to Rach2's.

The counts are the kernel's `/proc/stat` records ([vfio](../../rach2/sys/vfio/Readme.md#interfaces), [KVM](../../rach2/sys/kvm/Readme.md)), tap0's line of `/proc/net/dev`, and vhost-net's record `vhost-net: detached: tap0: N frames to the guest, M from it` ([vhost-net(4)](../../man/man4/vhost-net.4)).

## Sources and license

The gateway images are FreeBSD 15.1-RELEASE's BASIC-CLOUDINIT VM image, under FreeBSD's BSD licenses, and the Debian 13 generic cloud image with nftables from Debian's archive, each package under its own free license; neither is changed beyond the boot script and settings that its `setup.sh` writes. They are fetched by the digests their pins give and are never committed. Everything in this directory is Rach2's.

## Design

The card is the machine's only link on the Lenovo, so the way back is in every exit path: netguest.sh's trap gives the card back to its driver and puts the interface's address and default route back, each step read back and tried three times, at the end, after a failure, at a signal and at its deadline; run.sh's trap binds the driver again whatever stopped the run. FreeBSD's `em(4)` needs the 82540's I/O BAR, which Rach2's VFIO shows. The virtio card offers no mergeable receive buffers, since with them FreeBSD 15.1's `vtnet` takes a 4 KiB cluster for every receive buffer and ran out of them on the Lenovo. The inner link has no checksum or segmentation offload for either guest.

## Building

`make ARCH=x86_64 build/tests/cases/netguest/netguest.ext2` builds the payload: `run.sh`, `netguest.sh`, `traffic.sh`, the static `nettput` and the two images with their records, on an ext2 disk the cases mount read-only. `prepare.sh freebsd|debian OUT` makes an image on a Linux host with QEMU, qemu-img, xorriso, curl and xz, under KVM where `/dev/kvm` opens. FreeBSD's is the BASIC-CLOUDINIT image that `freebsd/image` pins by URL and SHA-256; Debian's is the cloud image of `tests/graphics/debian-ref/image`, with nftables from the snapshot that file names. One boot with a cidata seed runs the system's `setup.sh`, which installs the boot script, `netguest.sh` of the system's directory, for every later boot and powers the guest off; the result is converted to a compressed qcow2 with no backing file and kept in `build/cache/netguest` with the digest of its inputs, so that it is remade only when the recipe or the pins change.

## Tests

* `cycle`: iproute2 makes, addresses and removes `tap9`; the driver lets the card go and takes it back three times, the peer answering after each; the free interrupt vectors are as before.
* `debian`, `freebsd`: the guest with its filter; nettput sends and receives 8388608 bytes and makes 1000 exchanges and 200 connections through it, which tap0's byte counts must show each way; ten attempts at the peer's port 9 must raise the guest's deny9 count above 0.
* `debian-vhost`, `freebsd-vhost`: the same transfers through the guest with translation alone and the tap on vhost-net, which must have moved at least 5540 frames each way, the fewest frames of at most 1514 bytes that carry 8388608 bytes, with no line on vhost on QEMU's stderr, which QEMU writes when it falls back to moving the frames itself; the guest must deny nothing.
* `kill`: QEMU is killed under the Debian guest once tap0 has carried a mebibyte of a send through it.

Every boot also requires the card back with its driver, the peer answering on it, and every VFIO, KVM, IOMMU and vector count as at its start. No elapsed time decides a result.
