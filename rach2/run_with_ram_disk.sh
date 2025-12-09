#!/bin/sh

# Start the kernel in QEMU, call `make` to generate the kernel
# -serial stdio redirects COM1 output to terminal for debugging
# -initrd loads a RAM disk as a multiboot module
qemu-system-i386 -kernel kernel -initrd ../krffs/file_system.krffs -serial stdio
