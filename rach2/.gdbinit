# GDB commands for the kernel under QEMU, read by a GDB started in rach2/.
# debug.sh starts QEMU stopped at its first instruction with a GDB server on
# port 1234 (-s -S) and then starts GDB here. These lines load the kernel's
# symbols, attach to that server, and run to k_main. GDB reads a .gdbinit in
# the current directory only where its auto-load safe path allows; `gdb -x
# .gdbinit` reads this one in any case.
file ../build/rach2/kernel
target remote localhost:1234
break k_main
continue
