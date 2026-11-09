# Upstream faults of milestone 83

Faults this milestone found in another project's code, one section each, in the format of [dev-log/upstream.md](../upstream.md), which indexes them.

## Mesa's vtest client counts a put's data in words but sends it unpadded

* Project: Mesa 26.0.8, `src/gallium/winsys/virgl/vtest/virgl_vtest_socket.c`, read at the tag `mesa-26.0.8` (SHA-256 of the file 012fbc69a5078cee72a1a823930df458631239778206c791acb1e1aedecdeb3b); Rach2 keeps no copy.
* Fails: a `VCMD_TRANSFER_PUT` header's length is the command's eleven words plus the data's size rounded up to words (`virgl_vtest_send_transfer_cmd`, lines 420-426), but `virgl_vtest_send_transfer_put_data` writes the data's `data_size` bytes with no padding (lines 513-517). When the size is not a multiple of 4 the stream holds fewer bytes than the header counts, so a reader that skips a command by its length loses its place. virglrenderer's server reads the data by `data_size` and ignores the length (`vtest/vtest_renderer.c`, `vtest_transfer_put`), so the two ends agree with each other.
* Found: 2026-11-09, the testing PC, milestone 83, task 83-1a, while writing `bed/vtestrelay.c`, which reads the client's commands to find the version query.
* Reproduction: send a put of 13 bytes through a reader that trusts the length: its header says 15 words, 60 bytes after the header, and 57 follow. The relay's own test did so by hand against virglrenderer 1.2.0's server, which took the 13 bytes and gave them back.
* Evidence: `bed/vtestrelay.c`, `feed_commands`, which goes by `data_size`; `bed/Readme.md`, "The relay".
* Rach2: the relay skips a put by its data's size, not by the header's length.
* Status: unreported.
* Next: read Mesa's main branch and the vtest protocol's other clients (crosvm's and virglrenderer's own) to settle which reading is meant, then send a patch that pads the data or counts the length in bytes.

## Debian's virgl-server does not depend on an EGL loader

* Project: Debian 13's `virgl-server` 1.1.0-2 and `libvirglrenderer1` 1.1.0-2, from Debian's snapshot of 2026-11-08.
* Fails: `virgl_test_server --use-egl-surfaceless` opens EGL at run time through libepoxy, and neither package depends on `libegl1`, the loader that provides `libEGL.so.1`. With only the packages and their dependencies installed, the server's child for the first client fails with `Couldn't open libEGL.so.1: libEGL.so.1: cannot open shared object file` and drops the client unanswered. Ubuntu's 1.2.0-2ubuntu2 lists the same kind of dependencies, `libepoxy0`, `libgbm1` and `libx11-6` with no EGL loader; Debian's control file was not read.
* Found: 2026-11-09, the testing PC, milestone 83, task 83-1a, on the first check of the vtest guest that `bed/fixture/` prepares.
* Reproduction: in a Debian 13 cloud image, `apt-get install --no-install-recommends virgl-server libgl1-mesa-dri libegl-mesa0`, start the server and connect a vtest client.
* Evidence: `bed/fixture/packages`, whose comment names the two loaders; `bed/Readme.md`, "The guest".
* Rach2: the guest's package list adds `libegl1` and `libopengl0`.
* Status: unreported.
* Next: read Debian's `debian/control` for virglrenderer and check whether a recommended package pulls the loader in, since the guest installs no recommends; if none does, report it on Debian's tracker against virglrenderer.
