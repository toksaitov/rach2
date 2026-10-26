# Windows command forwarding

The libslirp repair for #1147 passes the existing real-forward fixture on native Windows 11, with the prepared i686 kernel and image and QEMU 11.1.2 under TCG. All nine assertions pass: service-ready, booted, shell, tcp-forward, udp-forward, guest-forward, relay-closed, restricted and service-stopped. This is one affected row, not a complete Windows collection or a WHPX result.

The original library passes a Winsock socket as a C runtime descriptor to GLib's spawn function. The retained failure reports a failed descriptor duplication. The first native-launch repair removed that error, but inherited socket standard streams stalled after the relay connected to the peer. That failed run and a traced diagnostic remain retained. The accepted repair gives the child ordinary pipes and uses two threads inside QEMU to transfer bytes to and from its native socket. Each direction preserves its half-close. Only upstream `src/misc.c` changes among 145 source files; the public headers and POSIX launch path remain unchanged. [The recipe](../../tools/setup/qemu-whpx/Readme.md#local-changes) explains the build.

## Artifacts

The owner approved rebuilding only libslirp. The existing compiler, GLib, Python, Meson and Ninja were reused. No QEMU program was relinked, no dependency was installed, and no endpoint policy changed. A fresh private runtime differs from the original in `libslirp-0.dll` alone. Its 27 exports keep their names and ordinals; it adds no DLL dependency. Two UCRT import descriptors are no longer needed.

| Artifact | SHA-256 |
| --- | --- |
| Official libslirp 4.9.3 archive | `ee698ca4ce05217ca7d520c7f0b1b1228fd7d32922dd32d1051c347152588417` |
| Accepted one-file patch | `2986d1e89564178c61d0a1032bcbe816a223a7d45d1f1c7623b19e93f5e7efaf` |
| Patched DLL | `3ca3bcd3f581c5e41d3adab46102dad539d92b2c6aaf96afae08395c180b9f97` |
| Unchanged i386 QEMU | `e9c3ea5204f98fc92af4d97bcf61f39e6724149e333e1bf098dab38d21590d7b` |
| Unchanged x86-64 QEMU | `95035dfbebd79233102ef5049d075bafbc4859f905e1cdca0ed5a95cedd92b32` |
| Prepared i686 kernel | `f6efcb6b81206ccf84299f877235b0d97830ae79d0e6116210b824d60f597b16` |
| Prepared test image | `7478994f99c665ba32201e1ce318d26cac78426e6ec9be63dcfd2dd7b4b71280` |
| GCC 16.2.0 | `70325d325c126905fc04c5bc57727969f4529173a139c129e3ff92b7adc2941e` |
| Native scenario fixture | `c88e2c5958674078fdde15fc8f9bd6f91622c26f46d5b9cbf1cd65f3ed19a22e` |

## Check and cleanup

The new `--real-forward-only` entry runs the original row and its judgement without starting unrelated rows. The native fixture compiled with `-Wall -Wextra -Werror` through `mk/host-check.mk`, from the source snapshot of aaf832390. The final branch's later rebase changes no fixture dependency. The accepted run used the committed fixture, without the separate diagnostic trace. The library build uses GCC 16.2.0, Python 3.14.9, Meson 1.12.1, Ninja 1.13.2 and the existing GLib 2.90.1 prefix. The only nonfatal compiler diagnostic class is the upstream socket-buffer warning described by the recipe.

The installed WSL launcher admitted the native build and private Windows fixture, each through the existing native Job owner. The Windows enrolled endpoint stayed disabled. The accepted command was `etude_scenario_hosttest.exe --real-forward-only`, from the private prepared tree with `ARCH=i686` and the patched runtime on `PATH`. Its admission says `exited 0`. The Job receipt says:

```text
status 0
active 0
cleanup confirmed
```

A separate native identity check confirms that every recorded process birth from all 15 build and diagnostic owners has ended. The shared ledger contains no claim of this session. The successful fixture removed its scratch; the two failed fixture directories remain as evidence. The source inventories and every private runtime file were rehashed after execution.

The records are under `D:/rach2-services/build/tests/qemu-build-11.1.2-whpx-01/libslirp-1147` on the testing PC and `build/tests/1147-libslirp` in the owner's Codex worker clone. The selected runtime is `runtime-pipes`. The final receipt `final-readback.json` has SHA-256 `2ac0ec77de72f9fc4325a40af4fe77bf640e510e99f4368b4b2a7967110aa3ff`; `pipes-row.owner` has `123c1c6eb5e0796b7745f0b14ca6f0a62ec9b3182b93f7282ce3bddd235e794e`; the complete local `pipes-row-launch.log` has `3e0150eaa1371c83755cfbcf5d8a4ae0fcd9aa60fc2b2d5bddf022d4d7637744`. Failed and successful events remain distinct.

The required style invocation stopped before its cases because the unchanged Etude command failed to compile on Linux, as #1202 records. The output was added to that issue. The separate Windows command build failures found while preparing this check are #1219; they do not affect the directly compiled scenario fixture. No broader host, GNU service or complete collection acceptance is claimed here.
