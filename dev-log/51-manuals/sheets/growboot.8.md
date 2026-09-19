# growboot(8)

NAME: growboot - prepare a recovery environment from a boot-partition backup. Source: utils/growboot.c header, utils/Readme.md.
* `growboot BOOT.IMG ENV.IMG`: BOOT.IMG the complete 64 MiB boot-partition backup, read through the boot-slot library; the 1024-byte GRUB environment must occupy one cluster; the editor sets recovery and removes the trial in a two-sector memory copy, so no write reaches BOOT.IMG; ENV.IMG is created exclusively (an existing path, the input included, is refused) and standard output reports its partition-relative sector. Recovery's 4 MiB RAM file system cannot hold the whole boot image, which is why this works on the environment alone.
* EXAMPLE: `growboot boot.img env.img`; `tests/run grow` runs it.
* SEE ALSO: bootctl(8), growroot(8), recovery(7).
