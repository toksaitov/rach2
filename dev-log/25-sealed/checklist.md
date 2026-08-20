# The sealing session

This is the session that writes the disk. Everything before it was rehearsal; after it the drive goes into the laptop and the case is closed, and every change after that arrives over SSH.

It is written to be worked through in order. Two things make it different from the milestone 24 session, and both are the reason it is a document rather than a habit: this one writes a whole drive, and the drive it writes is not the one that session measured.

## What this session does

1. Build the image and rehearse it in QEMU.
2. Identify the target drive by model and serial, twice, and confirm it is the 250 GB drive and not the laptop's original 1 TB one.
3. Write 660602880 bytes to it over a USB adapter, from the Mac.
4. Read the same count back off the drive and compare hashes.
5. Screw the drive into the laptop, boot it, and reach it over SSH.
6. Write down what was on the disk that day.

## The passwords come first

The passwords this repository builds with are development ones, written down in
every checklist and in the script that mints them: root is `1qA2wS3eD` and the
test account is `4rF5tG6yH`. They are fine for a machine that lives in QEMU.
They are not fine for this one, and the reason is a chain rather than any one of
them: the SSH server refuses root a password, but the test account has one, and
the image stages a set-id `su`. Anyone who has read this repository could log in
as `test` and become root without ever touching the key.

So the sealing build mints its own. The generator leaves credentials that are
already there alone, because every image target calls it and a script that
reminted on each call would change a machine's passwords under whoever was
using them; replacing them is asked for by name:

```sh
RACH2_REPLACE_PASSWORDS=1 \
RACH2_ROOT_PASSWORD="$(LC_ALL=C tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 20)" \
RACH2_TEST_PASSWORD="$(LC_ALL=C tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 20)" \
    sh krffs/gen_demo_password.sh
cat krffs/rach2_demo_password krffs/rach2_test_password
```

Run that BEFORE the build below, and copy both lines into the session record;
they are the only copy, and the machine they open will not be openable any other
way. Everything built afterwards carries those hashes. Without
`RACH2_REPLACE_PASSWORDS` the script exits without doing anything when
`krffs/shadow` is already there, which is what every other build wants and is
exactly the trap this step is written to avoid.

The tree is left holding those passwords afterwards, which is right for the
build that follows and wrong for the next ordinary day's work: to go back to the
development credentials, run the same command with `RACH2_REPLACE_PASSWORDS=1`
and neither password variable set.

The keys are the other half, and they are not per-machine by default:
`gen_demo_key.sh` leaves a keypair alone once it exists, so a tree that has ever
built an image is carrying the key every demo on that machine has used. The
sealing build mints new ones the same way:

```sh
RACH2_REPLACE_KEYS=1 sh krffs/gen_demo_key.sh
```

`krffs/rach2_demo_ed25519` is then the only thing that opens a root session on
this machine, and the old one opens nothing. Keep it where the machine is not,
and take a copy: it cannot be replaced afterwards without writing the disk
again.

## The machine's own identity

The server mints a host key on its first boot and keeps it, so the ordinary
system has one identity from then on and `ssh` can pin it in the usual way. The
first connection is the one with nothing to compare against, and the machine
prints the fingerprint as it generates the key:

```
Generated hostkey /etc/dropbear/dropbear_ed25519_host_key, fingerprint is SHA256:...
```

Read that off the laptop's own screen, at the machine, and write it into the
session record. This is the one moment in the whole procedure when the screen is
worth walking to: it is the only channel here that nobody else on the network
can write to. The netconsole capture carries the same line, and it is useful for
seeing what the machine did, but it is unauthenticated broadcast: anyone on the
segment can send a frame carrying whatever fingerprint they like. Do not verify
an identity from it.

Recovery is different and it is worth knowing before the day it is needed: its
root is a RAM image, so it mints a new host key on every recovery boot and `ssh`
will say the identity changed. The screen is the same answer, if the machine can
be reached; if it cannot, then the honest position is that the connection is
unauthenticated, and what goes down it should be judged on that basis. A whole
root partition sent to a machine that turns out to be somebody else is a bad
day, but so is a machine that stays broken. The mitigation this project has is
that the image sent is not a secret and its arrival is verified by a hash read
back off the drive, so a wrong destination is detected rather than trusted.

## Before starting

On the Mac, after the passwords above:

```sh
make rach2-sealed-disk    # the artifact: 64 MiB boot, 500 MiB root, 64 MiB recovery
make sealed-test          # the same artifact, operated over SSH in QEMU
```

`make sealed-test` prints the artifact's SHA-256 on its first line. Copy that
line into the session record and keep it to hand: it names the artifact that was
rehearsed, and every check below compares against that number rather than
against whatever file happens to be in `build/images` later.

```sh
SEALED_SHA=<the digest sealed-test printed>
shasum -a 256 build/images/rach2-sealed.disk    # must equal $SEALED_SHA
```

If it does not, something was rebuilt after the rehearsal, and what is on disk
is not what was tested. Rehearse again rather than writing it.

What to have on hand:

* The 250 GB SSD and a USB-to-SATA adapter.
* The laptop's screws and whatever opens its back panel.
* An Ethernet cable to the same switch as the Mac. The sealed image takes its address from DHCP; the static entries in its menu carry 192.168.0.105/24 through 192.168.0.1 for a network that hands out nothing.
* The two passwords minted above, from the session record. They are not the development ones any more, and nothing else knows them. Root logs in over SSH by key alone; the test account's password is what `su` asks for at the console.
* `krffs/rach2_demo_ed25519`, the key the image was built around.

## Identifying the drive

This is the step that cannot be undone by rebuilding, so it is the one written
in the most detail.

Read the target drive's model and serial off its own label, before it is
plugged into anything, and write both into the session record. That is the
independent source: everything below compares against what was written down
rather than against what a tool happens to report.

The milestone 24 baseline (`dev-log/24-metal/baseline.md`) describes the
laptop's ORIGINAL drive: a Samsung SSD 870 EVO 1TB, serial S74ZNX0Y927920A,
1953525168 sectors. That is not the drive being sealed, and writing this image
to it would destroy the Windows installation this project has kept intact
through two milestones.

With the target drive on the adapter and nothing else new plugged in:

```sh
diskutil list
diskutil info /dev/diskN     # model, serial, size, whether it is removable
```

Then check every one of these, and stop at the first that fails:

* The model and the serial `diskutil info` reports are EXACTLY the ones written
  down from the label. Not similar, and not blank: an adapter that reports no
  serial is an adapter to replace before writing anything, because it cannot
  tell you which drive is behind it.
* The size is around 250 GB, not 1 TB.
* The serial is NOT S74ZNX0Y927920A.
* `diskutil info` reports the disk as removable or external. The Mac's own
  internal drive never is.

Then unplug the adapter, run `diskutil list` again, and confirm the node has
gone. Plug it back in and confirm it comes back, with the same model and serial.
That is what says which node is the drive in your hand rather than which node
was there a minute ago.

## The write and the readback

One block, and the device is named once. Everything the earlier steps checked by
eye is checked again here by the shell, immediately before the write and in the
same breath as it, because the two numbers that matter are the two that can have
changed since: which node the drive is on, and which artifact is in the build
directory. `set -e` means the first line that disagrees stops the block before
anything is written.

Fill in the four values from the session record, and paste the whole thing. Every
value it reads off the machine is required to be there and to match exactly: a
field this could not parse comes back empty and stops the block, which is the
behaviour to want from something that is about to overwrite a disk.

```sh
(
    set -e
    DISK=/dev/disk4                       # the node the checks above settled on
    MODEL='Samsung SSD 870 EVO 250GB'     # exactly as the label reads
    SERIAL='S6PENX0T123456X'              # exactly as the label reads
    SEALED_SHA=0000000000000000000000000000000000000000000000000000000000000000

    RAW="/dev/r${DISK#/dev/}"             # the same device, derived and not typed
    INFO="$(diskutil info "$DISK")"

    GOT_MODEL="$(printf '%s\n' "$INFO" | awk -F': *' '/Device \/ Media Name/ {print $2; exit}')"
    GOT_SERIAL="$(printf '%s\n' "$INFO" | awk -F': *' '/Serial Number/ {print $2; exit}')"
    GOT_BYTES="$(printf '%s\n' "$INFO" | awk -F'[()]' '/Disk Size/ {print $2; exit}' | awk '{print $1}')"
    GOT_WHERE="$(printf '%s\n' "$INFO" | awk -F': *' '/Device Location/ {print $2; exit}')"

    test -n "$MODEL" && test -n "$SERIAL" && test -n "$SEALED_SHA"
    test -n "$GOT_MODEL" && test -n "$GOT_SERIAL" && test -n "$GOT_BYTES"
    test "$GOT_MODEL" = "$MODEL"
    test "$GOT_SERIAL" = "$SERIAL"
    test "$GOT_WHERE" = "External"
    test "$GOT_BYTES" -ge 660602880
    test "$GOT_BYTES" -lt 600000000000
    shasum -a 256 build/images/rach2-sealed.disk | grep -q "^$SEALED_SHA "

    diskutil unmountDisk "$DISK"
    sudo dd if=build/images/rach2-sealed.disk of="$RAW" bs=1m count=630
    sync

    sudo dd if="$RAW" bs=1m count=630 2>/dev/null | shasum -a 256 | grep -q "^$SEALED_SHA "
    echo "the drive holds the artifact that was rehearsed"
)
```

The values in the block are examples and every one of them is wrong for your
drive; fill them in from the session record. An empty or unchanged one stops the
block at the first `test`.

Run it once with the four `sudo` and `diskutil` lines commented out if you want
to see what it parsed. A field name this awk does not match is an empty value
and a stop, not a wrong write.

630 MiB is 660602880 bytes, which is the whole artifact; `dd` is given the count
so a truncated file cannot be written as if it were complete. The raw node is
derived from `$DISK` rather than typed again, because a raw node typed by hand
is a second chance to name the wrong disk, and it is the one that writes at a
usable speed.

The last two lines are the only check that the bytes on the drive are the bytes
that were tested. A `dd` that reported success has said nothing about what is on
the platter, and comparing against the file rather than against the recorded
digest would pass an artifact that was rebuilt after the rehearsal.

If the readback disagrees, nothing is lost: run the block again. The image is
built from the repository, not from the drive.

## Into the laptop

Windows lives on the original drive. Take that drive out and keep it, whole, as the way back to the machine as it was; put the sealed drive in its place.

Then, with the Ethernet cable in and nothing else attached, power it on. The default menu entry boots in three seconds with nothing typed.

What should happen, in order:

* The GRUB menu draws on the laptop's screen, with seven entries, the first highlighted.
* The machine takes an address from the router's DHCP server.
* Within about half a minute, it answers SSH.

From the Mac:

```sh
ssh -i krffs/rach2_demo_ed25519 root@<the address the router gave it>
bootctl status
```

`bootctl status` should say `booted: candidate`, report the four boot files' extents, and show no trial armed. That is the machine in the state the artifact was built in.

If it does not answer, the screen is the log again: photograph it. The netconsole is on (`rach2.netconsole`), so a capture on the Mac (`sudo tcpdump -i en0 -nn -e -s0 'ether proto 0x88b5'`) carries the boot log even when the machine cannot be reached.

If the network has no DHCP server, pick `Rach2 (static address)` at the menu; it carries 192.168.0.105/24.

## What to record

* The root and test passwords minted for this machine, kept where the machine
  is not.
* The artifact's SHA-256, from `make sealed-test`.
* The drive's model and serial as read off its label, beside what `diskutil`
  reported.
* The drive's model, serial and size, as `diskutil info` reported them.
* The readback hash, and that it matched.
* The address the router gave the machine, and the output of `bootctl status` from the first SSH session.
* Anything the machine printed that this document did not predict.

## What this session does not do

* It does not touch the original 1 TB drive. That drive comes out before the sealed one goes in, and it is the way back.
* It does not partition or format anything on the Mac. The image is written whole and carries its own table.
* It does not install a bootloader on the machine: the artifact already carries GRUB in the gap, its menu on partition 1, and recovery on partition 3.
* It does not need the machine again afterwards. Everything after this arrives over SSH, and what to do when that stops working is `tools/Readme.md`'s recovery section, not this file.
