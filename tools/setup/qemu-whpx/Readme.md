# Windows QEMU corrections

Two local patches correct the Windows QEMU runtime that the testing PC runs under WHPX, the Windows Hypervisor Platform: one to QEMU 11.1.2, so that CPUID reports the live size of the XSAVE area, and one to libslirp 4.9.3, so that a guest-forward command reaches its peer. Each is built into a private runtime below the tree's `build/tests`; neither replaces a global QEMU.

## Local changes

QEMU's WHPX CPUID exit path calculated leaf 0xD subleaf 0 EBX from cached XCR0, while the virtual processor had already enabled more state. The Windows diagnostic observed XCR0 `0xe7`, enabled component end `0x980` and kernel/signal image `0x980`, but CPUID EBX `0x240`. The unchanged xsaveprobe correctly rejected that discrepancy. Issue #1161 retains the raw result.

The patch adds subleaf 0 to QEMU's existing return of Hyper-V's live `DefaultResultRbx`. It preserves the feature masks, XSAVE, AVX and AVX-512, and changes no Rach2 assertion. Halt-state observability is a separate launcher setting; this patch does not change the interrupt controller.

Libslirp 4.9.3 passes a Winsock socket to `g_spawn_async_with_fds`, whose stream arguments are C runtime descriptors. GLib fails to duplicate that value, and a guest-forward command closes before reaching its peer. `libslirp-windows-exec.patch` changes only `src/misc.c`: it creates the per-connection relay with `CreateProcessW`, supplies inherited pipes for its standard streams, and transfers bytes between those pipes and a native socket on two worker threads. Each direction propagates EOF independently; the last worker closes the socket with `closesocket`. The relay stays in QEMU's Job Object. The POSIX spawn path is unchanged.

## Building

`pins.json` identifies the official QEMU 11.1.2 source and the matching UCRT MSYS2 library/build-tool archives by URL, SHA-256 and size. `prepare.py` runs on a preparation host with Python 3, bsdtar and zstd. It verifies each archive, extracts only its private UCRT prefix and packs that prefix for Windows. It neither executes Windows programs there nor modifies a package database.

```sh
python3 tools/setup/qemu-whpx/prepare.py OUTPUT ARCHIVE_CACHE
```

Run packing under the current host's shared admission policy. A historical policy with a different ledger does not account for concurrent runs. Keep `OUTPUT` new and preserve a failed output. The archive cache may retain correctly hashed downloads. Copy the output's source archive, dependency bundle, `dependencies.json`, pins, patch, Python wheels and `build.sh` into `PRIVATE_ROOT/inputs` on Windows. The pinned setuptools, wheel and pip wheels complete QEMU's offline tooling group; QEMU installs them only into its build virtual environment. The Meson and pycotap group is supplied by the private packages and upstream wheels; docs and Rust are disabled and functional tests are not selected. The prefix and all build output stay below `PRIVATE_ROOT`, itself below the tree's `build/tests`.

Run the script through the installed Windows build authority and its owned native process Job, with 8 processors, 16 GiB of memory and 40 GiB of disk reserved. An MSYS2 shell must pass the private root as an MSYS path, not `C:\...` to tar. The existing UCRT64 GCC, C++ compiler and WHPX headers are the only reused compiler installation; every extra library and Python/Meson/Ninja program comes from the private matching prefix.

```sh
sh PRIVATE_ROOT/inputs/build.sh PRIVATE_ROOT
```

The recipe verifies inputs before extraction, relocates the private pkg-config descriptors, refuses existing build outputs and builds only the i386 and x86-64 system emulators and their matching `qemu-img` companion. The companion uses the same UCRT DLLs, so its runtime directory need not mix the old MinGW executable with new libraries. It omits `roms/`, the firmware rebuild projects, and `tests/lcitool/`, the CI environment generator: their forward or absolute symbolic links are not supported by the native Windows extraction route. Neither subtree supplies these emulator targets; the shipped `pc-bios/` firmware remains included. After the original extraction-only failure, `build.sh PRIVATE_ROOT --resume-source` reuses the validated dependencies and already extracted regular files, requires no build output yet and checks the pristine WHPX source before patching. It does not repeat relocation or extraction. The failed attempt remains recorded. If configure stopped before producing `build.ninja`, `--resume-configure` verifies the already patched source, adds the pinned Python wheels and continues in the existing private virtual environment without extraction, relocation or repatching. Keep the new control script, its pins and wheels together; original archives and failed controls remain unchanged. Before configure, one native Python subprocess preflight executes the resolved compiler, C++ compiler, pkg-config, Ninja and Python paths together. Native configure operands use cygpath-produced Windows paths; shell and source-script operands stay POSIX. Configure permits no downloads and uses the vendored Python wheels where needed. Preserve the full command output and the installed authority's cleanup receipt. A missing tool or failed build is a failure, not permission to mix the old MinGW64 DLLs with UCRT libraries.

Use the complete official `https://gitlab.com/qemu-project/libslirp/-/archive/v4.9.3/libslirp-v4.9.3.tar.gz`, SHA-256 `ee698ca4ce05217ca7d520c7f0b1b1228fd7d32922dd32d1051c347152588417`, and apply the patch with `patch -p1` in its extracted source directory. On Windows, materialize the archive's fuzz-data symbolic links as regular files with the same contents before checking the complete source inventory. They are not compilation inputs. Use the existing private QEMU dependency prefix, compiler and Python environment described above. No new package is needed.

With `SOURCE` and `BUILD` as absolute native paths, the existing virtual environment's Python runs:

```sh
python.exe -m mesonbuild.mesonmain setup BUILD SOURCE --backend=ninja \
    --buildtype=release --default-library=shared --werror -Dstatic=false \
    -Dtarget_winver=0x0601 -Dc_args=-Wno-error=maybe-uninitialized
ninja.exe -C BUILD -j2 libslirp-0.dll
```

GCC 16.2 reports possible uninitialized values in the unchanged `src/socket.c`, where the buffer preparation returns zero without initializing its output vector. The callers require a nonempty receive buffer. The build retains these warnings and treats other warnings as errors; the patched file compiles without warnings.

## Tests

The unchanged raw CPUID/xsaveprobe now passes with the patched x86-64 runtime, and explicit kernel-irqchip=off makes HMP observe the original kernel halt. The retained report separates those actual results from the diagnostic wrapper string-wait failure, remaining warnings and pending broader public-case checks. No global QEMU executable or runtime input is replaced by this recipe. Record the compiler, dependency, binary and runtime DLL identities before selecting the tools in a collection.

Select the private runtime by putting its directory first on the native Windows PATH before preparation, prerequisite checks and execution. Keep both system emulators, qemu-img, DLLs, command helpers and firmware together. `prereq.sh --platform windows` checks that directory, falling back to `Program Files/qemu` only when PATH has no x86-64 emulator. Its command-helper check accepts the matching GLib 2.90.1 UCRT package from `pins.json` as well as the original MinGW package. A missing or mismatched UCRT helper requires restoring the pinned runtime; `Install-QemuSpawn` installs only the original MinGW helpers and rejects another GLib.

Compare the old and new DLL exports and imports before use. Copy the new DLL into a fresh private copy of the runtime, with every other file byte-identical. Do not install it globally or relink QEMU. Under the installed resource admission and native process owner, run `etude_scenario_hosttest --real-forward-only` from a prepared tree with that runtime on `PATH`. This selection runs the existing real-forward row and its complete judgement: TCP and UDP forwarding, guest echo, relay EOF, restricted networking, unchanged disk and process cleanup. It needs the prepared kernel, test image, scenario disk and hello program; it starts no other fixture row. A failed exchange or unconfirmed cleanup prevents acceptance.
