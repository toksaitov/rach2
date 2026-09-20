# Fuzz harnesses

This directory owns the libFuzzer harnesses for the parsers that take bytes from a disk or a wire, one file per parser: the ELF loader (`elf_fuzz.c`), the packet receive path (`packet_fuzz.c`), MBR and GPT partition tables (`mbr_fuzz.c`, `gpt_fuzz.c`), and the KRFFS, FAT, exFAT and ext2 drivers (`krffs_fuzz.c`, `fat32_fuzz.c`, `exfat_fuzz.c`, `ext2_fuzz.c`). The corpora under `corpus/NAME/` are committed; every crash a harness finds becomes an entry in the shortcomings list, and its minimised input joins the corpus as a regression.

## Interface

Each harness defines the one entry `fuzz.h` declares, `LLVMFuzzerTestOneInput`, and names the kernel sources it links in `fuzz-deps:` comment lines, as the host tests name theirs in `host-deps:`. `build.sh replay [NAME...]` compiles each harness with the host compiler, AddressSanitizer and UndefinedBehaviorSanitizer and the main `fuzz.h` supplies under `FUZZ_REPLAY`, which replays the files on its command line; `build.sh fuzz [NAME...]` compiles the libFuzzer program with clang. The programs are written to `build/tests/fuzz/`. The host tier (`tests/run host`) replays every corpus through its replay program; a long libFuzzer run is started by hand, for example:

    sh tests/fuzz/build.sh fuzz fat32
    build/tests/fuzz/fat32-fuzz -max_total_time=300 -timeout=10 -rss_limit_mb=2048 work/ tests/fuzz/corpus/fat32

## How the harnesses reach their parsers

The three block file systems share `fs_common.h`: the kernel calls the driver makes are stubbed as the host tests stub them, a block device answers from a volume in memory, and one walk mounts, lists the root, reads every file and unmounts. The block cache is invalidated before every mount, because it outlives the call and would otherwise answer a new input with the blocks the last mount read. ext2 and KRFFS take a whole 64 KiB volume as the input. FAT and exFAT take a focused input and scatter it over a volume the fixture makers build in memory through the builders `mkfatfixture.c` and `mkexfatfixture.c` expose: FAT's is the boot sector, 4 KiB of the first table and 16 KiB from the start of the data region; exFAT's is the twenty-four boot sectors, 4 KiB of the table and 32 KiB from the start of the cluster heap, with both boot checksums recomputed after the scatter. Where an input ends early the rest of its regions are zero, not the seed's bytes, so that libFuzzer cannot shorten an input to its first bytes and call the coverage the same. The ELF harness loads the input as a program image into pages it allocates one by one; the packet harness delivers the input as one Ethernet frame through the packet host test's interface.

## What proves it

A harness is proved by two things: libFuzzer's coverage line growing from its seed, and a fault planted in the parser found from the corpus as a sanitizer report. The records of both are in the milestone notes that added each harness.
