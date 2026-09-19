# null(4)

NAME: null, zero, full - the empty devices. Source: rach2/sys/fs/devfs.c dev_table (null: major 1 minor 3, zero: 1/5, full: 1/7, each 0666) and their ops.
* `/dev/null` discards writes and reads as empty; `/dev/zero` reads as zeros without end; `/dev/full` reads as zeros and refuses writes with ENOSPC. All three mode 0666. The kernel's own file system, devfs, mounted at `/dev`; `stat` reports the major and minor as given.
* SEE ALSO: random(4), console(4), disk(4), loop(4), bootlog(4).
