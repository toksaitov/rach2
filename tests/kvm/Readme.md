# The KVM suite

The KVM suite runs Rach2's KVM the way a person uses it: Debian's QEMU on the x86-64 GNU root boots guests with `-accel kvm`, and kvm-unit-tests and Linux's KVM selftests, which the [KVM layer](../../vendor/gnu/kvm/Readme.md) stages under `/opt`, run under it. This directory has the runner that boots a guest to its token, the runner of the two test corpora with their lists, and the recipe of the Debian guest. `tests/run kvm` runs them in QEMU as the cases of [`machine.kvm.json`](../cases/machine.kvm.json), on the processor profiles `max-svm` and `host-svm` that give Rach2 AMD's virtualization; on the Lenovo, a VMX machine, the same runners run by hand on its installed root.

## Running

`tests/run kvm` runs these cases, each on an x86-64 Rach2 that boots `gnu-test.disk` with this directory's files on a data disk, `kvm.ext2`:

* `entry`: the small real-mode guest in `svm-entry.c` reads its memory slot, executes CPUID, reads CR0 and EFER, exits through port I/O and HLT, and reads a replacement frame after MAP_FIXED. It starts with XCR0 at its reset value, which also checks that the backend restores the full XSAVE image before applying the guest's XCR0.
* `slots`: `svm-entry slots` repeats the read after fork, mprotect, discard and munmap with a replacement page. A guest then counts in another page while the caller forks, protects and replaces the data page; its first counter increment proves it was running before the changes, and it exits only after reading the replacement value. The same case reads VGA memory through a `/dev/mem` slot and the end of a 4 MiB GEM object through another slot, on std-vga.
* `leak`: `leak.sh` kills QEMU after the Debian guest prints its kernel version. The five live KVM counts and `fs.file-nr` must return to their initial values, QEMU must have no pinned pages before the kill, and its address space must disappear after it is reaped.
* `boot-debian-kvm` and `boot-rach2-kvm`: Debian and the small i686 Rach2 guest boot under Rach2's SVM on `max-svm`, on Linux and macOS. The `-nested` variants run on `host-svm`, on Linux alone.
* `boot-debian-tcg`: `kvm-boot.sh -a tcg` boots the Debian guest in Rach2's QEMU under TCG to its token.
* `unit-tests` and `selftests`: `corpus.sh` runs the corpus under Rach2's KVM, on the `max-svm` profile, on Linux and on macOS.
* `unit-tests-nested` and `selftests-nested`: the same on the `host-svm` profile, on Linux alone.

The accelerator outside is the run's, as for every case: on the testing PC KVM, so that Rach2 runs its guests under nested KVM, and on the Mac TCG, which emulates SVM; `RACH2_QEMU_ACCEL=tcg` runs the `max-svm` cases under TCG on the testing PC too. `host-svm` starts only under KVM.

On a Rach2 with the GNU root, the runners run alone, as root:

    sh kvm-boot.sh -k guests/debian/vmlinuz -i guests/debian/initrd
    sh kvm-boot.sh -a tcg -d /tmp/i686.img -t 'RACH2 OK'
    sh corpus.sh unit unit-tests.list
    sh corpus.sh selftests selftests.list

[`kvm-boot.sh`](kvm-boot.sh) boots a kernel and initramfs, or a raw disk under SeaBIOS, with `-accel kvm,kernel-irqchip=split`, since Rach2's KVM has the split interrupt controller alone, or `-accel tcg`, and the console on a serial port it reads once a second. It passes on the token, `KVMGUEST OK` by default, and fails when QEMU ends without it or the console stays silent for 600 s, a bound on silence that `-w` changes, not on the boot's length. Its header lists the options. [`corpus.sh`](corpus.sh) runs each test that a list runs, kvm-unit-tests' through their own `run_tests.sh` with no timeout, and judges each result against the list; its last line is `kvm-corpus: result: pass` or `fail` with the names of the tests that gave another result.

## Results

The processor profiles were qualified on the testing PC, Linux 6.18.58 (WSL's) with QEMU 10.2.1, by kvm-unit-tests' own SVM tests, `svm.flat` and `svm_npt.flat` at the KVM layer's revision, each test run alone by the outer QEMU, under TCG on `max-svm` and under KVM on `host-svm`. Their statuses are in [`ref/`](ref): 0 a pass, 77 a skip, 3 a failure, 127 a crash and 2 QEMU stopped without the test's exit. [`qualify.sh`](qualify.sh) runs the qualification again on any host, `sh tests/kvm/qualify.sh tcg max-svm`, the tests one at a time as [`svm-tests.list`](svm-tests.list) names them, and writes the same statuses.

| Tests | `max-svm` under TCG | `host-svm` under KVM |
| --- | --- | --- |
| `svm.flat`, 63 tests | 39 pass, 13 skip, 7 fail, 4 crash | 41 pass, 10 skip, 9 fail, 3 crash |
| `svm_npt.flat`, 11 tests | 7 pass, 3 skip, 1 fails | 8 pass, 3 skip |

Under TCG, VMRUN and VMEXIT, the intercepts of I/O, MSRs, selective CR0 writes, interrupts and NMIs, event injection with its pending events, the VMCB's guest state checks, nested paging with its NX, present, user and write faults and its MMIO, and VGIF pass. Four results are QEMU's TCG, which nested KVM passes, and are recorded among the faults of other projects that [dev-log/upstream.md](../../dev-log/upstream.md) indexes: the CR3 read intercept and LMSW's selective CR0 intercept give no exit, an exception injected after a VMRUN that failed on an invalid injection ends in #UD in the host, and nested paging raises no #NPF for the reserved bits of its entries. TCG gives no NRIP save, TSC scaling or pause filter, whose tests skip. Rach2's SVM backend is therefore proven under TCG on the paths that pass there and under nested KVM on the rest. On the Mac, with Homebrew's QEMU 11.1.3 on the M4, every status is the testing PC's ([`ref/svm-max-svm-tcg-mac.status`](ref/svm-max-svm-tcg-mac.status)) but `reg_corruption`'s: it passes in 1 s in about four runs of five and in the fifth never ends, its timer interrupt left pending while the nested guest runs, a fifth fault of QEMU's TCG that the testing PC's QEMU shows as interrupts taken at about 18 a second, so that the test takes some 9 minutes there. Under nested KVM, the LBR virtualization tests fail as under TCG, and `svm_pae_test`, `svm_pse_test`, `svm_guest_state_test`, `svm_tsc_scale_test` and `pause_filter_test` fail where TCG passes or skips them; `virq_inject`, `svm_exception_test` and `svm_unsupported_instruction_intercept_test` crash under both. These are tests of a hypervisor's nested SVM, which Rach2's KVM does not give its guests, so neither corpus runs them.

The Debian guest reports the profile's features: under `host-svm` and KVM, `svm npt nrip_save tsc_scale vmcb_clean flushbyasid pausefilter pfthreshold`, with none of the three the profile declares absent; under `max-svm` and TCG, `svm npt vgif`. It boots to its token in 1 s under KVM and 3 s under TCG on the testing PC, and in 9 s under TCG in the GNU root's QEMU on an x86-64 Rach2 that runs under KVM there, as `boot-debian-tcg` boots it.

## Design

The lists, [`unit-tests.list`](unit-tests.list) and [`selftests.list`](selftests.list), name every test of the two corpora, one a line: its name, what Rach2's KVM must give (`pass`; `skip`, the test's own skip for what the processor or QEMU lacks; `fail`, where Rach2 is to fail as Linux does; or `excluded`, not run), Linux's result on the testing PC, and the reason. Linux's results were taken by the same tests under WSL's Linux 6.18.58 with its KVM on the Ryzen 9 7950X3D, kvm-unit-tests with `kernel-irqchip=split`, and are kept in [`ref/`](ref). A test Linux passes must pass; a test Linux skips for a lack of the processor or of the testing PC's kernel, such as SEV, AMX, PKS, CET, TSX, Xen's hypercalls or the forced emulation prefix, must skip; a test Linux fails there must still pass, since its failure is the outer layers' or a selftest older than the kernel, and the row names Linux's failure, except `hyperv_connections`, which fails one of its seven tests under Linux and is expected to fail as it does there. The excluded tests are nested virtualization (every SVM, VMX and EPT test, and the selftest `hyperv_svm_test`), which the owner excluded on 2026-11-10; the PMU, which Rach2's KVM does not yet give; and kvm-unit-tests' three tests built only for i386.

| List | Pass | Skip | Fail | Excluded |
| --- | --- | --- | --- | --- |
| `unit-tests.list` | 50 | 7 | 1 | 29 |
| `selftests.list` | 51 | 15 | 0 | 19 |

The Debian guest, which [`guests/mkdebian.sh`](guests/mkdebian.sh) makes, is trixie's cloud kernel, 6.12.107, and an initramfs of Debian's static BusyBox 1.37.0 whose [`/init`](guests/debian-init) prints the kernel's version, the processor count and the first processor's flags, then `KVMGUEST OK`, and powers off. Both packages are pinned in [`guests/debian.txt`](guests/debian.txt) by the SHA-256 that the Debian layer's signed snapshot index gives. The guest boots in seconds even under TCG inside Rach2 and needs no disk, so a failure to reach its token is the hypervisor's. Make writes it to `build/kvm/guests/debian`, and puts it with the runners and the lists into `build/tests/cases/kvm/kvm.ext2`, which every case mounts read-only and copies to `/tmp/kvm`.

The i686 Rach2 guest is the ordinary kernel with a 1 MiB KRFFS root. Its shipped `init-shell` starts the first user program, `guests/rach2-init.c`, as `/bin/sh`; that program prints `KVMRACH2 OK` and powers off. It needs no disk, network or input from the controller.

## Adding tests

A guest of its own goes under `guests/` with its recipe, the token its console prints and a case in `machine.kvm.json` whose last part of the identity [`kvm_scenario.c`](../cases/machine/kvm_scenario.c) reads; its files join `KVM_FILES` or the data disk's rule in `tests/system/suites.mk`. A new revision of a corpus changes its list: each new test gets its row from Linux's result on the testing PC.

## Limits

* The SVM backend has passed `svm-entry.c` under TCG on the Mac. Its larger guests and the corpus still await qualification.
* No selftest goes past its start until Rach2's `/sys` has `/sys/module/kvm/parameters/force_emulation_prefix`, as the KVM layer's Readme says.
* The `max-svm` profile under TCG lacks NRIP save, TSC scaling and the pause filter, and has the five faults of Results, the last of which can leave a guest of Rach2's KVM that spins with its IF clear waiting on an interrupt QEMU does not deliver; the `host-svm` profile runs only on the testing PC.

## Tests

The suite is its own test. `kvm-boot.sh` was tried on the testing PC with the Debian guest under KVM and TCG, with a token the guest never prints, with an option QEMU refuses and with a guest that never starts, and `corpus.sh` with short lists against the outer QEMU, where it reported a listed pass that failed and left no scratch.
