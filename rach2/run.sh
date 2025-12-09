#!/bin/sh

# Start the kernel in QEMU, call `make` to generate the kernel
# -serial stdio redirects COM1 output to terminal for debugging
qemu-system-i386 -kernel kernel -serial stdio
