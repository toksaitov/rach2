#!/usr/bin/env sh
#
# usb.sh: the USB stack, from a port to a keystroke.
#
# Fifteen boots, because what has to be proved differs in each.
#
# The machine has no 8042 in the eight boots that type a key or move a mouse.
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
#          buffer and the line discipline are two different paths; a key that
#          reached the first says nothing about the second, and every program
#          on the machine reads through the second. ----
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
#         everything a person plugs in: unplug once and the next scan gave that
#         entry to another of them, after which nothing plugged in was ever
#         enumerated again until the machine was rebooted. The tablet here
#         stands for those devices, and the mouse is unplugged and plugged
#         back in the way a person does. The port it comes back on is not named: the
#         emulator hands out the next free one, as a person moving a plug from
#         one socket to another does. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-tablet,bus=xhci.0 \
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
    "expect=Type 'help'" wait=2 key=h key=e key=l key=p key=ret \
    "expect=Available commands" "send=exec /intrprobe" "expect=INTRPROBE OK" \
    > /dev/null 2>&1; then
    note "the controller's messages arrive and are counted: OK"
else
    bad "the controller's messages did not arrive, or the timer found their work"
fi

# ---- 10: the same machine with `rach2.nomsi`, which is the kernel as it was
#          before this milestone: no vector, no fallback, and a keyboard that
#          still types. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max --append "rach2.nomsi" \
    --qemu-arg=-machine --qemu-arg=pc,i8042=off \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci,msi=on \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --timeout 120 --steptmo 45 --ok "INTRPROBE OK" --never "INTRPROBE FAIL" \
    "expect=Type 'help'" wait=2 key=h key=e key=l key=p key=ret \
    "expect=Available commands" "send=exec /intrprobe" "expect=INTRPROBE OK" \
    > /dev/null 2>&1; then
    note "with rach2.nomsi the timer pumps the controller and typing still works: OK"
else
    bad "with rach2.nomsi the machine did not type, or took a message anyway"
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
    "expect=Type 'help'" wait=2 \
    mouse=20,-8,0 mouse=-12,6,0 mouse=8,8,1 wait=1 \
    "send=exec /intrprobe" "expect=INTRPROBE OK" > /dev/null 2>&1; then
    note "a mouse moved over messages has its reports counted on the vector: OK"
else
    bad "a mouse moved over messages had no reports counted on the vector"
fi

# ---- 12: the stages, recorded. The kernel stamps the controller's event, the
#          handler's entry, the keystroke reaching the line discipline and the
#          reader taking it, and prints them from the read. The handler's
#          entry is required to be within a millisecond of the event it was
#          raised for, which is what says the key did not wait for a tick, and
#          the reader is required to have run within two milliseconds of the
#          wake, which is what says the message's wake put it on the processor
#          instead of the next tick. The program reads once and writes
#          afterwards, so no time of its own is inside the numbers. ----
LAT_LOG="$IMAGE.latency.log"
rm -f "$LAT_LOG"
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-machine --qemu-arg=pc,i8042=off \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci,msi=on \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --log "$LAT_LOG" \
    --timeout 120 --steptmo 45 --ok "KEYLAT OK" \
    "expect=Type 'help'" wait=2 "send=exec /keylatprobe" wait=2 key=ret \
    > /dev/null 2>&1; then
    if ! grep -q "usb: latency:" "$LAT_LOG"; then
        bad "the keystroke's path was not recorded at all"
    elif ! grep -q "usb: latency: msi 1" "$LAT_LOG"; then
        bad "the keystroke was not brought by a message"
    elif ! awk '/usb: latency: msi 1/ {
                    fields = 0;
                    for (i = 1; i <= NF; i++) {
                        if ($i == "entry") { entry = $(i + 1) + 0; fields++; }
                        if ($i == "wake") { wake = $(i + 1) + 0; fields++; }
                        if ($i == "read") { read = $(i + 1) + 0; fields++; }
                    }
                    if (fields != 3) { short_record = 1; }
                    if (entry < 0) { entry = -entry; }
                    if (entry > 1000) { late = 1; }
                    if (read < wake) { backwards = 1; }
                    if (read - wake > 2000) { slow = 1; }
                    seen = 1;
                }
                END { exit !(seen && !short_record && !late && !backwards && !slow) }' \
            "$LAT_LOG"; then
        bad "the record is short, or the handler entered late, or the reader waited for a tick"
    else
        note "the four stages are recorded, the handler enters at its event and the reader runs on the wake: OK"
        grep "usb: latency" "$LAT_LOG" | sed 's/^/usb-test:   /'
    fi
    if grep -q "usb: pump:" "$LAT_LOG"; then
        bad "the pump enabled interrupts inside itself"
    fi
else
    bad "the latency boot did not read its line"
fi
# The log is kept when something failed, since it is the only account of what
# the machine did.
[ "$fail" -eq 0 ] && rm -f "$LAT_LOG"

# ---- 13: a machine that loses its edges. `rach2.msidrop=N` silences the
#          controller after N messages, which no hardware switch can do on
#          demand, and the machine must go on typing on the timer's pump
#          alone with the count of what it found rising. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max --append "rach2.msidrop=3" \
    --qemu-arg=-machine --qemu-arg=pc,i8042=off \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci,msi=on \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --timeout 120 --steptmo 45 --ok "INTRPROBE OK" --never "INTRPROBE FAIL" \
    "expect=Type 'help'" wait=2 key=h key=e key=l key=p key=ret \
    "expect=Available commands" "send=exec /intrprobe" "expect=INTRPROBE OK" \
    > /dev/null 2>&1; then
    note "a machine whose messages stop keeps typing on the timer's pump: OK"
else
    bad "a machine whose messages stop did not keep typing, or counted nothing"
fi

# ---- 14: the 8259's own interrupts, with the APIC between them and the
#          processor. The model in the host test cannot prove this: it takes a
#          machine with a PS/2 keyboard, whose keystroke arrives on the
#          virtual-wire path, and a timer whose vector must be counting. ----
if run_harness --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci,msi=on \
    --timeout 120 --steptmo 45 --ok "INTRPROBE OK" --never "INTRPROBE FAIL" \
    "expect=Type 'help'" wait=2 key=h key=e key=l key=p key=ret \
    "expect=Available commands" "send=exec /intrprobe" "expect=INTRPROBE OK" \
    > /dev/null 2>&1; then
    note "with the APIC enabled the 8259's keyboard and timer still arrive: OK"
else
    bad "with the APIC enabled the 8259's interrupts did not arrive"
fi

if [ "$fail" -ne 0 ]; then
    echo "usb-test: FAIL" >&2
    exit 1
fi
echo "usb-test: PASS"
