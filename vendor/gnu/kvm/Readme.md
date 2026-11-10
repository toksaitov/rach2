# The KVM layer

The KVM layer gives the x86-64 GNU root what Rach2's KVM is tested and measured with: Debian's QEMU, with its OpenGL modules, SeaBIOS, OVMF and iPXE's network boot ROMs, as a Debian closure of its own from the snapshot and keys of the [Debian layer](../debian/Readme.md); and kvm-unit-tests and Linux's KVM selftests, built from pinned sources with the layer's cross toolchain at the revisions with which task 80-2 measured Linux's KVM on the Lenovo. `make ARCH=x86_64 gnu-kvm` stages it, and `install_root.sh` installs it after the games layer. The i686 root has none of it.

## Use

`qemu-system-x86_64` is Debian's QEMU 10.0.13, and `kvm` and `qemu-system-amd64` are links to it; `qemu-system-i386` and `qemu-system-x86_64-microvm` are its builds for a 32-bit machine and for the microvm machine alone. [qemu-system(1)](../../man/man1/qemu-system.1), Debian's manual, describes the four. `qemu-img`, `qemu-io`, `qemu-nbd`, `qemu-storage-daemon`, `qemu-pr-helper` and `qemu-vmsr-helper` are Debian's too. The virtio-gpu devices with GL (`virtio-gpu-gl-pci`, `virtio-vga-gl`), the GL displays (`-display egl-headless`) and the D-Bus display are modules, which QEMU loads from `/usr/lib/x86_64-linux-gnu/qemu`. QEMU finds SeaBIOS and iPXE's ROMs itself, as on Debian; OVMF is `/usr/share/ovmf/OVMF.fd`, which `-bios` names.

kvm-unit-tests is under `/opt/kvm-unit-tests`, laid out as its source tree after a build, with only what its runner reads: `run_tests.sh`, `config.mak`, `scripts/`, `errata.txt`, and in `x86/` the runner `run`, the list of tests `unittests.cfg` and the 49 tests as `.flat` files. The runner works in the directory it is started in and writes its logs to `logs/` there, so it is run from the tree as root, or from a copy:

    cp -a /opt/kvm-unit-tests /tmp/kut && cd /tmp/kut
    ACCEL=kvm ./run_tests.sh -g vmexit
    ACCEL=tcg ./x86/run x86/sieve.flat -smp 1

`ACCEL` chooses QEMU's accelerator, by default `kvm` where `/dev/kvm` is a character device and `tcg` elsewhere, and `QEMU` the program, by default `qemu-system-x86_64` found on the path. `run_tests.sh -h` lists the other options. A test passes when QEMU exits with 1, the code that the test device `isa-debug-exit` gives a test's status 0.

The KVM selftests are under `/opt/kvm-selftests`, where Linux's tree has them: the tests for every architecture in the top directory and the x86 ones under `x86_64/`. `list` names the 85 tests, one a line, and `x86_64/nx_huge_pages_test.sh` is the one test that is a script. Each test is a static program run with no arguments, such as `/opt/kvm-selftests/x86_64/state_test`; it exits with 0 when it passes, 4, the kselftest code, when it skips with its reason on its last line, and any other code when it fails.

`build-info` in each directory names the pinned revision, the build's options and compiler, and the number of tests.

## Sources and license

[`qemu.list`](qemu.list) names the packages in the Debian layer's list format: `qemu-system-x86`, `qemu-system-modules-opengl`, `qemu-utils`, `seabios` and `ovmf`. `make ARCH=x86_64 gnu-kvm` runs `../debian/fetch_debs.sh kvm` on it, which checks the snapshot's signed indices and every package by its SHA-256, as for the desktop, and stages the closure in `debian/kvm` of the GNU build tree, with its closure and file lists under `work/debian/kvm`. Its versions are QEMU 1:10.0.13+ds-0+deb13u1, the one 80-2 measured with on the Lenovo's Debian, SeaBIOS 1.16.3-2, OVMF 2025.02-8+deb13u1 and iPXE 1.21.1+git20250501.dad20602+dfsg-1. QEMU is under the GNU GPL version 2, with parts under other free licenses; each package keeps its notice at `/usr/share/doc/PACKAGE/copyright`, and `../debian/copyright/` keeps those of `qemu-system-common` and `qemu-utils`, whose pages are in `man/`.

[`sources.txt`](sources.txt) pins kvm-unit-tests at commit `5a221342a025948fa7d84f0c404748f6ba57f3ad` and Linux 6.12.115, whose KVM selftests are built, in the format of `../sources.txt`, and says how each digest was checked; `build_kvm.sh` fetches each into the clone's `build/cache/gnu` with `../debian/fetch_asset.sh`, which refuses a file with another digest. kvm-unit-tests is under the GNU GPL version 2, with many files under the LGPL version 2; its `COPYRIGHT` and `LICENSE` are in `/opt/kvm-unit-tests`. The selftests are under the GNU GPL version 2 only; Linux's `COPYING` and `GPL-2.0` are in `/opt/kvm-selftests`.

The layer's downloads, which the stack's plan approved with its other downloads (its decision G), measured on 2026-11-10:

| Download | Files | Bytes |
| --- | --- | --- |
| Debian's QEMU closure, all of it | 109 packages | 92,458,512 |
| of which no other layer of the root has | 32 packages | 25,379,808 |
| `qemu-system-x86` | 1 package | 10,278,596 |
| `ovmf` | 1 package | 5,779,964 |
| kvm-unit-tests at `5a221342` | `kvm-unit-tests-5a221342a025948fa7d84f0c404748f6ba57f3ad.tar.gz` | 878,049 |
| Linux 6.12.115 | `linux-6.12.115.tar.xz` | 148,760,708 |

The packages are kept in the clone's `build/cache/debian/amd64` and the two tarballs in `build/cache/gnu`, so a rebuild downloads nothing.

## Design

`build_kvm.sh` is the step `gnu-kvm`, in four parts, each with its log in the build tree's `log/kvm-PART.log`:

* `qemu` runs the Debian fetcher on `qemu.list` as the layer `kvm`.
* `unit` configures kvm-unit-tests for x86-64 with the cross toolchain's prefix and builds it. Outside a git checkout its configure leaves `-Werror` off. Its Makefile writes `build-head`, which `run_tests.sh` prints, from `git rev-parse HEAD`, which in a build tree inside a clone would name the clone's commit; git is kept out with `GIT_CEILING_DIRECTORIES`, and the file is given the pinned commit's first eight digits, as a checkout of it would have. The staged `config.mak` names `/opt/kvm-unit-tests` as its source and errata, and prints stacks raw, since `pretty_print_stacks.py` reads the tests' ELF files with the cross toolchain's `addr2line`, neither of which the root has.
* `selftests` runs Linux's `make headers`, which installs the kernel's exported headers in the tree with the build host's compiler, then builds the KVM selftests with `ARCH=x86`, the cross toolchain's prefix and `EXTRA_CFLAGS=-static`, as 80-2 built them against Debian's glibc, and stages each program the Makefile lists for x86 with its debugging information stripped: 97 MiB in place of 424.
* `check` boots kvm-unit-tests' `sieve` under TCG, as `x86/run` boots a test, with the staged QEMU run by the staged loader on the staged libraries, and requires QEMU's exit code 1. It then runs each selftest in a root of its own with no `/dev`, in a user namespace of the build's account, and requires each to exit with 4, its skip. Both run only on an x86-64 host; in the tooling machine on a Mac, which runs x86-64 programs through QEMU's user-mode emulator, the part counts the tests and runs none.

[`install_kvm.sh`](install_kvm.sh), which `install_root.sh` sources after the games layer, installs the closure as the games layer's is installed: a package of `SKIP` is left out, and so is one the desktop closure installed at the same version, while one it installed at another version, or a file the root already has, stops the image; a package whose i386 twin of the same version the i386 layer installed, such as `libusb-1.0-0` and `libpcsclite1`, shares the twin's identical files under `/usr/share`, as dpkg lets two architectures share them. Of the 109 packages the root takes the 30 the desktop and the ports lack: QEMU's own, its firmware and the libraries only QEMU links, among them libcapstone, libfdt, libslirp, libvirglrenderer, liburing, libpmem and libibverbs. QEMU's libnl-3 and libnl-route-3 are the ports' libnl 3.12 in `/usr/lib`, since `SKIP` names Debian's: in `/usr/lib/x86_64-linux-gnu`, which the loader searches first, Debian's 3.7 would replace the library wpa_supplicant was built against, and it would crash. Since nothing resolved them against the root, it then refuses the image when a library that an ELF file of the layer needs, a QEMU module among them, is in none of `/lib`, `/usr/lib`, `/usr/lib/x86_64-linux-gnu` and `/lib64`, where glibc's loader is. Last it copies `kvm/` of the build tree, the two directories under `/opt`. The layer adds 243 MiB to the root: 132 MiB of packages and 111 MiB of tests.

The Debian fetcher takes, of a package that several suites list, the highest version, as apt does, and satisfies a virtual package with a provider already wanted. Without the first, trixie-security's QEMU 10.0.2, which the point release 10.0.13 superseded in trixie, would replace it; without the second, `qemu-system-modules-opengl`'s dependency on `qemu-system-any` would bring `qemu-system-arm`, the first provider by name.

## Updating

QEMU and its firmware follow the Debian layer's snapshot: a new snapshot gives this closure its packages too, and `qemu.list` changes only to add or drop a package. A new revision of kvm-unit-tests or a new Linux release is a new line in `sources.txt`, with its digest checked as the file's header says. 81-7 sets Rach2's results beside Linux's from 80-2, which ran the same two revisions on the Lenovo, so a new revision needs Linux's numbers taken again with it. The layer's commands are in [`commands`](commands), which `man/programs.sh` reads; a command added to the closure needs its page under `man/`, from Debian's text as the others have it.

## Limits

* The layer is the x86-64 root's alone. Rach2's KVM runs on the x86-64 kernel, and the tests are built for x86-64.
* The selftests are built with `-march=x86-64-v2`, which their Makefile adds whenever the compiler accepts it, as on Debian; they stop with an illegal instruction on a processor without SSE4.2, POPCNT and CMPXCHG16B, such as QEMU's default `qemu64`, so the machine that runs them needs a CPU model that has those.
* Every selftest reads `/sys/module/kvm/parameters/force_emulation_prefix` before its `main`, in `kvm_selftest_arch_init` (`lib/x86_64/processor.c` of the selftests), and skips with "Cannot open /sys/module/kvm/parameters/force_emulation_prefix" where the file is missing, so no selftest goes further on Rach2 until its `/sys` has that file.
* QEMU's graphical displays other than GL's are not staged: Debian's `qemu-system-gui`, with GTK and SDL, is not in the closure.
* `qemu-vmsr-helper` stops at once on Rach2, which has no RAPL driver ([qemu-vmsr-helper(8)](../../man/man8/qemu-vmsr-helper.8)).

## Tests

The step's `check` part is the layer's own test. On the testing host its first build took 122 s, of which the sieve's boot under TCG took 48 s, and all 85 selftests skipped. The tests run on Rach2 through the runner and the case lists of `tests/kvm/`.
