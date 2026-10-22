# Fuzz harnesses

This directory has the libFuzzer harnesses for the kernel's parsers of bytes that come from a disk or a wire, one file for each: the ELF loader of the i686 kernel and of the x86-64 kernel (`elf_fuzz.c`, `elf64_fuzz.c`), the network receive path (`packet_fuzz.c`), the MBR and GUID partition table readers (`mbr_fuzz.c`, `gpt_fuzz.c`), and the KRFFS, FAT, exFAT and ext2 drivers (`krffs_fuzz.c`, `fat32_fuzz.c`, `exfat_fuzz.c`, `ext2_fuzz.c`). Each harness compiles the kernel's own source on the host, so a fuzzer can run it millions of times under the sanitizers. The corpora under `corpus/NAME/` are committed, and every host run replays them, so an input that once found a fault is tested again on every change.

## Running

`build.sh replay [NAME...]` compiles each harness named, or every one, with the host compiler (`cc` unless `CC` is set), AddressSanitizer and UndefinedBehaviorSanitizer, and the `main` that `fuzz.h` supplies under `FUZZ_REPLAY`, which runs the harness once on each file named on its command line. `build.sh fuzz [NAME...]` compiles the libFuzzer program with clang. The programs are `build/tests/fuzz/NAME` and `build/tests/fuzz/NAME-fuzz`.

`tests/run host` replays every corpus through its replay program. A fuzzing run is started by hand, for example:

    sh tests/fuzz/build.sh fuzz fat32
    build/tests/fuzz/fat32-fuzz -max_total_time=300 -timeout=10 -rss_limit_mb=2048 work/ tests/fuzz/corpus/fat32

## Results

A replay program prints `fuzz: replayed N input(s)` and exits 0 when the parser survives every input, and a sanitizer's report and a nonzero exit when it does not. The ELF64 harness also aborts, after `elf64-fuzz: loader fault: RULE`, when the loader accepts an image that breaks one of its rules; `tests/run host` names the harness that failed and keeps its log. An input longer than `FUZZ_INPUT_MAX`, 4 MiB, is refused with status 2 rather than replayed short, since a shortened input is a different one. A crash found by fuzzing is filed as a `bug` issue, and its minimised input joins the corpus.

## Design

Every harness defines the one function `fuzz.h` declares, `LLVMFuzzerTestOneInput`, and names the sources it links in `fuzz-deps:` comment lines and any more compiler flags in `fuzz-cflags:` lines, as the host tests name theirs in `host-deps:` and `host-cflags:` lines. The harnesses build with `RACH2_HOSTTEST`, as the host tests do, so they compile the i686 configuration of the kernel's code whatever the host is; one that adds `-DRACH2_HOST_X86_64` on a `fuzz-cflags:` line compiles the x86-64 configuration, as `elf64_fuzz.c` does.

* The ELF harness includes `rach2/sys/proc/elf.c` and loads the input as a program image into pages it allocates one at a time, so that a write past a page is a write past an allocation.
* The ELF64 harness includes the same file in the x86-64 configuration, so the loader reads ELF64 headers and checks segments against the x86-64 user window, and puts the input through each path a program takes: `elf_check` over the bytes in memory, a copy at the image's own addresses, as the boot loads, and `elf_read_layout` and a load mapped from a file at the bias execve gives a program and at the one it gives an interpreter. Its model page directory is a list of the pages it has mapped. A load that fails has rejected the image; one that succeeds is checked against the headers the harness reads from the input itself: every page mapped once and inside the window from `USER_ADDR_MIN` to `USER_STACK_BASE`, every segment record matching a PT_LOAD whose file bytes the file has, in ascending order, its bytes and zeros after them in the pages filled, and the entry point and the program header table inside the window. The seeds are two x86-64 test programs, `hello` and the musl `muslhello`, and a position-independent image with an interpreter, `pie`; four more each break one rule the loader enforces and are otherwise valid: a segment past the top of the window (`pasttop`), two segments in one page (`overlap`), an entry point at the window's top (`entrytop`) and a file offset of 2^32 or more (`offset4g`).
* The packet harness includes the packet host test and hands the input, cut to 1514 bytes, to `ethernet_rx` as one Ethernet frame, from the end of an allocation of exactly its length.
* The MBR harness includes `rach2/sys/block/mbr.c`, pads the input to a sector with zeros and puts the table through each check the kernel makes of one. The GPT harness links `rach2/sys/block/gpt.c` and takes the input as a header sector followed by the entry array, and recomputes both checksums, so that the checks behind them are reached.
* The KRFFS harness takes the input, up to 1 MiB, as a whole volume behind a memory backend, lists up to 64 entries of the root and reads each.
* The FAT, exFAT and ext2 harnesses share `fs_common.h`: the kernel calls their drivers make are stubbed as the host tests stub them, a block device answers from a volume in memory of up to 4 MiB, and one walk mounts the volume, lists up to 64 entries of the root, reads up to 4 KiB of each file among them and unmounts. The block cache is invalidated before every mount, since it outlives the call and would otherwise answer a new input with blocks the last one read. ext2 takes the input as a whole volume, and its seeds are 64 KiB volumes `mke2fs` made. FAT and exFAT take a smaller input and scatter it over a volume the fixture makers build in memory, through the builders `tests/host/mkfatfixture.c` and `tests/host/mkexfatfixture.c` expose: for FAT, a 4 MiB volume, the input being the boot sector, 4 KiB of the first table and 16 KiB from the start of the data region; for exFAT, a 1 MiB volume, the input being the 24 sectors of the two boot regions, 4 KiB of the table and 32 KiB from the start of the cluster heap, with both boot checksums recomputed after the scatter, since the driver refuses a region whose checksum is wrong. A region past the end of a short input is zeros, not the seed's bytes.

## Adding tests

A new harness is `NAME_fuzz.c` with its `fuzz-deps:` lines and a seed under `corpus/NAME/`; `build.sh` and `tests/run host` find it by its name.

## Limits

Only the ELF loader is fuzzed in both configurations; the other harnesses compile the i686 configuration alone.
