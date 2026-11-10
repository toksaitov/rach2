# The testbed

`bed/` holds the testbed's own programs, with which Rach2 draws through a Linux virtual machine's GPU (milestone 83, [its plan](../dev-log/83-testbed/plan.md)). Its first rung needs no kernel code: [`vtestrelay.c`](vtestrelay.c) carries Mesa's vtest protocol from its local socket to virglrenderer's test server over TCP, and [`fixture/`](fixture/) keeps the Debian guest that serves vtest and the check that draws a triangle through the relay. The relay is POSIX C that builds for Linux and for Rach2's GNU root. [`nctx/`](nctx/Readme.md) builds the matched Mesa, virglrenderer and QEMU of the native-context rung. [`shm/`](shm/Readme.md) is the shared-memory transport of the Venus rung, and [`vm/`](vm/Readme.md) the Linux VM whose backend serves it, renders through virglrenderer and shows the scanout on the VM's display.

## Running

The guest is prepared once for each clone, and each run boots a new copy of it:

    sh bed/fixture/vtest-guest.sh prepare
    sh bed/fixture/vtest-guest.sh check
    sh bed/fixture/vtest-guest.sh run 'nc -z "$VTEST_HOST" "$VTEST_PORT"'

`prepare` fetches the cloud image that [`tests/graphics/debian-ref/image`](../tests/graphics/debian-ref/image) pins, through `debian-ref.sh fetch`, which checks its size and SHA-512, and boots an overlay of it once with a cloud-init seed. [`fixture/setup.sh.in`](fixture/setup.sh.in) installs [`fixture/packages`](fixture/packages) from Debian's snapshot of the date that file names, adds the services that serve vtest and turns cloud-init off, so that every later boot is the same; the result is `prepared.qcow2`, read-only, in `build/cache/bed-vtest` of the main clone, which its worktrees share, or in `BED_VTEST_CACHE`.

`run` boots an overlay of the prepared image with 4 CPUs and 4096 MiB (`-c`, `-m`), forwards the guest's vtest port, 5555, from a free port on the host's loopback address over QEMU's user network, waits for the guest to report its server ready on the serial console, runs the command in sh on the host with `VTEST_HOST` and `VTEST_PORT` set to that address, stops the guest and removes the overlay. It exits with the command's status. The accelerator is KVM where `/dev/kvm` opens and TCG elsewhere, or what `ACCEL` names, as [`rach2/qemu_devices.sh`](../rach2/qemu_devices.sh) chooses for the graphics runs.

`check` builds the relay and [`fixture/triangle.c`](fixture/triangle.c) into `build/bed` with `CC`, `CFLAGS` and `LDLIBS` (`-ldl` by default; the triangle opens `libEGL.so.1` and takes every EGL and GLES function from its `eglGetProcAddress`, so it needs no development headers), draws the triangle on the host's llvmpipe, then boots the guest and draws it again with Mesa's `virpipe` through the relay, and passes when both pictures check and their checksums are equal.

[vtestrelay(1)](../man/man1/vtestrelay.1) is the relay's manual.

## Results

`check` prints the triangle's lines for each driver, prefixed `vtest-guest: llvmpipe:` and `vtest-guest: virpipe:`, the relay's counts and a last line, `vtest-guest: result: the triangle through the relay equals llvmpipe's, CHECKSUM`, or the reason it failed. On a failure through the relay it prints the last 20 lines of the guest's console, where the server and socat write their diagnostics. `run` and `check` write the guest's record to standard error as `vtest-guest: record:` lines: the versions of `virgl-server`, `libgl1-mesa-dri`, `socat` and the kernel, as the guest reports them, the accelerator, CPUs, memory, QEMU's version, the image with its SHA-512 and the snapshot.

`triangle` draws, in a 256 by 256 framebuffer, a checkered texture of squares of 16 pixels, uploaded as one 256 KiB texture, over the whole picture, and over it a triangle of one color with its corners on pixel edges, at (32, 32), (224, 32) and (128, 224). Each pixel's center falls on the center of its texel, so the nearest sample is exact on any driver. It waits on an EGL fence, reads the picture back and prints its renderer, its FNV-1a checksum and its pixels by kind, and fails unless every pixel is the triangle's color or the texel under it and the triangle's center and the corners are right; `-o FILE` writes the picture as a PPM.

Measured on the testing PC under KVM, with the WSL host's Mesa 26.0.8 and the guest's virglrenderer 1.1.0-2 and Mesa 25.0.7-2+deb13u1: both pictures have 18,432 pixels of the triangle, 192 by 192 halved, and 47,104 of the texture, and the checksum `f0b3cd1d6d2e2625`. Through the relay, the client sent 18 commands: the renderer, the capabilities twice, 4 resources, the texture in 2 transfers in, one submission, 6 busy waits and the picture in one transfer out, 264,849 bytes to the server and 263,924 back; the relay hid 1 version query. With the guest under TCG (`ACCEL=tcg`) the check passes with the same checksum; the client polled 9 busy waits in place of 6 while the slower guest drew.

## Design

### The relay

Mesa's vtest client, the `virpipe` driver of the software path, speaks only Unix sockets, at `VTEST_SOCKET_NAME` or `/tmp/.virgl_test`, and always asks for the newest protocol (`virgl_vtest_socket.c`, `virgl_vtest_negotiate_version`). Protocols 2 and later pass file descriptors on the socket, for shared memory and fences, which no network connection carries. Protocol 0 carries everything in the stream: a resource's contents travel in transfers in and out, and a fence is a busy wait that the server answers.

The client asks by sending `VCMD_PING_PROTOCOL_VERSION` and then a busy wait on handle 0, and reads one reply: a server that knows the query answers it first, and an older one answers only the busy wait, which the client takes for protocol 0. The relay drops the query, so the server, which accepts a busy wait on handle 0 before its context exists for exactly this probe (`vtest_renderer.c`, `vtest_resource_busy_wait`), answers as an older server would. Nothing else needs changing: under protocol 0 the client sends no command that the server answers with a descriptor.

To find the query the relay reads the client's commands: a header of two native 32-bit words, the length and the id, then the payload. The length counts words, except in two commands: `VCMD_CREATE_RENDERER` counts the bytes of the program's name after it, and `VCMD_TRANSFER_PUT` is followed by its eleven words and then by its data, whose size in bytes is the last of those words; Mesa counts the data in the header's length too, rounded up to words, but writes it unpadded, so the relay goes by the data's size. A command whose id protocol 0 has not, or any command before `VCMD_CREATE_RENDERER`, closes both connections. The server's replies pass unread: a transfer out has no header of its own, so they could not be parsed without the client's state.

The listening process forks a child for each client, which connects to the server with Nagle's delay off, since vtest is a run of small requests that each wait for a reply, and relays both directions through one `poll()` loop with a 64 KiB buffer each way. A side whose source has ended has its destination's write side shut once its buffer is empty, and the child exits when both are shut.

### The guest

The guest is the reference guest's Debian 13 cloud image of 2026-11-03, with its packages from Debian's snapshot of 2026-11-08, as the [graphics tests](../tests/graphics/Readme.md#the-reference-guest) pin them, on q35 with QEMU's standard VGA and no window, a virtio disk, a virtio network card on QEMU's user network and a virtio random number generator. It has no login and no SSH key: three services start at boot. `vtest-server` runs `virgl_test_server --use-egl-surfaceless` as the user `vtest` on `/run/vtest/vtest.sock`, forking a child for each client, with `LIBGL_ALWAYS_SOFTWARE=1`, which keeps it on llvmpipe whatever GPU the guest has. `vtest-tcp` runs socat, which forks for each TCP connection to port 5555 and connects it to that socket. `vtest-ready` waits for the socket and the port and writes the versions and `vtest-guest: ready on port 5555` to the serial console, which `run` reads. The server's and socat's output goes to the console too.

virglrenderer loads EGL and OpenGL at run time through libepoxy, and Debian's `virgl-server` depends on neither loader, so the package list names `libegl1` and `libopengl0`; without them the server's child fails with `Couldn't open libEGL.so.1` and drops the client unanswered.

## Limits

* The relay keeps to protocol 0, which copies every resource's contents through the stream and has no blob resources. Venus's vtest client passes file descriptors under every protocol, so the relay carries virgl alone; Venus's first rung is the shared-memory transport of 83-3.
* A command's words are in the machine's byte order, which the relay and both ends must share; Rach2 and the guest are both little-endian x86.
* The guest's server takes any client that reaches its port, with no authentication. QEMU forwards the port on the host's loopback address alone.

## Tests

`sh bed/fixture/vtest-guest.sh check` is the relay's check between two Linux machines, the host and the guest; it runs no suite. 83-1b stages the relay in the GNU root and adds the `bed-vtest` family, which draws the triangle from Rach2 through the same guest.
