# Windows QEMU XSAVE size correction

QEMU's WHPX CPUID exit path calculated leaf 0xD subleaf 0 EBX from cached XCR0, while the virtual processor had already enabled more state. The Windows diagnostic observed XCR0 `0xe7`, enabled component end `0x980` and kernel/signal image `0x980`, but CPUID EBX `0x240`. The unchanged xsaveprobe correctly rejected that discrepancy. Issue #1161 retains the raw result.

The patch adds subleaf 0 to QEMU's existing return of Hyper-V's live `DefaultResultRbx`. It preserves the feature masks, XSAVE, AVX and AVX-512, and changes no Rach2 assertion. Halt-state observability is a separate launcher setting; this patch does not change the interrupt controller.

`pins.json` identifies the official QEMU 11.1.2 source and the matching UCRT MSYS2 library/build-tool archives by URL, SHA-256 and size. `prepare.py` runs on a preparation host with Python 3, bsdtar and zstd. It verifies each archive, extracts only its private UCRT prefix and packs that prefix for Windows. It neither executes Windows programs there nor modifies a package database.

```sh
python3 tools/setup/qemu-whpx/prepare.py OUTPUT ARCHIVE_CACHE
```

Run packing under the current host's shared admission policy. A historical policy with a different ledger does not account for concurrent runs. Keep `OUTPUT` new and preserve a failed output. The archive cache may retain correctly hashed downloads. Copy the output's source archive, dependency bundle, `dependencies.json`, pins, patch and `build.sh` into `PRIVATE_ROOT/inputs` on Windows. The prefix and all build output stay below `PRIVATE_ROOT`, itself below the tree's `build/tests`.

Run the script through the installed Windows build authority and its owned native process Job, with 8 processors, 16 GiB of memory and 40 GiB of disk reserved. An MSYS2 shell must pass the private root as an MSYS path, not `C:\...` to tar. The existing UCRT64 GCC, C++ compiler and WHPX headers are the only reused compiler installation; every extra library and Python/Meson/Ninja program comes from the private matching prefix.

```sh
sh PRIVATE_ROOT/inputs/build.sh PRIVATE_ROOT
```

The recipe verifies inputs before extraction, relocates the private pkg-config descriptors, refuses existing build outputs and builds only the i386 and x86-64 system emulators. It omits `roms/`, the firmware rebuild projects, and `tests/lcitool/`, the CI environment generator: their forward or absolute symbolic links are not supported by the native Windows extraction route. Neither subtree supplies these emulator targets; the shipped `pc-bios/` firmware remains included. After the original extraction-only failure, `build.sh PRIVATE_ROOT --resume-source` reuses the validated dependencies and already extracted regular files, requires no build output yet and checks the pristine WHPX source before patching. It does not repeat relocation or extraction. The failed attempt remains recorded. Configure permits no downloads and uses the vendored Python wheels where needed. Preserve the full command output and the installed authority's cleanup receipt. A missing tool or failed build is a failure, not permission to mix the old MinGW64 DLLs with UCRT libraries.

The produced programs are separate diagnostic tools until the unchanged raw CPUID/xsaveprobe and affected halt checks pass with their exact paths and hashes. No global QEMU executable or runtime input is replaced by this recipe. Record the new compiler, dependencies, binary and runtime DLL identities before selecting them in a collection.
