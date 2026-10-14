# Fuzz harnesses

This directory has the libFuzzer harnesses for the kernel's parsers of bytes that come from a disk or a wire, one file for each: the ELF loader (`elf_fuzz.c`), the network receive path (`packet_fuzz.c`), the MBR and GUID partition table readers (`mbr_fuzz.c`, `gpt_fuzz.c`), and the KRFFS, FAT, exFAT and ext2 drivers (`krffs_fuzz.c`, `fat32_fuzz.c`, `exfat_fuzz.c`, `ext2_fuzz.c`). Each harness compiles the kernel's own source on the host, so a fuzzer can run it millions of times under the sanitizers. The corpora under `corpus/NAME/` are committed, and every host run replays them, so an input that once found a fault is tested again on every change.

## Running

`build.sh replay [NAME...]` compiles each harness named, or every one, with the host compiler (`cc` unless `CC` is set), AddressSanitizer and UndefinedBehaviorSanitizer, and the `main` that `fuzz.h` supplies under `FUZZ_REPLAY`, which runs the harness once on each file named on its command line. `build.sh fuzz [NAME...]` compiles the libFuzzer program with clang. The programs are `build/tests/fuzz/NAME` and `build/tests/fuzz/NAME-fuzz`.

`tests/run host` replays every corpus through its replay program. A fuzzing run is started by hand, for example:

    sh tests/fuzz/build.sh fuzz fat32
    build/tests/fuzz/fat32-fuzz -max_total_time=300 -timeout=10 -rss_limit_mb=2048 work/ tests/fuzz/corpus/fat32

## Results

A replay program prints `fuzz: replayed N input(s)` and exits 0 when the parser survives every input, and a sanitizer's report and a nonzero exit when it does not; `tests/run host` names the harness that failed and keeps its log. An input longer than `FUZZ_INPUT_MAX`, 4 MiB, is refused with status 2 rather than replayed short, since a shortened input is a different one. A crash found by fuzzing is filed as a `bug` issue, and its minimised input joins the corpus.

## Design

Every harness defines the one function `fuzz.h` declares, `LLVMFuzzerTestOneInput`, and names the sources it links in `fuzz-deps:` comment lines, as the host tests name theirs in `host-deps:` lines. The harnesses build with `RACH2_HOSTTEST`, as the host tests do, so they compile the i686 configuration of the kernel's code whatever the host is.

* The ELF harness includes `rach2/sys/proc/elf.c` and loads the input as a program image into pages it allocates one at a time, so that a write past a page is a write past an allocation.
* The packet harness includes the packet host test and hands the input, cut to 1514 bytes, to `ethernet_rx` as one Ethernet frame, from the end of an allocation of exactly its length.
* The MBR harness includes `rach2/sys/block/mbr.c`, pads the input to a sector with zeros and puts the table through each check the kernel makes of one. The GPT harness links `rach2/sys/block/gpt.c` and takes the input as a header sector followed by the entry array, and recomputes both checksums, so that the checks behind them are reached.
* The KRFFS harness takes the input, up to 1 MiB, as a whole volume behind a memory backend, lists up to 64 entries of the root and reads each.
* The FAT, exFAT and ext2 harnesses share `fs_common.h`: the kernel calls their drivers make are stubbed as the host tests stub them, a block device answers from a volume in memory of up to 4 MiB, and one walk mounts the volume, lists up to 64 entries of the root, reads up to 4 KiB of each file among them and unmounts. The block cache is invalidated before every mount, since it outlives the call and would otherwise answer a new input with blocks the last one read. ext2 takes the input as a whole volume, and its seeds are 64 KiB volumes `mke2fs` made. FAT and exFAT take a smaller input and scatter it over a volume the fixture makers build in memory, through the builders `tests/host/mkfatfixture.c` and `tests/host/mkexfatfixture.c` expose: for FAT, a 4 MiB volume, the input being the boot sector, 4 KiB of the first table and 16 KiB from the start of the data region; for exFAT, a 1 MiB volume, the input being the 24 sectors of the two boot regions, 4 KiB of the table and 32 KiB from the start of the cluster heap, with both boot checksums recomputed after the scatter, since the driver refuses a region whose checksum is wrong. A region past the end of a short input is zeros, not the seed's bytes.

## Adding tests

A new harness is `NAME_fuzz.c` with its `fuzz-deps:` lines and a seed under `corpus/NAME/`; `build.sh` and `tests/run host` find it by its name. As every new test must, it is trusted only after it has been made to fail: a fault planted in the parser must give a sanitizer report from an input the fuzzer finds or one written by hand, and the replay of the corpus must pass again once the source is restored. The `testing` skill has the mechanics.

## Limits

The harnesses fuzz the i686 configuration only, so the x86-64 kernel's ELF64 path through the loader is not fuzzed.
