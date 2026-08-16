# The laptop session

Everything below needs hands on the Lenovo. It is written to be worked through in order, because each step's failure has a different next move and the order puts the cheap answers first.

Nothing here writes the SSD. The stick is the only boot device, Windows is untouched, and the metal kernel is built read-only at the disk so that this stays true even if something goes wrong.

## Before starting

On the Mac:

```sh
make full                 # the base build and the capstones
make rach2-metal          # the read-only kernel, checked as it is built
make rach2-metal-iso      # the hybrid image for the stick
make metal-test           # the same image, rehearsed unattended in QEMU
```

Then write `build/iso/rach2-metal.iso` to the stick unchanged. It is a hybrid image: the same file boots in QEMU and from a USB port.

What to have on hand:

* An Ethernet cable from the laptop to the same switch or router as the Mac.
* An address for the laptop that the router will not hand to anything else. Reserve it, or pick one outside the DHCP pool. Write it down; it goes on the kernel command line, since there is no DHCP.
* The Mac's own address on that network, and a terminal ready to run `tcpdump`.
* A camera. On the boots before SSH works, a photograph of the screen is the log.

## What to connect and what to serve

The laptop gets: power, the Ethernet cable, and the stick. Nothing else.

The Mac serves two things, and only while the session is running:

* A packet capture, which is the first proof the network card works at all:
  `sudo tcpdump -i en0 -nn -e -s0 'ether proto 0x88b5'`
  Those are the netconsole frames. Their source address should be the laptop's own `28:D2:44:7E:E0:AF`.
* An SSH client, once the machine answers.

## The boots, in order

Each boot is written down with where it came from, because the network card's entry state depends on it and that is the risk this milestone is most exposed to.

### 1. Cold power-on, the diagnostic entry

From a full power-off, boot the stick and pick the `rach2.diag` menu entry. Photograph the screen.

What it should show: the kernel's end address, the allocator's break, the direct-map end, the free frame count, every module's range, and the Ethernet controller's identity read from `TxConfig`.

* The masked revision should be `0x509`. Write down what it actually is. This is the number the driver gates on and the one thing no tool under Windows could safely read.
* If the boot stops before this, the module layout is the first suspect. `--quirk-modules-after-kernel` is the recorded fallback.

### 2. Cold power-on, the normal entry, no network expected yet

Boot the normal entry. It should reach `rach2 login:` on VGA with the 8042 keyboard working.

If it stops during bring-up, the switches exist for exactly this. Edit the command line at the GRUB prompt and add them one at a time: `rach2.nomouse`, `rach2.noaudio`, `rach2.nonet`. A skipped device says so on its boot line, so a photograph still shows what was asked for. Whichever switch lets the machine reach the login prompt names the probe that stopped it.

### 3. Cold power-on, with the address and the log on the wire

Add to the command line:

```
rach2.ip=<the reserved address>/24 rach2.gateway=<the router> rach2.netconsole
```

With `tcpdump` already running on the Mac, this is the first real test of the ported driver. What to look for, in order of how much it proves:

* Frames arriving at all, with source `28:D2:44:7E:E0:AF`. The card transmits.
* Their sequence numbers ascending with no gaps. The ring and the doorbell work.
* The text in them being the boot log, starting from before the network came up. The buffered log was carried.

Capture the whole session to a file; it is the boot log this machine has never been able to produce.

If no frames arrive, the boot line still says a great deal: the driver's own line reports the revision, the station address and the negotiated speed, and a refusal names which handshake did not complete. Photograph it. That message is the diagnostic the whole driver was written to produce.

### 4. The same, at 100 Mbps

Repeat on a 100 Mbps port. The ring is 64 descriptors where the reference uses 256, and that deviation is decided by numbers rather than argument: a 64-frame burst should complete with no drops, and the counters on the boot line say so.

### 5. SSH

If the frames arrive, the machine is probably reachable. From the Mac:

```sh
ssh -i krffs/rach2_demo_ed25519 root@<the laptop>
```

The host key is minted per boot, so use a scratch known-hosts file rather than turning checking off.

Everything after this point is a normal session, and the loop of this milestone is over: the screen and the camera stop being the interface.

### 6. After a Windows shutdown

Shut Windows down normally, then boot the stick again. This is the other entry state the machine will really present, and the one the out-of-band handshake and the AHCI reset exist for. Both the network card and the SATA link may behave differently from a cold start; record which.

### 7. Windows, with the stick removed

Boot Windows. It must come up exactly as before. This is checked at the end of every session, not once.

## What to capture when something does not work

* The screen, photographed, including the line the boot stopped on.
* The `tcpdump` capture, whatever it holds, including nothing.
* Which entry, which command line, and what the machine was doing before this boot (cold, after Windows, after a Rach2 reboot). The entry state is part of the fact.

A capture proves the displayed state and nothing more. What is wanted from this session is measured numbers: the revision, the station address, the negotiated speed and duplex, the frame and drop counters, and the memory-layout figures.

## What is deliberately not in this session

* Nothing writes the SSD. Not the partition table, not a filesystem, not one sector. The metal kernel cannot: it refuses every command but the four that read.
* No Linux is booted on this machine. Keeping it off keeps its driver's idea of the network card's state out of the experiment.
* The carve, the persistent host key and the install loop are the next milestone phase, and they are gated on the owner's approval after a full backup.
