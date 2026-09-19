# dmesg(1)

NAME: dmesg - what this machine printed while it was starting. Source: krffs/dmesg.script (staged as /bin/dmesg on os-real, metal and recovery).
* `dmesg` takes no arguments: Linux's options are about a ring buffer this kernel does not keep, so an argument is refused (`dmesg: this machine's boot log is /dev/bootlog and takes no options`, exit 1) rather than ignored. It prints `/dev/bootlog`, the log the kernel fills as it starts, which any program that reads a file can read (root's). BusyBox's own dmesg applet is not built, since it could only fail here.
* SEE ALSO: bootlog(4), rach2(7).
