# The deployment inventory

What phase 13's sitting writes to the notebook and to the stick, what each
artifact is, where it goes, and the boot that proves it. Every hash is the
testing host's, where the accepted set ran; a Mac rebuild of the same source
differs, so what goes on the machine is copied from the host and not rebuilt
here.

## The artifacts

| Artifact | SHA-256 | Bytes |
| -------- | ------- | ----- |
| `build/rach2/kernel` | `7d592148fc816150ba3d8733c8fb0cb73524d3fd73e486b6ccd8652c175abb20` | |
| `build/rach2-recovery/kernel` | `45779227c8654ed9178a1bfc7ecf5a6271cc70901a44e528abc6a6b1eccd9033` | |
| `build/images/rach2-recovery-with-key.krffs` | `ce19bdce2120407a4c596c3981c57530f3e3409fd653a02c2492c52b4270a3b8` | |
| `build/images/rach2-metal-disk.ext2` | `fa55ea43e6384815497729d18f419edacb3d4e6a8a46c95c384622c38f88e57a` | |
| `build/images/rach2-sealed.disk` | `db98585ffca4587415611e8d459083f0eebf048ad543ace07607470b9d4e0ad1` | 1211105280 |
| `build/images/rach2-install-stick-with-key.img` | `c46f9232151f956dccef318bffb192c6a1bf7c6db802dea4b8d138d870c16707` | 1222639616 |

## The stick

The install stick is written to the development flash drive, a SanDisk
3.2Gen1 of 61,524,148,224 bytes, USB serial `00014101091624031318`. It is the
recovery ISO at sector 1 for 13291 sectors, then an exFAT volume at sector
14336 for 2373632 sectors holding `rach2-sealed.disk` and the manifest that
declares its hash.

Written and read back on 2026-09-20. The ISO half and the volume's first
sectors are byte-identical to the image; `rach2-sealed.disk` on the volume
hashes to `db98585f`, the accepted image, and the manifest on the stick
declares that same hash. The 16512 bytes that differ from the image are
`.Spotlight-V100` and `.fseventsd`, which macOS wrote when it mounted the
volume after the write. They are outside the ISO and outside the staged file.

The boot that proves it: the stick boots the notebook into the recovery
system, which is what every later write goes through.

## The notebook's destinations

The disk is a Samsung SSD 870 EVO of 488397168 sectors on AHCI port 0, in
three partitions.

| Destination | Start | Length | What goes there |
| ----------- | ----- | ------ | --------------- |
| Partition 1, boot | sector 2048 | 131072 sectors, 64 MiB | the candidate kernel at sector 312 of the partition, the fallback at 4408, GRUB and the menu |
| Partition 2, recovery | sector 133120 | 131072 sectors, 64 MiB | the recovery kernel and its RAM root |
| Partition 3, root | sector 266240 | 487106928 sectors | the ext2 root, grown into the disk |

The order of the writes is milestone 49's: the recovery partition from the
booted stick, then the root through `rach2-install --update`, which keeps the
machine's accounts, then the kernels, the candidate through `bootctl kernel`
and the fallback through `bootctl fallback`, since installing a candidate
moves the saved default to the fallback.

Before each write the sectors it overwrites are saved, on this host and on the
stick, and recovery is the saved boot default until the write is verified.

## The boots that prove it

1. The stick boots into recovery and `reflash` runs.
2. The updated recovery partition boots from the disk's recovery entry.
3. The updated root boots and the installed credentials log in.
4. The candidate kernel boots, and the fallback boots when it is selected.
