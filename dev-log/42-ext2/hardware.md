# Milestone 42 hardware

What this milestone measures about the machines it runs on. The Lenovo's
column is filled in milestone 43; nothing here touches it.

## What milestone 43 needs

Written in phase 6: the fourth partition's reflash with the ext2 image,
the `bootctl config` diff, the trial, the checks, the deliberate fallback
and the commit last, each with its safeguards.

## The port

| Measure | Value |
|---------|-------|
| `e2fsck` static binary, stripped | 673672 bytes |
| Recovery image | a fixed 4 MiB, with room |
| Free RAM in recovery at rest, 32 MiB managed | 16412 kB |
| `e2fsck -fn` on a 64 MiB volume, peak | 800 kB |
| The limit | 737280 bytes for the binary; 8 MiB at the peak on the 1 GiB image, measured in phase 6 |
