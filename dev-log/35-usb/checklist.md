# The USB session on the notebook

Every step needs physical access to the Lenovo G50-70. Nothing here writes the SSD, and nothing here can leave the machine without input: its own keyboard and touchpad are on the 8042, which this milestone does not touch. If the USB stack goes wrong, `rach2.nousb` on the boot line switches it off and the machine boots as it did before.

The steps are in order because each answers a question the next one needs.

## Before starting

On the Mac:

```sh
make                      # everything, including the metal kernel and image
tests/run usb             # the stack against QEMU, seven boots
tests/run metal           # the metal image rehearsed unattended, USB included
```

Have ready: an external USB keyboard, an external USB mouse, and the notebook on the same network as the Mac, reachable over SSH the way `dev-log/24-metal/checklist.md` sets up.

## 1. What the firmware does before Rach2 exists

Boot the notebook to its firmware setup and record, without changing anything:

* whether VT-d, or anything named for virtualisation of I/O, is enabled
* whether there is a setting for USB legacy support, XHCI hand-off, or a choice between "smart auto", "auto", "enabled" and "disabled" for the USB controller
* the exact wording and current value of each

Those three decide what the driver meets. Write them into `dev-log/35-usb/hardware.md` under the notebook's column.

If VT-d is enabled, stop here and disable it before going on. This kernel gives the controller physical addresses and installs no mappings, so with translation active its registers answer and its transfers do not, and what that looks like is a machine that hangs in bring-up rather than one that says why. The network card has the same constraint and the same answer.

## 2. Whether the firmware already types for you

With the external USB keyboard plugged in, boot Rach2 from the stick **with `rach2.nousb` on the boot line** and, at the login prompt, type on the USB keyboard. The switch matters: with the stack running, a key that appears may have come through it, and the question here is what the firmware does on its own.

If characters appear, the firmware is emulating a PS/2 keyboard out of the USB one through system management mode, and it is doing so while Rach2 runs. Record that. It changes nothing about the driver, and it tells the reader of the record what the driver is for: the same keyboard working when no firmware is emulating anything.

Then type on the notebook's own keyboard and confirm that it also works. It should; it is the recovery path for every step after this.

## 3. What the machine reports

This is the first boot with the stack running on that machine, so watch the screen while it happens rather than waiting for SSH: a machine that stops in bring-up stops before it has a network, and the last line on the screen is then the only evidence there is.

Boot with the ordinary metal entry and read the log over SSH:

```sh
ssh -i <key> root@<notebook> 'cat /dev/bootlog' | grep 'usb:'
```

or run the acceptance script, which prints the same lines with everything else it checks:

```sh
sh tests/system/accept/metal_accept.sh <notebook>
```

Record every `usb:` line in `hardware.md`, beside QEMU's column. The lines to look for:

* `usb: controller:` once per controller. The notebook should show two: an xHCI at `0:20.0` and an EHCI at `0:29.0`.
* `usb: routing:` for the Intel xHCI: `usb2 0xN of 0xM, usb3 0xN of 0xM`. The first number of each pair is where the pins currently go, the second is which pins may be moved at all. A bit set means the pin belongs to the xHCI.
* `usb: ready:` with the version, the port count, the slot count and the context size. This is the first line that says the driver got the controller from the firmware.
* `usb: port:` for each port with something on it, and `usb: device:` for each device enumerated.
* `usb: absent:` or `usb: refused:` instead, with the reason, if it did not.

## 4. What the answers mean

**If there is a `usb: device:` line for the keyboard and the mouse**, the stack works on the notebook. Type on the USB keyboard at the console and move the USB mouse; run `cat /proc/usb` over SSH and see them listed. Phase 6 is done and nothing below is needed.

**If `usb: ready:` appears but no port has anything on it**, the controller came up and the ports are not its own. Read the routing line: if `usb2` is zero, or the bits for the ports you plugged into are clear, the firmware left those pins on the companion EHCI. That is the question for the owner in step 5.

**If `usb: refused: the firmware never gave up the controller`**, the firmware holds it and did not answer the request in a second. Note whether the firmware has a hand-off setting from step 1 and what it is set to.

**If `usb: refused:` names the scratchpad pages**, the controller wants more than the 32 this kernel compiles in. The record says both numbers; raise `XHCI_MAX_SCRATCHPADS` in `rach2/sys/usb/xhci.h` to what it asked for and build again.

**If nothing after `usb: controller:` appears at all**, the machine stopped in bring-up. Reboot with `rach2.nousb` and record where the log ends.

## 5. The routing question, which is the owner's

Only if step 4 left the ports on the EHCI.

The chipset can move each USB2 pin between the two controllers, and the register that does it is `XUSB2PR` at configuration offset `0xD0` of the xHCI, masked by `XUSB2PRM` at `0xD4`. Writing it changes the running machine until its next power cycle. It writes nothing to disk and survives no reboot.

This kernel has no writer for it. Nothing in the tree writes `XUSB2PR` or any other chipset register, and adding one is a code change with its own review and its own host-test rows: it would have to check the vendor and device identifiers and the function it is writing to, change only the bits `XUSB2PRM` says may move, read the register back, and reset the ports it moved.

So this step is a decision and not an action. Present to the owner: the two values as read, which bits are movable, and which ports they correspond to. The owner decides whether such a writer is worth building. Record the decision, whichever way it goes, in `notes.md`. If it is built, the recovery from a bad write is a cold power cycle, because the routing survives a warm reboot.

## 6. What to record either way

In `hardware.md`: the notebook's controllers, its routing registers, its capability values, its scratchpad count, its context size, and the firmware settings from step 1. The `usb: caps:` and `usb: handoff:` lines in the log carry the raw register words for all of them, including the legacy-support registers before and after the handoff.

In `notes.md`: what the machine did that QEMU did not, and the owner's decision from step 5 if it was needed.

In `dev-log/35-usb/Readme.md`: the notebook's half of the trust statement, which is at present a paragraph saying it is not done.

If the machine cannot be made to work, the plan's "If the laptop refuses" applies: the milestone does not grow an EHCI driver to get around it. The owner is shown the evidence and decides whether the milestone closes with QEMU alone or stays open.
