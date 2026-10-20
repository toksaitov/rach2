# Large-memory backing

T46 gives the 72 GiB accounting boot a separate lazy memory profile. It does not change `tests/system/suites/bigmem.sh` or `tests/memprobe.c`. T69 converts their assertions, including the 8 GiB boot that writes and reads 5 GiB. T48 establishes the lazy profile on the Mac before T91 uses it for a comparison.

## Workload and bound

The boundary boot has 73728 MiB of RAM, the `max-nopdpe1gb` processor profile and `rach2.rescue`. It reaches the debug shell, runs `exec /memprobe total` once, and stops. The configured memory stays 72 GiB. The assertions stay: usable plus 513 KiB of holes is 63 GiB; the 9 GiB past physical address 64 GiB is unreachable; and the shell is reached through 2 MiB page tables.

The profile reserves 4 GiB of physical memory: 2 GiB for guest pages and 2 GiB for QEMU, firmware and the controller. It has no disk backing. These are three different quantities: configured guest RAM, the resident allowance, and disk backing. They must remain separate in admission and in the run's capacity record.

The guest's large address space does not require every page to be touched:

* `frame_map_build` in `rach2/sys/mm/frame.c` clips the usable map at the architecture's reach before `recount` sets `top_frame`. At 64 GiB the frame table has at most 16777216 entries.
* `init_paging` in `rach2/sys/mm/paging.c` writes one 32-bit entry per frame, at most 64 MiB. It counts entries and reserves frames; it does not clear all physical RAM.
* `mmu_frame_table` in `rach2/sys/arch/x86_64/mmu.c` places that table below the reach. The direct map uses 64 page directories, at most 256 KiB, rather than a leaf page table for every physical page. The heap's leaf tables and page bitmap are metadata; `kheap_init` maps data frames as objects are allocated.
* `memprobe total` in `tests/memprobe.c` reads `/proc/meminfo` into a 1024-byte buffer and exits. It does not run the mapping, pattern, fork or exec workload.
* The launcher bounds the kernel file at 64 MiB and the initial RAM image at 256 MiB. Rescue boot starts no ordinary init services. The 2 GiB guest allowance leaves room for firmware, page tables, the loaded kernel, the image and the small boot and probe allocations beyond the frame table.

The site enables `boundary_lazy` only after its platform proof. The focused acceptance test opts in to establish that proof; ordinary sites default to refusal of the reduced reservation. The host backing is anonymous RAM with preallocation, merging and memory locking disabled explicitly. On macOS the launcher requires a zero soft `RLIMIT_CORE`, which QEMU inherits. Outside macOS it also passes `dump=off`; macOS QEMU rejects that option because the host has no `MADV_DONTDUMP`. [QEMU's invocation reference](https://www.qemu.org/docs/master/system/invocation.html) documents these backend controls. Untouched guest pages need virtual addresses, not a physical allocation for all 72 GiB. File backing is not used: a sparse file alone neither reserves its future disk blocks nor bounds resident memory.

This analysis is for the fixed boot and its tested artifacts. The allowance is not a hard resident limit imposed by macOS or Linux. It does not promise safety for an arbitrary kernel or a guest that allocates all its configured RAM. The launcher restricts console commands, QMP commands, processor, boot line, devices and artifacts; the manifest checker rejects a smaller reservation and unproved lazy or file backing. An allocating guest retains its full configured reservation and overhead.

## Proof and acceptance

`etude_qemu_hosttest.c` checks the profile's command line and amounts, rejects a changed workload, and starts no boundary QEMU when admission is one byte short. Its `--memory` mode runs the profile and refusal rows without booting QEMU. `tests/proofs/etude-boundary-core.json`, run on macOS with a hard core-file limit that permits a nonzero soft limit, removes the core-file refusal and requires that assertion to fail, then rebuilds the restored source and requires a pass. Its separate `--boundary` mode, with `ARCH=x86_64`, boots the exact 72 GiB profile, runs the accounting probe, checks the original boundary values, records QEMU's resident peak against the 4 GiB reservation, and confirms cleanup. `etude_qemu_mutate.sh` removes the launcher's admission check and requires the refusal assertion to fail, then restores it and requires a pass.

The focused command, after `tests/run host` has built its binary, is:

```sh
ARCH=x86_64 build/tests/host-suite/etude_qemu_hosttest --boundary
```

The ordinary host suite retains its small memory declaration; the 72 GiB row runs separately with a 4 GiB reservation and the machine's exclusive large-memory claim during platform acceptance.

The scenario library copies the memory profile and site permission into the launcher. A case's machines may run at once, so the library refuses a boot of a case whose declared machines together reserve more than `resources.memory_bytes`, and gives the launcher that reservation less what the other machines reserve. Its host test refuses an unsafe profile, refuses both boots of a boundary machine and an ordinary one whose reservation covers the boundary machine alone, and boots the boundary machine when the reservation covers both, where QMP's queries are answered and `object-add` and the human monitor's `migrate` are refused. Its mutation proof removes the copy of the profile, then the check of the whole reservation, then the launcher's check of a boundary machine's QMP commands, and requires each refusal assertion to fail.

The examples and invalid fixtures check admission declarations as well: a missing resident declaration, a wrong processor, a short reservation, a boundary machine beside an ordinary one that the reservation does not cover, and an ordinary guest with unproved reduced backing fail validation; the same pair passes when the reservation covers both.

[The native Mac acceptance](mac.md) establishes this profile on macOS 27.0.1 with QEMU 11.1.2 under TCG: the fixed 72 GiB boot passed with a 173391872-byte QEMU peak and no observed paging or memory pressure. That record gives the immutable guest artifacts, their hashes and sizes, the backend controls, the reservation and the host readings. A failed backend starts no alternative backend. The 24 GiB Mac can admit the 4 GiB reservation only within its shared capacity; this proof enrolls no site and changes no capacity. T91 must charge the profile with the comparison run's other work.
