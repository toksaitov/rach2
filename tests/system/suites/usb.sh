#!/usr/bin/env sh
#
# usb.sh: the USB stack, from a port to a keystroke.
#
# Twenty-one boots, because what has to be proved differs in each.
#
# The machine has no 8042 in the eleven boots that type a key or move a mouse.
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
# What this suite writes under the images directory carries its name.
OUT="$(dirname "$IMAGE")/usb-test"
HARNESS=$3
REPO=$(cd "$(dirname "$0")/../../.." && pwd)

if [ "${RACH2_USB_ROWS:-all}" = latency ]; then
    exec sh "$REPO/tests/system/usb_latency.sh" "$KERNEL" "$IMAGE" "$HARNESS"
fi

fail=0
note() { echo "usb-test: $1"; }
bad() { echo "usb-test: $1" >&2; fail=1; }

run_harness() {
    "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" "$@"
}
# The harness kernel ends the machine when its first process exits, which is
# right for a boot that runs one probe; a probe run in parts needs the
# ordinary kernel and its debug shell between the parts.
run_shell() {
    "$HARNESS" --kernel "$REPO/build/rach2/kernel" --initrd "$IMAGE" "$@"
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
    "seen=class 3 protocol 1" "seen=Type 'help'" key=h key=e key=l key=p key=ret \
    "expect=Available commands" > /dev/null 2>&1; then
    note "a key typed on the USB keyboard reaches the shell: OK"
else
    bad "a key typed on the USB keyboard did not reach the shell"
fi

# ---- 3b: and a program reading the terminal gets it too. The shell's own
#          buffer and the line discipline are two different paths; a key that
#          reached the first says nothing about the second, and every program
#          on the machine reads through the second. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-machine --qemu-arg=pc,i8042=off \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --timeout 120 --steptmo 45 --ok "All stdin syscalls passed" \
    "seen=class 3 protocol 1" "seen=Type 'help'" "send=exec /stdintest" \
    "expect=Test 1: read from stdin" \
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
    "seen=class 3 protocol 2" "seen=Type 'help'" "send=exec /mouseprobe" \
    "expect=MOUSEPROBE: move the mouse" \
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

# ---- 6b: more devices than the table held before milestone 50: two hubs
#          on two root ports with fourteen devices between them, sixteen in
#          all, every one configured. A hub's last port is scanned like its
#          first, which is what a table that fills from behind a hub needs. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-hub,bus=xhci.0,port=1,id=hub1 \
    --qemu-arg=-device --qemu-arg=usb-hub,bus=xhci.0,port=2,id=hub2 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.1 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.2 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.3 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.4 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.5 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.6 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.7 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.8 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=2.1 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=2.2 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=2.3 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=2.4 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=2.5 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=2.6 \
    --timeout 120 --log "$REPO/build/images/usb-sixteen.log" "probe=usbprobe" > /dev/null 2>&1 \
    && [ "$(grep -ac 'usb: device: port [0-9.]*: .*class' "$REPO/build/images/usb-sixteen.log")" = 16 ]; then
    note "two hubs with fourteen devices between them, sixteen in all, are every one configured: OK"
else
    bad "sixteen devices were not every one configured: $(grep -ac 'usb: device: port [0-9.]*: .*class' "$REPO/build/images/usb-sixteen.log") configured, $(grep -a 'USBPROBE' "$REPO/build/images/usb-sixteen.log" | tr -d '\r' | tail -1)"
fi

# ---- 6c: the same sixteen with the hubs' scans slowed to 300 ms a port
#          (`rach2.hubslow`), so a device found on an early port settles
#          while the scan is still reading later ones, as it does on a
#          loaded host: a connection told to the core before the scan ends
#          has its reset refused by a hub that cannot take one yet, and the
#          port is given up. Every device must still be configured; the
#          probe is run once the slowed scans have had their time. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-hub,bus=xhci.0,port=1,id=hub1 \
    --qemu-arg=-device --qemu-arg=usb-hub,bus=xhci.0,port=2,id=hub2 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.1 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.2 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.3 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.4 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.5 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.6 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.7 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.8 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=2.1 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=2.2 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=2.3 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=2.4 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=2.5 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=2.6 \
    --append "rach2.hubslow=300" \
    --timeout 240 --steptmo 120 --ok "USBPROBE OK" --log "$REPO/build/images/usb-slowscan.log" \
    "expect=Type 'help'" "send=exec usbprobe 16" "expect=USBPROBE settled" "expect=USBPROBE OK" > /dev/null 2>&1 \
    && [ "$(grep -ac 'usb: device: port [0-9.]*: .*class' "$REPO/build/images/usb-slowscan.log")" = 16 ]; then
    note "sixteen devices behind hubs whose scans take 300 ms a port are every one configured: OK"
else
    bad "with the hubs' scans slowed, $(grep -ac 'usb: device: port [0-9.]*: .*class' "$REPO/build/images/usb-slowscan.log") of sixteen devices were configured"
fi

# ---- 7: a device this kernel cannot drive is refused once and left alone,
#         and the devices beside it are still enumerated. Both halves matter:
#         the notebook found a device of this kind and the stack enumerated it
#         again every 150 ms until it ran out of slots, which cost the keyboard
#         that was already working. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-tablet,bus=xhci.0 \
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
#         everything a person plugs in: unplug once and the next scan gave
#         that entry to another of them, after which nothing plugged in was
#         ever enumerated again until the machine was rebooted. The tablet
#         here stands for those devices, and the mouse is unplugged and
#         plugged back in the way a person does. The port it comes back on is
#         not named: the emulator hands out the next free one, as a person
#         moving a plug from one socket to another does. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-tablet,bus=xhci.0 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,id=themouse \
    --log "$OUT.replug.log" \
    --timeout 120 --steptmo 45 --ok "class 3 protocol 2, endpoint 1" \
    "expect=usb: devices: 2 configured" \
    "usb=del:themouse" "expect=: gone" \
    "usb=add:usb-mouse,bus=xhci.0,id=themouse" \
    "expect=class 3 protocol 2, endpoint 1" > /dev/null 2>&1; then
    note "a device plugged in again after an unplug is enumerated again: OK"
else
    bad "a device plugged in again after an unplug was not enumerated"
fi

# ---- 9: the controller's own interrupt. The keys are typed on the USB
#          keyboard, so every one of them is a message, and the probe reads
#          the counts out of /proc/interrupts on the machine: the controller's
#          vector has counted, and the timer's pump has found nothing that a
#          message should have brought. `msi=on` is QEMU's switch for the
#          capability; without it the controller offers MSI-X alone, which
#          this kernel refuses. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-machine --qemu-arg=pc,i8042=off \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci,msi=on \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --timeout 120 --steptmo 45 --ok "INTRPROBE OK" --never "INTRPROBE FAIL" \
    "seen=class 3 protocol 1" "seen=Type 'help'" key=h key=e key=l key=p key=ret \
    "expect=Available commands" "send=exec /intrprobe" "expect=INTRPROBE OK" \
    > /dev/null 2>&1; then
    note "the controller's messages arrive and are counted: OK"
else
    bad "the controller's messages did not arrive, or the timer found their work"
fi

# ---- 10: the same machine with `rach2.nomsi`: no message vector, the
#          controller's PCI line counting at least one interrupt per key, no
#          fallback, and a keyboard that still types. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max --append "rach2.nomsi" \
    --qemu-arg=-machine --qemu-arg=pc,i8042=off \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci,msi=on \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --timeout 120 --steptmo 45 --ok "INTRPROBE OK" --never "INTRPROBE FAIL" \
    "seen=class 3 protocol 1" "seen=Type 'help'" key=h key=e key=l key=p key=ret \
    "expect=Available commands" "send=exec /intrprobe" "expect=INTRPROBE OK" \
    > /dev/null 2>&1; then
    note "with rach2.nomsi the controller's line carries its interrupts and typing still works: OK"
else
    bad "with rach2.nomsi the machine did not type, took a message anyway, or its line took none"
fi

# ---- 11: a mouse over messages, since a mouse and a keyboard are two
#          different endpoints and the mouse is the one whose reports arrive
#          without anybody asking. What is asked of this boot is the count and
#          not the movement, which boot 4 already proves: a mouse whose
#          messages never arrive is still read by the timer's pump, so a check
#          on the movement alone would pass either way. `mouseprobe` is not run
#          here because it stops the machine when it has its movement, and
#          nothing could be read afterwards. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-machine --qemu-arg=pc,i8042=off \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci,msi=on \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0 \
    --timeout 120 --steptmo 45 --ok "INTRPROBE OK" --never "INTRPROBE FAIL" \
    "seen=class 3 protocol 2" "seen=Type 'help'" \
    mouse=20,-8,0 mouse=-12,6,0 mouse=8,8,1 \
    "send=exec /intrprobe" "expect=INTRPROBE OK" > /dev/null 2>&1; then
    note "a mouse moved over messages has its reports counted on the vector: OK"
else
    bad "a mouse moved over messages had no reports counted on the vector"
fi

if ! sh "$REPO/tests/system/usb_latency.sh" "$KERNEL" "$IMAGE" "$HARNESS"; then
    bad "the keystroke dispatch rows failed"
fi

# ---- 13: the same keystroke without `rach2.usblatency`: the probe reads its
#          key as before and nothing is printed, since on a console the record
#          is a line for every key typed. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-machine --qemu-arg=pc,i8042=off \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci,msi=on \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --timeout 120 --steptmo 45 --ok "KEYLAT OK" --never "usb: latency:" \
    "seen=class 3 protocol 1" "seen=Type 'help'" "send=exec /keylatprobe" \
    "expect=KEYLAT READING" key=ret \
    > /dev/null 2>&1; then
    note "without rach2.usblatency the key is read and no path is printed: OK"
else
    bad "without rach2.usblatency the key was not read, or its path was printed"
fi

# ---- 14: a machine that loses its edges. `rach2.msidrop=N` silences the
#          controller after N messages, which no hardware switch can do on
#          demand, and the machine must go on typing on the timer's pump
#          alone with the count of what it found rising. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max --append "rach2.msidrop=3" \
    --qemu-arg=-machine --qemu-arg=pc,i8042=off \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci,msi=on \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --timeout 120 --steptmo 45 --ok "INTRPROBE OK" --never "INTRPROBE FAIL" \
    "seen=class 3 protocol 1" "seen=Type 'help'" key=h key=e key=l key=p key=ret \
    "expect=Available commands" "send=exec /intrprobe" "expect=INTRPROBE OK" \
    > /dev/null 2>&1; then
    note "a machine whose messages stop keeps typing on the timer's pump: OK"
else
    bad "a machine whose messages stop did not keep typing, or counted nothing"
fi

# ---- 15: the keyboard's line and the timer with messages on, the I/O APIC
#          delivering the line and the local APIC's timer ticking where ACPI
#          lists them. The model in the host test cannot prove this: it takes
#          a machine with a PS/2 keyboard, whose keystroke arrives on its
#          line, and a timer whose vector must be counting. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci,msi=on \
    --timeout 120 --steptmo 45 --ok "INTRPROBE OK" --never "INTRPROBE FAIL" \
    "expect=Type 'help'" key=h key=e key=l key=p key=ret \
    "expect=Available commands" "send=exec /intrprobe" "expect=INTRPROBE OK" \
    > /dev/null 2>&1; then
    note "with messages on, the keyboard's line and the timer still arrive: OK"
else
    bad "with messages on, the keyboard's line or the timer did not arrive"
fi

# ---- 16: a keyboard and a mouse behind a hub are enumerated at their
#          location, listed with their path, and the keyboard types on a
#          machine with no 8042 ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-machine --qemu-arg=pc,i8042=off \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-hub,bus=xhci.0,port=1,id=hub \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.2 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=1.3 \
    --append "rach2.hubtree" \
    --timeout 120 --steptmo 45 --ok "USBPROBE OK" --never "USBPROBE FAIL" \
    "seen=class 3 protocol 1" "seen=Type 'help'" key=h key=e key=l key=p key=ret \
    "expect=Available commands" "send=exec /usbprobe" "expect=USBPROBE OK" \
    > /dev/null 2>&1; then
    note "a keyboard and a mouse behind a hub are listed with their path and the keyboard types: OK"
else
    bad "the keyboard and mouse behind a hub"
fi

# ---- 17: a stick behind the hub mounts, takes a write, is pulled from under
#          a mount through the monitor, and plugged back in mounts again ----
# The stick is this suite's own: the usbdisk suite writes its images in
# place while it runs, and a copy of one taken beside it could be half
# written.
STICK="$OUT.hubstick.img"
dd if=/dev/zero of="$STICK" bs=1048576 count=80 2>/dev/null
"$REPO/build/tools/image/mkmbr" --lba "$STICK" 2048:98304:0c > /dev/null
mformat -i "$STICK"@@1048576 -F -T 98304 -v STICKFAT ::
echo "hello from the stick" > "$STICK.hello"
mcopy -i "$STICK"@@1048576 "$STICK.hello" ::/hello.txt
awk 'BEGIN { for (i = 0; i < 128; i++) for (j = 0; j < 128; j++) printf "%03d.", i }' \
    > "$STICK.pattern"
mcopy -i "$STICK"@@1048576 "$STICK.pattern" ::/pattern.bin
if run_shell --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-hub,bus=xhci.0,port=1,id=hub \
    --drive "if=none,id=stick,format=raw,file=$STICK" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,port=1.4,drive=stick,id=st0 \
    --timeout 180 --steptmo 60 --ok "USBDISKPROBE OK" --never "USBDISKPROBE FAIL" \
    --log "$OUT.hubstick.log" \
    "expect=Type 'help'" "send=exec /usbdiskprobe write" "expect=USBDISKPROBE OK" \
    "expect=Type 'help'" "send=exec /usbdiskprobe hold" "expect=USBDISKPROBE HOLDING" \
    "usb=del:st0" "expect=USBDISKPROBE OK" \
    "expect=Type 'help'" \
    "drive=add:if=none,id=again,format=raw,file=$STICK" \
    "usb=add:usb-storage,bus=xhci.0,port=1.4,drive=again,id=st1" "expect=usbdisk: disk0: mbr" \
    "send=exec /usbdiskprobe replug" "expect=USBDISKPROBE OK" > /dev/null 2>&1; then
    note "a stick behind the hub is written, pulled from under a mount and mounts again: OK"
else
    bad "the stick behind the hub"
fi

# ---- 17b: a 32 KiB read in flight when the stick is pulled: the request is
#           failed only after the controller has given the slot back, since
#           the request's pages are the controller's until then, which the
#           driver's line at the close says. The stick's medium is throttled
#           to 32 KiB a second, so a read's data stage lasts a second and the
#           pull lands inside it; unthrottled, the emulator completes each
#           stage inside its doorbell and the pull lands between stages, and
#           the emulator gives the slot back within a millisecond, so the
#           order of the process's line and the kernel's proves nothing.
#           The next stick enumerates after. ----
if run_shell --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-object --qemu-arg=throttle-group,id=tgs,x-bps-total=32768 \
    --qemu-arg=-blockdev --qemu-arg="driver=file,filename=$STICK,node-name=stickfile" \
    --qemu-arg=-blockdev --qemu-arg=driver=raw,file=stickfile,node-name=stickraw \
    --qemu-arg=-blockdev --qemu-arg=driver=throttle,throttle-group=tgs,file=stickraw,node-name=stick \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,id=st0 \
    --timeout 180 --steptmo 60 --ok "USBDISKPROBE OK" --never "USBDISKPROBE FAIL" \
    --log "$OUT.pullread.log" \
    "expect=Type 'help'" "send=exec /usbdiskprobe pullread" "expect=USBDISKPROBE READING" \
    "usb=del:st0" "expect=its request is kept, its data stage in flight" \
    "expect=: given back" "expect=its request is failed with the slot given back" \
    "expect=USBDISKPROBE READ FAILED" \
    "expect=Type 'help'" \
    "drive=add:if=none,id=again,format=raw,file=$STICK" \
    "usb=add:usb-storage,bus=xhci.0,drive=again,id=st1" "expect=: mbr" > /dev/null 2>&1; then
    note "a 32 KiB read in flight when the stick is pulled fails after the slot is given back, and the next stick enumerates: OK"
else
    bad "the read in flight at the pull"
fi

# ---- 18: the hub pulled with all three behind it: the stick's mount answers
#          EIO and unmounts once its file is closed, and every device below
#          the hub goes before the hub does ----
if run_shell --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-hub,bus=xhci.0,port=1,id=hub \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.2 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=1.3 \
    --drive "if=none,id=stick,format=raw,file=$STICK" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,port=1.4,drive=stick,id=st0 \
    --timeout 180 --steptmo 60 --ok "USBDISKPROBE OK" --never "USBDISKPROBE FAIL" \
    --log "$OUT.hubpull.log" \
    "expect=Type 'help'" "send=exec /usbdiskprobe hold" "expect=USBDISKPROBE HOLDING" \
    "usb=del:hub" "expect=USBDISKPROBE OK" > /dev/null 2>&1; then
    note "the hub pulled with three devices behind it: the stick's mount answers EIO and unmounts: OK"
else
    bad "the hub pulled with three devices behind it"
fi
if [ "$(grep -ac 'usb: device: port [0-9]*\.[0-9]*: gone' "$OUT.hubpull.log")" = 3 ] && \
   grep -a 'gone' "$OUT.hubpull.log" | tail -1 | grep -aq 'port [0-9]*: gone'; then
    note "and the three below went before the hub: OK"
else
    bad "the devices below the hub did not all go before it"
fi
if [ "$(grep -ac 'usb: slot: [0-9]*: given back' "$OUT.hubpull.log")" = 4 ] && \
   ! grep -aq 'the entry is lost' "$OUT.hubpull.log"; then
    note "and all four slots were given back: OK"
else
    bad "the four slots were not all given back"
fi

# ---- 19: a hub behind a hub, with a keyboard on the second, typing ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-machine --qemu-arg=pc,i8042=off \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-hub,bus=xhci.0,port=1,id=hub \
    --qemu-arg=-device --qemu-arg=usb-hub,bus=xhci.0,port=1.1,id=hub2 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.1.2 \
    --timeout 120 --steptmo 45 --ok "Available commands" --log "$OUT.hubhub.log" \
    "seen=class 3 protocol 1" "seen=Type 'help'" key=h key=e key=l key=p key=ret \
    "expect=Available commands" > /dev/null 2>&1 && \
   grep -aq 'usb: device: port [0-9]*\.1\.2: 627:1, class 3 protocol 1' "$OUT.hubhub.log"; then
    note "a keyboard behind a hub behind a hub is at its path and types: OK"
else
    bad "the hub behind a hub"
fi

# ---- 20: the device table filled: two hubs and fifteen devices are sixteen
#          entries, and the seventeenth is refused by name ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-hub,bus=xhci.0,port=1,id=hub \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.1 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=1.2 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.3 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=1.4 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.5 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=1.6 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.7 \
    --qemu-arg=-device --qemu-arg=usb-hub,bus=xhci.0,port=1.8,id=hub2 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=1.8.1 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.8.2 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=1.8.3 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.8.4 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=1.8.5 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.8.6 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0,port=1.8.7 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0,port=1.8.8 \
    --timeout 120 --steptmo 60 --ok "the device table is full" --log "$OUT.hubfull.log" \
    "seen=the device table is full" "seen=Type 'help'" "send=help" "expect=Available commands" \
    > /dev/null 2>&1; then
    note "sixteen devices fill the table and the seventeenth is refused by name: OK"
else
    bad "the device table boot"
fi
if [ "$(grep -ac 'usb: device: port [0-9.]*: .*class' "$OUT.hubfull.log")" = 16 ]; then
    note "and sixteen were configured: OK"
else
    bad "sixteen devices were not configured: $(grep -ac 'usb: device: port [0-9.]*: .*class' "$OUT.hubfull.log")"
fi

# ---- two controllers: each is driven from its own record, with its own
#      rings and its own interrupt. A mouse on the first, and on the second a
#      hub with a keyboard and a mouse behind it. The machine's only keyboard
#      is the one behind that hub, so a shell that answers what was typed is
#      the second controller delivering transfers, and it is the second
#      controller because there is nothing else the keystrokes could have
#      come from. The 8042 is off for the same reason. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-machine --qemu-arg=pc,i8042=off \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci0 \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci1 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci0.0 \
    --qemu-arg=-device --qemu-arg=usb-hub,bus=xhci1.0,port=1,id=hub3 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci1.0,port=1.1 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci1.0,port=1.2 \
    --timeout 120 --steptmo 60 --ok "Available commands" --log "$OUT.twohc.log" \
    "seen=class 3 protocol 1" "seen=Type 'help'" key=h key=e key=l key=p key=ret \
    "expect=Available commands" > /dev/null 2>&1; then
    note "the only keyboard, behind a hub on the second controller, types: OK"
else
    bad "the two-controller boot"
fi
if [ "$(grep -ac 'usb: ready: xhci[01]:' "$OUT.twohc.log")" = 2 ]; then
    note "and both controllers came up, each named by its own number: OK"
else
    bad "both controllers did not come up"
fi
if [ "$(grep -ac 'usb: device: port .*class' "$OUT.twohc.log")" = 4 ]; then
    note "and all four devices were configured across them: OK"
else
    bad "the four devices across two controllers were not all configured"
fi
# Each controller has an interrupt of its own, by message or by line: two
# controllers on one vector cannot both be told to stop. A controller the timer
# pumps has none at all, which the boot says in as many words.
v0="$(sed -n 's/.*interrupts: xhci0: .*vector \([0-9]*\).*/\1/p' "$OUT.twohc.log" | head -1)"
v1="$(sed -n 's/.*interrupts: xhci1: .*vector \([0-9]*\).*/\1/p' "$OUT.twohc.log" | head -1)"
if [ -n "$v0" ] && [ -n "$v1" ] && [ "$v0" != "$v1" ] &&
    ! grep -aq 'the timer pumps this controller' "$OUT.twohc.log"; then
    note "and each takes an interrupt of its own, $v0 and $v1: OK"
else
    bad "the two controllers did not each take an interrupt of their own"
fi

# ---- sticks on both controllers, and one pulled from under a reader while
#      the other controller's stick is in use. Two boots, one for each
#      direction: the sticks are enumerated controller by controller, so the
#      one the probe holds, `/dev/disk1`, is on the second controller in the
#      first boot and on the first in the second. What the row requires is
#      that the pull answers the reader that held it and leaves the stick on
#      the other controller mounting and reading. ----
for i in 0 1 2; do
    cp "$STICK" "$OUT.twohc-stick-$i.img" || bad "the two-controller sticks"
done
twohc_sticks() {
    where0=$1
    where1=$2
    where2=$3
    shift 3
    run_shell --qemu-arg=-cpu --qemu-arg=max \
        --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci0 \
        --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci1 \
        --drive "if=none,id=stick0,format=raw,file=$OUT.twohc-stick-0.img" \
        --drive "if=none,id=stick1,format=raw,file=$OUT.twohc-stick-1.img" \
        --drive "if=none,id=stick2,format=raw,file=$OUT.twohc-stick-2.img" \
        --qemu-arg=-device --qemu-arg="usb-storage,bus=$where0.0,drive=stick0,id=st0" \
        --qemu-arg=-device --qemu-arg="usb-storage,bus=$where1.0,drive=stick1,id=st1" \
        --qemu-arg=-device --qemu-arg="usb-storage,bus=$where2.0,drive=stick2,id=st2" \
        "$@"
}
# One stick on the first controller and two on the second. The core enumerates
# one device per controller in turn, so `/dev/disk1` is the second
# controller's first stick, and pulling it leaves the first controller's.
if twohc_sticks xhci0 xhci1 xhci1 \
    --timeout 180 --steptmo 60 --ok "USBDISKPROBE OK" --never "USBDISKPROBE FAIL" \
    --log "$OUT.twohcstick.log" \
    "expect=Type 'help'" "send=exec /usbdiskprobe three" "expect=USBDISKPROBE OK" \
    "expect=Type 'help'" "send=exec /usbdiskprobe pullone" "expect=USBDISKPROBE HOLDING" \
    "usb=del:st1" "expect=USBDISKPROBE OK" > /dev/null 2>&1; then
    note "three sticks across two controllers mount, and one pulled from the second leaves the first's reading: OK"
else
    bad "the sticks across two controllers"
fi
# And the other way. The sticks are enumerated one controller at a time in
# turn, so with two on the first controller and one on the second the disks
# come out as st0, st2, st1: `/dev/disk2` is the first controller's second
# stick, and pulling it leaves a stick on each controller behind.
if twohc_sticks xhci0 xhci0 xhci1 \
    --timeout 180 --steptmo 60 --ok "USBDISKPROBE OK" --never "USBDISKPROBE FAIL" \
    --log "$OUT.twohcstick2.log" \
    "expect=Type 'help'" "send=exec /usbdiskprobe three" "expect=USBDISKPROBE OK" \
    "expect=Type 'help'" "send=exec /usbdiskprobe pullone 2" "expect=USBDISKPROBE HOLDING" \
    "usb=del:st1" "expect=USBDISKPROBE OK" > /dev/null 2>&1; then
    note "and one pulled from the first controller leaves the second's reading: OK"
else
    bad "the stick pulled from the first controller"
fi

# ---- one controller refused at its bring-up: the other still comes up and
#      the keyboard on it still types. `rach2.hcfail=0` refuses the first,
#      which is the case no QEMU machine has and the one the bring-up loop was
#      written for. What was taken from the firmware goes back and the record
#      is closed, so the controller that is left is numbered from zero and the
#      devices on it are enumerated as though the other had never been there.
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-machine --qemu-arg=pc,i8042=off \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci0 \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci1 \
    --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci0.0 \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci1.0 \
    --append "rach2.hcfail=0" \
    --timeout 120 --steptmo 60 --ok "Available commands" --log "$OUT.hcfail.log" \
    "seen=class 3 protocol 1" "seen=Type 'help'" key=h key=e key=l key=p key=ret \
    "expect=Available commands" > /dev/null 2>&1; then
    note "a controller refused at its bring-up leaves the other one serving: OK"
else
    bad "the refused-controller boot"
fi
if grep -aq 'usb: refused: xhci0: rach2.hcfail=0' "$OUT.hcfail.log" &&
    [ "$(grep -ac 'usb: ready: xhci' "$OUT.hcfail.log")" = 1 ]; then
    note "and it was named with its reason while one controller came up: OK"
else
    bad "the refusal was not recorded, or the wrong number of controllers came up"
fi
if [ "$(grep -ac 'usb: device: port .*class' "$OUT.hcfail.log")" = 1 ]; then
    note "and only the device on the controller that came up was enumerated: OK"
else
    bad "the devices behind a refused controller were enumerated anyway"
fi

if [ "$fail" -ne 0 ]; then
    echo "usb-test: FAIL" >&2
    exit 1
fi
echo "usb-test: PASS"
