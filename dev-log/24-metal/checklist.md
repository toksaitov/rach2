# Laptop hardware session

Every step requires physical access to the Lenovo. Perform them in order because each failure has a different follow-up and the early steps provide the simplest results.

No step writes the SSD. The stick is the only boot device, Windows remains unchanged, and the metal kernel enforces read-only disk access even after an error.

## Before starting

Run these commands on the Mac:

```sh
make full                 # the base build and the capstones
make rach2-metal          # the read-only kernel, checked as it is built
make rach2-metal-iso      # the hybrid image for the stick
make metal-test           # the same image, rehearsed unattended in QEMU
```

Write `build/iso/rach2-metal.iso` to the stick without modification. The same hybrid image boots in QEMU and from USB.

Prepare the following:

* Connect the notebook by Ethernet cable to the same switch or router used by the Mac.
* The first three metal-menu entries already include `rach2.ip=192.168.0.105/24 rach2.gateway=192.168.0.1 rach2.netconsole`, so they require no GRUB edits. For another network, rebuild with `RACH2_METAL_IP=... RACH2_METAL_GATEWAY=... make rach2-metal-iso`, or select the entry without network configuration and edit it at the prompt.
* The short, fixed development passwords are `1qA2wS3eD` for root and `4rF5tG6yH` for the test account. `krffs/gen_demo_password.sh` generates them, and `RACH2_ROOT_PASSWORD` overrides the root value.
* Reserve a notebook address that the router will not assign elsewhere, or choose one outside the DHCP pool. Record it for the kernel command line because Rach2 has no DHCP client.
* Record the Mac's address on the same network and prepare a terminal for `tcpdump`.
* Keep a camera ready. Before SSH works, a screen photograph provides the boot log.

## Connections and host services

Connect only power, Ethernet, and the stick to the notebook.

The Mac provides two services only during the session:

* Start a packet capture to obtain the first evidence that the network card transmits:
  `sudo tcpdump -i en0 -nn -e -s0 'ether proto 0x88b5'`
  These are netconsole frames. Their source address should be the notebook's `28:D2:44:7E:E0:AF`.
* Use an SSH client after the notebook responds.

## Ordered boots

Record the preceding machine state for every boot. The network card's initial state depends on whether the boot follows cold power, Windows, or Rach2.

### 1. Cold power-on with the diagnostic entry

After a complete power-off, boot the stick and select `rach2.diag`.

The boot log scrolls past in well under a second and cannot be photographed; the first session required video. The diagnostic entry therefore prints the report a second time at the end of boot and waits for a key. Photograph that report before pressing a key. Do not try to capture the earlier scrolling copy.

The report should include the kernel end address, allocator break, direct-map end, free-frame count, every module range, and the Ethernet controller identity read from `TxConfig`.

* Record the actual masked revision; the expected value is `0x509`. The driver checks this value, and no Windows tool could read it safely.
* If the boot stops earlier, investigate module placement first. The recorded fallback is `--quirk-modules-after-kernel`.

### 2. Cold power-on with the normal entry

Boot the normal entry without expecting network access. It should reach `rach2 login:` on VGA with a working 8042 keyboard.

If initialization stops, edit the GRUB command line and try `rach2.nomouse`, `rach2.noaudio`, and `rach2.nonet` one at a time. The boot log identifies each skipped device, so a photograph records the requested configuration. The option that permits login identifies the device probe that stopped.

### 3. Cold power-on with addressing and netconsole

Add this command line:

```
rach2.ip=<the reserved address>/24 rach2.gateway=<the router> rach2.netconsole
```

Start `tcpdump` on the Mac before booting. This is the first hardware test of the ported driver. Check these results in increasing order of evidence:

* Frames arrive from `28:D2:44:7E:E0:AF`, proving that the card transmits.
* Sequence numbers increase without gaps, proving that the ring and doorbell operate.
* Frame contents contain the boot log beginning before network initialization, proving that buffering retained the earlier output.

Save the complete capture. It provides the boot log previously unavailable from this notebook.

If no frames arrive, photograph the driver's boot line. It reports the revision, station address, negotiated speed, and any failed handshake. The driver was written to provide this diagnostic without networking.

### 4. Repeat at 100 Mbps

Repeat the test on a 100 Mbps port. This ring has 64 descriptors while the reference uses 256. A 64-frame burst must complete without drops, as reported by the boot-line counters.

### 5. SSH and a disk read against the baseline

The first session established two usage requirements. This project's `ping` accepts a host without flags. Use `ping 192.168.0.1`; `ping -c 1 192.168.0.1` instead tries to resolve `-c` as a name. Host names require `/etc/resolv.conf`, which the metal image stages with 1.1.1.1. Numeric addresses do not use that file.

If frames arrive, try SSH from the Mac:

```sh
ssh -i krffs/rach2_demo_ed25519 root@<the laptop>
```

The host key changes on every boot. Use a temporary known-hosts file instead of disabling host-key checking.

After SSH login, the remaining work uses a normal session. The screen and camera are no longer the interface.

Add `rach2.disk=ahci rach2.diskdiag` to the command line, then read the disk without writing it:

```sh
cat /proc/disk
dd if=/dev/disk0 bs=512 count=1 2>/dev/null | sha256sum
```

Compare every line with `baseline.md`, which records the Windows measurements taken before Rach2 ran on this notebook: model, serial, 1953525168 sectors, disk signature `0x4905b015`, two partition entries, and the MBR SHA-256. Any difference requires investigation, regardless of which side is wrong. This step performs no writes. If it prints `refused command`, the diagnostic left the read path; report that defect instead of bypassing it.

### 6. Boot after a Windows shutdown

Shut Windows down normally, then boot the stick again. This provides the other expected initial state and exercises the out-of-band handshake and AHCI reset. Record whether the network card or SATA link differs from a cold start.

### 7. Boot Windows without the stick

Remove the stick and boot Windows. It must behave exactly as before. Repeat this check at the end of every session.

### 8. Run scripted acceptance

After SSH responds, run the scripted checks from the development host:

```sh
sh tools/metal_accept.sh <the laptop>
```

The script reports the machine identity, requires the read-only kernel and no mounted disk, and compares the drive identity and first-sector hash with `baseline.md`. Retain its output as the session record. This checklist retains the manual tasks that the script cannot perform: disconnecting a cable, power-cycling the notebook, hearing a sound, and watching Windows boot.

## Failure evidence

* Photograph the screen, including the line where boot stopped.
* Save the `tcpdump` capture even when it contains no frames.
* Record the menu entry, complete command line, and preceding state: cold, after Windows, or after a Rach2 reboot. That initial state is part of the result.

A capture proves only the recorded state. Collect the measured revision, station address, negotiated speed and duplex, frame and drop counters, and memory-layout values.

## Excluded work

* Do not write the SSD: not the partition table, a file system, or one sector. The metal kernel permits only the four read commands.
* Do not boot Linux on the notebook. Its driver would change the network card's initial state and invalidate the experiment.
* Partitioning, a persistent host key, and the installation loop belong to the next milestone phase. They require the owner's approval after a complete backup.
