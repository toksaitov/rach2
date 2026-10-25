# WHPX returns a stale XSAVE area size

QEMU's WHPX CPUID exit path returned `0x240` for leaf `0xD`, subleaf 0, EBX after the guest had set XCR0 to `0xe7`. The enabled components ended at `0x980`. Rach2 allocated and reported a `0x980`-byte signal image, which its unchanged XSAVE assertion rejected against the stale CPUID size. Issue [#1161](https://github.com/toksaitov/rach2/issues/1161) tracks the local repair. No upstream report or patch has been submitted. The patched executables have compiled; their actual guest validation remains pending.

## Versions and inputs

The failing executable reported `QEMU 11.1.0 (v11.1.0-12130-ge470268ff4)`. Its exact vendor source revision has not been established. The executable was copied from the installed Windows QEMU distribution, then retained under the collection's private runtime directory. Its SHA-256 was `47d57a6072e0bb3bd98f87926eb129eb1736dfe818c67b3b81ef7ce4edd0b3cd`, size 25,457,048 bytes. The observed machine type was `pc-i440fx-11.1`.

Published QEMU tags v11.1.0, v11.1.1 and v11.1.2 contain the same omission in `target/i386/whpx/whpx-all.c`. The pinned repair source is [qemu-11.1.2.tar.xz](https://download.qemu.org/qemu-11.1.2.tar.xz), 141,815,216 bytes, SHA-256 `731b5681e4bb18be313231579b8efd0296c5b015fa36dc533874b639ba838016`. This does not establish that the failing vendor executable was built from any one of those tags.

The diagnostic used x86-64, WHPX, one processor, 128 MiB of memory, RTL8139 and CPU `max,rdrand=on,rdseed=on,xsave=on,avx=on,enforce=on`. It did not mask AVX-512. Its Rach2 artifact binding was source `0da2f8d327516deaaa26d22232c8004dfe8f76e2`:

* Kernel: SHA-256 `9090a7284ec97ba686356b84014c4a31a16df5d589ba270dfa63515d2fc5318a`, 1,767,696 bytes.
* Original test initrd: SHA-256 `1e22e1c336fab284dccd5f3b30668f94e2905be28e32d81b50115ec3781a42b7`, 13,631,488 bytes.
* Diagnostic probe: SHA-256 `6566720c0b9efa2a48c6e035019307de80b673316d3f010348aab5e0db235b16`, 26,576 bytes. Its source SHA-256 is `7ed51f76e6ccddaac930df1c8fad13818fb23c586b18009ddcda84edd0685688`; its actual compiler receipt SHA-256 is `7772dc999a87d2507d198d57666294b22e60bc637e611bbc8347e8c1228da77f`.

The probe added raw CPUID, XGETBV and signal-frame diagnostics to `tests/probes/xsaveprobe.c`, preserving its assertions. A private copy of the initrd staged this probe as `/sbin/init`, avoiding dependence on debug-shell input. Earlier serial-command attempts did not execute the probe and establish no CPU result.

## Decisive output

The successful diagnostic dispatch printed the following. Hexadecimal register values are shown exactly; zero subleaves 3 and 4 are retained.

```text
fpu: fxsr: on, sse: on, xsave: on, xcr0: 0xe7, area: 2432 bytes
@etude 1 begin guest.default.xsaveprobe
@etude 1 branch save xsave
@etude 1 branch avx avx
@etude 1 assert osxsave pass
@etude 1 assert xcr0 pass
diagnostic XCR0 low=000000e7
diagnostic XCR0 high=00000000
diagnostic probe image bytes=00000240
diagnostic leaf 0d subleaf=00000000
  eax=000000e7
  ebx=00000240
  ecx=00000980
  edx=00000000
diagnostic leaf 0d subleaf=00000001
  eax=0000000f
  ebx=00000980
  ecx=00001800
  edx=00000000
diagnostic leaf 0d subleaf=00000002
  eax=00000100
  ebx=00000240
  ecx=00000000
  edx=00000000
diagnostic leaf 0d subleaf=00000003
  eax=00000000
  ebx=00000000
  ecx=00000000
  edx=00000000
diagnostic leaf 0d subleaf=00000004
  eax=00000000
  ebx=00000000
  ecx=00000000
  edx=00000000
diagnostic leaf 0d subleaf=00000005
  eax=00000040
  ebx=00000340
  ecx=00000000
  edx=00000000
diagnostic leaf 0d subleaf=00000006
  eax=00000200
  ebx=00000380
  ecx=00000000
  edx=00000000
diagnostic leaf 0d subleaf=00000007
  eax=00000400
  ebx=00000580
  ecx=00000000
  edx=00000000
@etude 1 assert image-size pass
xsaveprobe: cpuid: OSXSAVE, XCR0 has x87, SSE, AVX and AVX-512
@etude 1 assert switches pass
diagnostic frame extended bytes=00000984
diagnostic frame image bytes=00000980
diagnostic frame XCR0 low=000000e7
diagnostic frame XCR0 high=00000000
XSAVEPROBE FAIL: the handler: the frame's extended size is not the image's and the magic's
@etude 1 assert signals fail the handler: the frame's extended size is not the image's and the magic's
@etude 1 notrun header an earlier check failed
@etude 1 notrun exec an earlier check failed
@etude 1 notrun ptrace an earlier check failed
@etude 1 notrun core an earlier check failed
@etude 1 end 11
panic: init (pid 1) exited; halting (boot rach2.rescue for the debug shell)
```

Enabled XCR0 components 2, 5, 6 and 7 have offset-plus-size ends `0x340`, `0x380`, `0x580` and `0x980`. Leaf 0 EBX should describe the currently enabled standard XSAVE area. Its observed `0x240` disagrees with XGETBV and those component bounds. The kernel's `0x980` image plus its four-byte trailing magic gives the observed `0x984` extended size. The failing assertion must remain intact.

After that panic, QMP `human-monitor-command` with `command-line: info registers` and QMP `query-status` returned these decisive fields:

```text
RIP=ffffffff8020d844 RFL=00000046 [---Z-P-] CPL=0 II=0 A20=1 SMM=0 HLT=0
{"return": {"status": "running", "running": true}, "id": 5}
qemu-system-x86_64.exe: warning: Ignoring request for interrupt vector 0
qemu-system-x86_64.exe: failed to get xsave state: No error
```

These are actual observations, not proof that the guest resumed after its halt. QEMU's HMP `HLT` field reflects its software CPU state. WHPX's in-kernel interrupt controller can handle halt without setting that field through QEMU's halt-exit handler. Explicit `kernel-irqchip=off` is a separate halt-observation setting under validation; it does not repair CPUID EBX. Neither a running QMP status nor the stderr text alone establishes another guest defect.

## Reproduction and cause

The shortest existing reproducer boots the unchanged kernel and the private initrd whose `/sbin/init` is the diagnostic probe. With the matching runtime DLLs and BIOS files, the essential command is:

```sh
qemu-system-x86_64.exe -accel whpx -nodefaults \
  -cpu max,rdrand=on,rdseed=on,xsave=on,avx=on,enforce=on \
  -smp 1 -m 134217728B -display none -no-reboot -monitor none \
  -kernel kernel -initrd diagnostic.krffs -serial stdio \
  -netdev user,id=net0 -device rtl8139,netdev=net0
```

The retained diagnostic also used `-S` and loopback QMP to issue `cont`, then collected registers and status. Those controls are not required to observe the CPUID discrepancy. A smaller upstream reproducer can query XGETBV and CPUID `0xD` under any guest which actually enables the same components, but that substitute has not been executed here.

In v11.1.0, `target/i386/cpu.c:8880` computes leaf 0 EBX with `xsave_area_size(env->xcr0, false)` outside KVM. The WHPX CPUID exit path calls the common CPU calculation without first refreshing the cached XCR0 for this exit. It already substitutes Hyper-V's `DefaultResultRbx` for subleaves 1 and 2, but omits subleaf 0. In v11.1.2 this block is at `target/i386/whpx/whpx-all.c:2621`. The live Hyper-V result is the existing mechanism for these dynamic return values; adding subleaf 0 avoids a broader register synchronization change.

The exact tracked [patch](../../tools/setup/qemu-whpx/whpx-live-xsave-size.patch) is:

```diff
-            /* CPUID[0xD,{1,2}].EBX are dynamic depending on guest features. */
+            /* Use Hyper-V's live XSAVE sizes and component offset. */
             if (vcpu->exit_ctx.CpuidAccess.Rax == 0xd) {
-                if (vcpu->exit_ctx.CpuidAccess.Rcx == 1
+                if (vcpu->exit_ctx.CpuidAccess.Rcx == 0
+                    || vcpu->exit_ctx.CpuidAccess.Rcx == 1
                     || vcpu->exit_ctx.CpuidAccess.Rcx == 2) {
                     reg_values[4].Reg64 = vcpu->exit_ctx.CpuidAccess.DefaultResultRbx;
                 }
             }
```

Patch SHA-256 is `bd8def6f2773d33eaa850ab0aa50296c62b103f464601085376b2754acef2de0`. Pristine v11.1.2 `whpx-all.c` is `0352cafd1a7294b1d0fdeb29ce12b52a86ebd11950d8ea3a5c81653f573ca0a2`; patched bytes are `a9d80bb3340083bebcc30519178c782844ab3f0254999c177177bc1eccf54c3e`. No feature mask, kernel allocator or probe assertion changes.

## Verified build and pending validation

The tracked [build recipe](../../tools/setup/qemu-whpx/Readme.md), `build.sh`, `prepare.py` and `pins.json` retain source, package and wheel identities. The actual private build used MSYS2 UCRT GCC 16.2.0, GLib 2.90.1, pixman 0.46.4, libslirp 4.9.3, zlib 1.3.2, Python 3.14.9, Meson 1.12.1 and Ninja 1.13.2. GCC executable SHA-256 was `70325d325c126905fc04c5bc57727969f4529173a139c129e3ff92b7adc2941e`. The dependency bundle was SHA-256 `f41ff53b7571985081d1e4edaa65e5b5e3e0ab19b3e1be5ede403aab8a64e9a8`, 154,953,858 bytes. The three offline Python wheels are pinned separately in `pins.json`.

Retry 05 ran the recipe with `--resume-configure`, retaining the verified extracted source and dependencies after earlier extraction, offline-Python and native-path failures. Its executed script SHA-256 was `1383e642e747344923ac1b8854ac512cd0504e098c2647c4f9bb52bda47ee764`. All five native Python tool-launch checks passed. Meson enabled WHPX and TCG, and Ninja compiled both system targets and qemu-img. The command exited 0 after 160.795 seconds; all three version commands reported `11.1.2 (rach2-whpx-d0)`.

* `qemu-system-i386.exe`: SHA-256 `e9c3ea5204f98fc92af4d97bcf61f39e6724149e333e1bf098dab38d21590d7b`, 76,268,435 bytes.
* `qemu-system-x86_64.exe`: SHA-256 `95035dfbebd79233102ef5049d075bafbc4859f905e1cdca0ed5a95cedd92b32`, 77,543,402 bytes.
* `qemu-img.exe`: SHA-256 `17358a3b76da5669e1455dbdc87fed8df1609022ea588866b6ddcc3289118f85`, 11,018,527 bytes.

The actual Windows configuration is relocatable, with executables in the prefix itself and BIOS files in `share/`. Its matching private UCRT DLLs and GLib spawn helpers must accompany the new executables. Do not mix old MinGW DLLs into this runtime. Independent cleanup confirmed all 6,375 recorded native process births and the Linux owner ended, both admission ledgers contained only their headers, and all nine native and eleven Linux endpoint baseline records were restored. No global package database or original QEMU runtime was replaced.

Raw evidence is retained under `build/tests/windows-qemu-diagnostic-0da2f8d32751-05-controls` and `build/tests/windows-qemu-build-11.1.2-whpx-05/controls/operator/terminal` in the runner worktree. The decisive data needed to reconstruct the report is embedded above because those scratch directories are not permanent storage.

Before submission, run the unchanged raw CPUID diagnostic and complete XSAVE probe with these exact executables, retaining XGETBV, all component bounds, signal frames, every assertion and owned cleanup. Check the halt assertion separately with explicit IRQ-chip configuration. Record the Windows build and processor model, Hyper-V/WHPX versions, matching runtime DLL closure and final command. Reduce the CPUID case for an upstream maintainer if needed, verify it on an unpatched pinned build, and check the current upstream branch before preparing the patch submission. No runtime pass, upstream acceptance or phase completion is claimed here.
