/* A dynamic executable that exports and points to the IFUNC pick, which
   pick32.s or pick64.s defines as a resolver that returns triple. */
int triple(int x)
{
    return 3 * x;
}

extern int pick(int);
int (*fp)(int) = pick;

/** Stand for the entry point that -nostdlib leaves out; the check only reads
    the executable. */
void _start(void)
{
    for (;;)
        ;
}
