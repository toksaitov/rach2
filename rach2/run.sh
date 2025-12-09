#!/bin/sh

# Start the kernel in QEMU, call `make` to generate the kernel
# -serial stdio redirects COM1 output to terminal for debugging
#
# To load a RAM disk as a multiboot module, use:
#   qemu-system-i386 -kernel kernel -initrd /path/to/ramdisk.bin -serial stdio
#
qemu-system-i386 -kernel kernel -serial stdio
