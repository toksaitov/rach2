# bootlog(4)

NAME: bootlog, screenlog, framefail - the kernel's records and its test hooks. Source: devfs.c (bootlog 240/2 0400 bootlog_ops; screenlog 240/38 0600 screenlog_ops; framefail 240/37 0600 framefail_ops) and rach2/sys/fs/Readme.md (the synthetic file system section on /dev/screenlog and /dev/bootlog); utils/Readme.md "Init and account programs" (init stops the kernel's screen output when the login owns the console).
* `/dev/bootlog`: this boot's kernel log from its first line, for root; read whole; `dmesg` is a script over it. `/dev/screenlog`: reads and sets the level the kernel prints to the screen (what the console shows once init owns it). `/dev/framefail`: a test hook that makes frame allocation fail (say what the ops do; if it is for the probes only, say so).
* SEE ALSO: console(4), dmesg(1) (phase 4), proc(5).
