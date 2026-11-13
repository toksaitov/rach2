#!/bin/sh
# Boot the four-processor PCI affinity probe with its three storage controllers.
set -eu
system=$1
shift
exec qemu-system-x86_64 -machine q35 -accel "${ACCEL:-tcg}" -cpu "${CPU:-max}" -smp 4 -m 512 -display none -monitor none -serial stdio -no-reboot -net none -audiodev driver=none,id=silent -device intel-hda -device hda-duplex,audiodev=silent -device nec-usb-xhci,id=xhci -device usb-kbd,bus=xhci.0 -drive file=build/tmp/4893/usb.raw,format=raw,if=none,id=usb -device usb-storage,bus=xhci.0,drive=usb -drive file=build/tmp/4893/ahci.raw,format=raw,if=none,id=sata -device ich9-ahci,id=ahci -device ide-hd,drive=sata,bus=ahci.0 -drive file=build/tmp/4893/nvme.raw,format=raw,if=none,id=nvm -device nvme,drive=nvm,serial=4893 "$@"
