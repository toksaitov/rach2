# rach2-recover(8)

NAME: rach2-recover - the recovery system's menu for a machine that will not boot. Source: utils/rach2-recover (a shell script), its header and menu.

* SYNOPSIS: `rach2-recover`. No options.
* Runs from the recovery system, which mounts nothing from the disk it repairs, so a root that will not mount is still a root this can write. Every step can be left without doing anything, and every step says in one line what it did or why it did not, as `rach2-recover: ...`.
* The menu: 1 what this machine has: each disk as `/dev/diskN: MODEL, serial S, BYTES bytes, TRANSPORT, table T` and its partitions from `/proc/disk`; 2 check a file system (e2fsck on a chosen partition); 3 put a saved image back on a disk: a whole disk, never a partition, since an image begins with a table; the disk is asked for (`/dev/diskN`, checked to be a whole disk of this machine), the image offered is the stick's own when the stick is mounted, the image is hashed and compared with its manifest before anything is written, then written with `reflash -p` at offset 0 and read back with `reflash --verify`, and a hash that differs is said; 4 install a kernel on a boot partition (through bootctl); 5 a shell; 6 leave. (Read the script's four functions, `list_disks`, `check_volume`, `restore_image`, `install_kernel`, for what each asks and refuses; state only what they do.)
* A signal (INT, TERM, HUP) ends the run and unmounts what the menu had mounted under `/mnt/recover`.
* FILES: /proc/disk, /mnt/recover.
* SEE ALSO: recovery(7), rach2-install(8), reflash(8), bootctl(8), e2fsck(8).
