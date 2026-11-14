/* A dynamic executable that exports the IFUNC pick, which pick64.s defines,
   loads libref.so, which refers to it, and prints IFUNCRUN and three flags:
   that dlsym gives the address of pick that the library's reference takes,
   that the executable's own pointers to pick, one in initialized data and
   one taken at run time, hold the same address, and that a call through
   each reaches triple. Linked with -nostdlib against the
   C library itself, so it declares what it calls and starts at _start. */
int triple(int x)
{
    return 3 * x;
}

extern int pick(int);
int (*fp)(int) = pick;

void *dlopen(const char *path, int mode);
void *dlsym(void *handle, const char *name);
long write(int fd, const void *buf, unsigned long n);
void _exit(int status);

/** Print the result and end the program; called by _start. */
void run(void)
{
    char line[] = "IFUNCRUN 0 0 0\n";
    void *h, *by_name;
    void *(*ref)(void);
    int (*p)(int) = pick;

    h = dlopen("./libref.so", 2); /* RTLD_NOW */
    ref = h ? (void *(*)(void))dlsym(h, "ref_pick") : 0;
    by_name = dlsym(0, "pick"); /* RTLD_DEFAULT */
    if (ref && by_name && by_name == ref())
        line[9] = '1';
    if (by_name && by_name == (void *)fp && by_name == (void *)p)
        line[11] = '1';
    if (fp(7) == 21 && p(5) == 15)
        line[13] = '1';
    write(1, line, sizeof line - 1);
    _exit(0);
}

/* The entry point: the ABI leaves the stack 16-byte aligned with no return
   address, so align it for the call to run, which does not return. */
#ifdef __x86_64__
__asm__(".text\n.globl _start\n_start:\nxorl %ebp,%ebp\nandq $-16,%rsp\n"
        "call run\nhlt\n");
#else
__asm__(".text\n.globl _start\n_start:\nxorl %ebp,%ebp\nandl $-16,%esp\n"
        "call run\nhlt\n");
#endif
