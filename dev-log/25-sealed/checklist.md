# The sealing session

This session writes the disk. All earlier work was rehearsal. Afterward, the drive goes into the laptop, the case is closed, and every subsequent change arrives over SSH.

Follow the procedure in order. It differs from the milestone 24 session in two ways: it writes a complete drive, and it does not write the drive measured in that session.

## What this session does

1. Build the image and rehearse it in QEMU.
2. List the candidate drive and all properties reported by the host, obtain the owner's approval for that node, and confirm it by unplugging and reconnecting the drive.
3. Write 660602880 bytes from the Mac through a USB adapter.
4. Read the same byte count from the drive and compare hashes.
5. Install the drive in the laptop, boot it, and connect over SSH.
6. Record the disk contents from that day.

## The passwords come first

The repository's default passwords are for development and appear in every checklist and in their generating script: root uses `1qA2wS3eD`, and the test account uses `4rF5tG6yH`. They are suitable for QEMU but not for this machine. The SSH server rejects root password login, but the test account has a password and the image stages a set-id `su`. Anyone who has read the repository could log in as `test` and become root without the key.

The sealing build therefore creates separate passwords. The generator preserves existing credentials because every image target invokes it; replacing them on every call would unexpectedly change the machine's passwords. Replacement requires an explicit option:

```sh
RACH2_REPLACE_PASSWORDS=1 \
RACH2_ROOT_PASSWORD="$(LC_ALL=C tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 20)" \
RACH2_TEST_PASSWORD="$(LC_ALL=C tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 20)" \
    sh krffs/gen_demo_password.sh
cat krffs/rach2_demo_password krffs/rach2_test_password
```

Run that BEFORE the build below and copy both lines into the session record. They are the only copy, and no other passwords will open the machine. Every later build carries those hashes. Without `RACH2_REPLACE_PASSWORDS`, the script exits without changing an existing `krffs/shadow`, as required by ordinary builds. The explicit option prevents this sealing build from retaining the development credentials.

The tree retains the new passwords for the following build. Before returning to ordinary development, run the same command with `RACH2_REPLACE_PASSWORDS=1` and neither password variable set.

The login keys also require replacement. `gen_demo_key.sh` preserves an existing keypair, so a tree that has built an image retains the key used by every demo on that machine. The sealing build creates a new pair explicitly:

```sh
RACH2_REPLACE_KEYS=1 sh krffs/gen_demo_key.sh
```

`krffs/rach2_demo_ed25519` then becomes the only key that opens a root session on this machine; the previous key no longer works. Store it away from the machine and keep a copy. Replacing it later requires another disk write.

## The machine's own identity

The server creates and retains a host key on its first boot. The ordinary system then has a stable identity that `ssh` can pin. No previous identity is available for the first connection, so the machine prints the fingerprint when it generates the key:

```
Generated hostkey /etc/dropbear/dropbear_ed25519_host_key, fingerprint is SHA256:...
```

Read the fingerprint from the laptop's screen and write it in the session record. This is the only step that requires the screen because no other machine on the network can write to it. The netconsole capture contains the same line and records the boot, but it is unauthenticated broadcast. Any station on the segment can transmit an arbitrary fingerprint. Do not use it to verify the server's identity.

Recovery differs because its root is a RAM image. It creates a new host key on every recovery boot, and `ssh` reports the identity change. Verify the new fingerprint on the screen when the machine is accessible. Without the screen, the connection is unauthenticated and must be treated accordingly. Sending a complete root partition to the wrong machine is destructive, but leaving the target unrepaired may also be unacceptable. The transmitted image is not secret, and a hash of the bytes read back from the drive verifies its arrival. This detects a wrong destination instead of assuming the connection was authentic.

## Before starting

After replacing the passwords, run these commands on the Mac:

```sh
make rach2-sealed-disk    # the artifact: 64 MiB boot, 500 MiB root, 64 MiB recovery
make sealed-test          # the same artifact, operated over SSH in QEMU
```

`make sealed-test` prints the artifact's SHA-256 on its first line. Copy that line into the session record. It identifies the rehearsed artifact, and every later check uses that value rather than the file that happens to occupy `build/images`.

```sh
SEALED_SHA=<the digest sealed-test printed>
shasum -a 256 build/images/rach2-sealed.disk    # must equal $SEALED_SHA
```

If the values differ, the artifact was rebuilt after rehearsal and is not the tested image. Rehearse the new artifact before writing it.

What to have on hand:

* The 250 GB SSD and a USB-to-SATA adapter.
* The laptop's screws and a tool for opening its back panel.
* An Ethernet cable connected to the same switch as the Mac. The sealed image obtains its address through DHCP; the static menu entries use 192.168.0.105/24 through 192.168.0.1 when no server responds.
* The two passwords created above and copied into the session record. They are no longer the development passwords, and no other copy exists. Root uses SSH key authentication alone; the test account uses its password for `su` at the console.
* `krffs/rach2_demo_ed25519`, the key included in the image.

## Identifying the drive

This step cannot be undone by rebuilding, so it requires the most detailed checks.

Rach2 is not bound to a particular drive. The kernel probes the available disk and scans its partition entries for the KRFFS magic, so any drive with the layout can boot the image. Drive identity protects only this host's `dd` command. The required check is therefore a person confirming the physical drive, not a label embedded in the image.

The check requires approval under the irreversible-hardware policy in `CLAUDE.md`. Before writing, the operator or assistant lists the candidate and all properties reported by the host, presents them to the owner, and waits for approval that specifies the node. Approval applies only to that run on that host because a node number identifies the host's current attachment, not the drive permanently.

Connect the target drive through the adapter with no other new device attached:

```sh
diskutil list physical       # every disk, so the candidate is seen among them
diskutil info /dev/diskN     # size, bus, internal or external, whole disk
```

Present the following properties and confirm them before requesting approval:

* The size is around 250 GB, not 1 TB. The laptop's original drive is a Samsung SSD 870 EVO 1TB, serial S74ZNX0Y927920A, with 1953525168 sectors (`dev-log/24-metal/baseline.md`). Writing this image there would destroy the Windows installation preserved through two milestones.
* `Device Location` is `External`. The Mac's internal drive does not have this value.
* `Whole` is `Yes`, confirming that the node represents a complete disk rather than a partition.
* The drive's current partition table, reported without interpretation, so the owner can compare it with the expected device.
* The model and serial when the bridge reports them. The adapter used here reports model `Generic` and no serial because it describes itself rather than the drive. This adapter limitation does not prevent the operation, but it makes explicit approval more important because no serial comparison is available.

Disconnect the adapter, run `diskutil list` again, and confirm that the node disappears. Reconnect it and confirm that the node returns. This identifies the attached drive independently of any property reported by the bridge.

## The write and the readback

The following block uses one device value. It rechecks the properties that could have changed after approval and verifies the artifact in the build directory. It cannot recheck drive identity because the adapter supplies no serial; the approval above establishes that identity. `set -e` stops the block at the first failed check, before the write.

Fill in the two values and paste the complete block. `DISK` is the approved node, and `SEALED_SHA` is the digest printed by `make sealed-test`. Both are empty here so an unedited copy stops at its first check instead of carrying a real device number into another session.

The mechanical checks detect a node that changed between approval and writing. They do not compare a serial because the bridge may omit it. They require an external whole disk of a plausible size and reject the host's boot disk.

```sh
(
    set -e
    set -o pipefail                       # a failing dd in a pipe must not pass
    IMG=build/images/rach2-sealed.disk
    BYTES=660602880

    DISK=                                 # the node the approval named
    SEALED_SHA=                           # the digest sealed-test printed
    APPROVED_BYTES=                       # the size the approval was given for

    # One test per line. `test a && test b` is an AND list, and a failing FIRST
    # command in one of those is exempt from `set -e`: the shell skips the rest
    # and carries on. Written that way, an empty DISK would not stop here.
    test -n "$DISK"
    test -n "$SEALED_SHA"
    test -n "$APPROVED_BYTES"

    # The digest is compared as a string, never interpolated into a pattern: a
    # nonempty but wrong-shaped value such as `.*` matches every line grep is
    # given, and both hash checks would then pass on any drive.
    printf '%s' "$SEALED_SHA" | grep -Eq '^[0-9a-f]{64}$'

    # The node is a whole-disk device node and not the host's boot disk, which
    # is resolved rather than assumed to be disk0: an externally booted Mac has
    # a different number, and `DISK=disk0` without the prefix would slip a
    # literal comparison while `RAW` still became /dev/rdisk0.
    printf '%s' "$DISK" | grep -Eq '^/dev/disk[0-9]+$'

    # Two hops, because `/` resolves to the APFS container and not to the disk
    # under it: on this Mac the container is disk3 and the drive is disk0, so a
    # check that stopped at the first answer would leave the physical boot disk
    # unguarded. The External test below is the real protection; this is the
    # belt to its braces.
    BOOTC="$(diskutil info / | awk -F': *' '/Part of Whole/ {print $2; exit}')"
    test -n "$BOOTC"
    test "$DISK" != "/dev/$BOOTC"
    BOOTS="$(diskutil info "$BOOTC" | awk -F': *' '/APFS Physical Store/ {print $2; exit}')"
    if [ -n "$BOOTS" ]; then
        BOOTW="$(diskutil info "$BOOTS" | awk -F': *' '/Part of Whole/ {print $2; exit}')"
        test -n "$BOOTW"
        test "$DISK" != "/dev/$BOOTW"
    fi

    RAW="/dev/r${DISK#/dev/}"             # the same device, derived and not typed
    INFO="$(diskutil info "$DISK")"

    GOT_BYTES="$(printf '%s\n' "$INFO" | awk -F'[()]' '/Disk Size/ {print $2; exit}' | awk '{print $1}')"
    GOT_WHERE="$(printf '%s\n' "$INFO" | awk -F': *' '/Device Location/ {print $2; exit}')"
    GOT_PROTO="$(printf '%s\n' "$INFO" | awk -F': *' '/Protocol/ {print $2; exit}')"
    GOT_WHOLE="$(printf '%s\n' "$INFO" | awk -F': *' '/^ *Whole/ {print $2; exit}')"

    test -n "$GOT_BYTES"
    test -n "$GOT_WHERE"
    test -n "$GOT_PROTO"
    test "$GOT_WHERE" = "External"
    test "$GOT_PROTO" = "USB"
    test "$GOT_WHOLE" = "Yes"
    # The exact size the approval named, not a range: a range accepts any other
    # external disk that happens to be attached.
    test "$GOT_BYTES" -eq "$APPROVED_BYTES"

    # The artifact is whole before it is written. `count` is a maximum, so dd
    # succeeds on an early end of file and would write a truncated image as if
    # it were complete.
    test "$(stat -f %z "$IMG")" -eq "$BYTES"
    ACTUAL_SHA="$(shasum -a 256 "$IMG" | awk '{print $1}')"
    test "$ACTUAL_SHA" = "$SEALED_SHA"

    diskutil unmountDisk "$DISK"
    sudo dd if="$IMG" of="$RAW" bs=1m count=630
    sync

    READBACK_SHA="$(sudo dd if="$RAW" bs=1m count=630 | shasum -a 256 | awk '{print $1}')"
    echo "readback: $READBACK_SHA"        # for the session record
    test "$READBACK_SHA" = "$SEALED_SHA"
    echo "the drive holds the artifact that was rehearsed"
)
```

Every value begins empty and has a separate test, so the unedited block stops before reading a device. Without a reported serial, the shell can verify only that the node is an external USB whole disk of exactly the approved size. Approval and the disconnect-and-reconnect check establish the drive's identity.

To inspect the parsed values, comment out `diskutil unmountDisk`, both `sudo dd` lines, and `sync`, then add a `printf` for the four `GOT_` values after the reads. Do not comment out the `diskutil info` line because the parser needs its output. An unmatched field produces an empty value and stops the block before a write.

630 MiB is 660602880 bytes, the complete artifact. Although `dd` receives this count, the count is a maximum and `dd` succeeds after an early end of file. The block therefore checks the file size before writing. It derives the raw node from `$DISK` instead of accepting a second device name, which avoids another opportunity to select the wrong disk while retaining raw-device speed.

The final two lines verify that the drive contains the tested bytes. Successful `dd` completion does not establish the stored contents. Comparing with the recorded digest, rather than the current file, also detects an artifact rebuilt after rehearsal.

If the readback differs, run the block again with the image built from the repository.

## Into the laptop

Windows remains on the original drive. Remove that drive intact and keep it as the way to restore the previous machine. Install the sealed drive in its place.

Connect only the Ethernet cable and power on the laptop. Without input, the default menu entry boots after three seconds.

What should happen, in order:

* The laptop screen displays the GRUB menu with seven entries and the first entry selected.
* The machine obtains an address from the router's DHCP server.
* The SSH server answers within about half a minute.

From the Mac:

```sh
ssh -i krffs/rach2_demo_ed25519 root@<the address the router gave it>
# compare the SHA256 fingerprint ssh shows against the one read off the screen
# BEFORE typing yes; if they differ, something else is answering. Abort.
bootctl status
```

`bootctl status` should report `booted: candidate`, the extents of the four boot files, and no armed trial. These values describe the artifact's initial state.

If SSH does not answer, photograph the screen. Netconsole is enabled through `rach2.netconsole`, so a Mac capture with `sudo tcpdump -i en0 -nn -e -s0 'ether proto 0x88b5'` records the boot log even when the machine is unreachable.

If the network has no DHCP server, select `Rach2 (static address)` from the menu. It uses 192.168.0.105/24.

## What to record

* The root and test passwords created for this machine, stored away from the machine.
* The artifact's SHA-256 from `make sealed-test`.
* Every property presented for approval: node, byte size, bus, external or internal location, whole-disk status, partition table before the write, and any model and serial reported by the bridge.
* The readback hash and confirmation that it matched.
* The address assigned by the router and the output of `bootctl status` from the first SSH session.
* The SSH host-key fingerprint generated by the machine and read from the laptop screen. No other station on the segment can write to that channel; without this comparison, the first connection authenticates nothing.
* Any output not predicted by this document.

## What this session does not do

* It does not modify the original 1 TB drive. That drive is removed before installing the sealed drive and provides the way back.
* It does not partition or format a device on the Mac. The complete image includes its partition table.
* It does not install a bootloader on the laptop. The artifact already contains GRUB in the post-MBR region, its menu on partition 1, and recovery on partition 3.
* It requires no later physical access during ordinary operation. Subsequent changes arrive over SSH. If that path fails, follow the recovery section in `tools/Readme.md`, not this file.
