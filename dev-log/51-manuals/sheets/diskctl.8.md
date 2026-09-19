# diskctl(8)

NAME: diskctl - reread a disk's table, or print its identity. Source: utils/diskctl.c header.
* `diskctl reread DEVICE`: `BLKRRPART` on the whole-disk node, so the partitions a program has just written get their nodes; refused (EBUSY) while anything is mounted from the disk or another descriptor is open on it, which is what keeps a reread from moving the windows under a file system. `diskctl id DEVICE`: the disk's MBR signature, the four bytes at offset 440 of its first sector, as eight lowercase hexadecimal digits, which is how growroot(8) and the boot line name a disk. `usage: diskctl reread DEVICE | diskctl id DEVICE` otherwise; errors are the kernel's, printed with the device.
* EXAMPLE: `diskctl id /dev/disk0`; the installer runs both.
* SEE ALSO: growroot(8), disk(7), ioctl(2).
