# mtools

mtools reads and writes FAT volumes in the test runner's Rach2 image helper. The host attaches a private disk image to that helper and runs `/bin/mtools -c COMMAND` through the framed guest channel. The tests need no installed mtools program on the host.

The source is GNU mtools 4.0.49 from `https://ftp.gnu.org/gnu/mtools/mtools-4.0.49.tar.gz`, under GPL version 3 or later. `build_mtools.sh` verifies its SHA-256 before extraction and builds its multicall program against the ported musl. X11 and the floppy daemon are disabled. `make mtools ARCH=i686` or `make mtools ARCH=x86_64` stages the stripped program under the architecture's `build/vendor/mtools/`. The root Makefile owns the build; do not run the script by hand.

The image helper uses ordinary image files supplied as raw disks. It neither opens the host's devices nor mounts the tested FAT volume. The machine suites compare its output with the kernel's result.
