#!/usr/bin/env sh
#
# usb.sh: the USB stack, from a port to a keystroke.
#
# Nine boots, because what has to be proved differs in each.
#
# The machine has no 8042 in the three boots that type a key or move a mouse.
# That is the whole point of the option: with a PS/2 keyboard and mouse
# present, a typed key or a moved mouse could have arrived by either road, and
# a check that passes either way says nothing about the one this milestone
# built. QEMU's `sendkey` and `mouse_move` go to whatever input device the
# machine has, so taking the other one away is what makes the answer mean
# something.
#
# The devices are bound to the controller by name rather than left to QEMU's
# automatic placement, for the same reason.
#
# Usage: usb.sh KERNEL IMAGE HARNESS
set -u

KERNEL=$1
IMAGE=$2
HARNESS=$3

fail=0
note() { echo "usb-test: $1"; }
bad() { echo "usb-test: $1" >&2; fail=1; }

run_harness() {
    "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" "$@"
}

# ---- 1: a keyboard and a mouse at high speed are enumerated, and the guest
#         reads them out of /proc/usb ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0 \
    --timeout 90 "probe=usbprobe" > /dev/null 2>&1; then
    note "a high-speed keyboard and mouse are enumerated and listed: OK"
else
    bad "a high-speed keyboard and mouse were not enumerated"
fi

# ---- 2: the same at full speed, which is the speed a real keyboard uses and
#         the one QEMU offers only when it is asked ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,usb_version=1 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,usb_version=1 \
    --timeout 90 "probe=usbprobe" > /dev/null 2>&1; then
    note "a full-speed keyboard and mouse are enumerated and listed: OK"
else
    bad "a full-speed keyboard and mouse were not enumerated"
fi

# ---- 3: a key typed on the USB keyboard reaches the shell, on a machine with
#         no other keyboard at all ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-machine --qemu-arg=pc,i8042=off \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --timeout 90 --ok "Available commands" \
    "expect=Type 'help'" wait=2 key=h key=e key=l key=p key=ret \
    "expect=Available commands" > /dev/null 2>&1; then
    note "a key typed on the USB keyboard reaches the shell: OK"
else
    bad "a key typed on the USB keyboard did not reach the shell"
fi

# ---- 3b: and a program reading the terminal gets it too. The shell's own
#          buffer is one road and the line discipline is another; a key that
#          reached the first says nothing about the second, and it is the
#          second that every program on the machine reads through. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-machine --qemu-arg=pc,i8042=off \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --timeout 120 --steptmo 45 --ok "All stdin syscalls passed" \
    "expect=Type 'help'" wait=2 "send=exec /stdintest" wait=2 \
    key=a key=b key=ret key=c key=d key=ret key=e key=f key=ret \
    > /dev/null 2>&1; then
    note "a key typed on the USB keyboard is read by a program: OK"
else
    bad "a key typed on the USB keyboard was not read by a program"
fi

# ---- 4: a mouse moved on the wire arrives at the mouse device, again with no
#         other mouse on the machine. The probe is started first and reads
#         until something arrives, because opening the device empties the
#         queue; it exits as soon as a movement does, which stops the machine,
#         so nothing is asked of it afterwards. The token is what says it
#         worked. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-machine --qemu-arg=pc,i8042=off \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0 \
    --timeout 90 --ok "MOUSEPROBE OK" \
    "expect=Type 'help'" wait=2 "send=exec /mouseprobe" wait=1 \
    mouse=20,-8,0 > /dev/null 2>&1; then
    note "a mouse moved over USB arrives at /dev/mouse: OK"
else
    bad "a mouse moved over USB did not arrive at /dev/mouse"
fi

# ---- 5: a device plugged in after the boot is enumerated, and one pulled out
#         is taken down ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --timeout 120 --steptmo 45 --ok "port 5: gone" \
    "expect=Type 'help'" \
    "usb=add:usb-kbd,bus=xhci.0,id=later" \
    "expect=usb: device: port 5:" \
    "usb=del:later" "expect=port 5: gone" > /dev/null 2>&1; then
    note "a device plugged in after the boot is enumerated and one pulled out is taken down: OK"
else
    bad "hotplug did not work"
fi

# ---- 6: switched off, the file is there and empty. A missing file and an
#         empty one are different answers, and a program that opens this one
#         has to get the second. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max --append "rach2.nousb" \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --timeout 90 "probe=usbprobe" > /dev/null 2>&1; then
    note "with USB switched off the file is there and empty: OK"
else
    bad "with USB switched off the file was not there, or was not empty"
fi

# ---- 7: a device this kernel cannot drive is refused once and left alone,
#         and the devices beside it are still enumerated. Both halves matter:
#         the notebook found a device of this kind and the stack enumerated it
#         again every 150 ms until it ran out of slots, which cost the keyboard
#         that was already working. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-hub,bus=xhci.0 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0 \
    --timeout 90 "probe=usbprobe" > /dev/null 2>&1; then
    note "a device that cannot be driven does not stop the ones that can: OK"
else
    bad "a device that cannot be driven stopped the ones that can"
fi

# ---- 8: a device that cannot be driven does not consume the table. The
#         notebook has three of them soldered to it, and while a refusal was
#         remembered by holding a table entry, those three left one entry for
#         everything a person plugs in: unplug once and the next scan gave that
#         entry to another of them, after which nothing plugged in was ever
#         enumerated again until the machine was rebooted. The hub here stands
#         for those devices, and the mouse is unplugged and plugged back in the
#         way a person does. The port it comes back on is not named: the
#         emulator hands out the next free one, as a person moving a plug from
#         one socket to another does. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-hub,bus=xhci.0 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,id=themouse \
    --log "$IMAGE.replug.log" \
    --timeout 120 --steptmo 45 --ok "class 3 protocol 2, endpoint 1" \
    "expect=usb: devices: 2 configured" \
    "usb=del:themouse" "expect=: gone" \
    "usb=add:usb-mouse,bus=xhci.0,id=themouse" \
    "expect=class 3 protocol 2, endpoint 1" > /dev/null 2>&1; then
    note "a device plugged in again after an unplug is enumerated again: OK"
else
    bad "a device plugged in again after an unplug was not enumerated"
fi

if [ "$fail" -ne 0 ]; then
    echo "usb-test: FAIL" >&2
    exit 1
fi
echo "usb-test: PASS"
