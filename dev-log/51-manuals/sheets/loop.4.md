# loop(4)

NAME: loop, loop-control - the loop devices. Source: loopctl(8)'s sheet, the block Readme "The loop device", devfs.c (loop0-3 240/32-35 0400; loop-control 240/36 0600).
* The four devices, their modes, the control node's command lines, `/proc/loop`, the rules (regular file of whole sectors, one attachment per file, a chain one deep, read-only attachment, EBUSY on the file while attached, detach refusals, a pulled medium).
* SEE ALSO: loopctl(8), mount(8), disk(4).
* Found at the proofreading: an empty file is refused (EINVAL); a removed medium marks the device gone and a mount or transfer delays its release; the stored path is the first 63 bytes.
