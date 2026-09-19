# random(4)

NAME: random, urandom - the entropy devices. Source: devfs.c dev_table (random 1/8, urandom 1/9, 0666, rand_ops); the entropy source in the kernel (the boot record `entropy: ready: RDRAND, trusted`; rach2/sys/entropy* or where rand_ops reads from); getrandom(2).
* Both read from the same source and never block; what the source is (RDRAND when the processor has it and it is trusted; otherwise what the code does); the boot record; getrandom(2) is the same source. `/dev/random` is not a blocking pool as on Linux.
* SEE ALSO: getrandom(2), null(4).
