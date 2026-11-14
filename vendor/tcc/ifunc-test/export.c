/* An IFUNC that a dynamic executable defines, exports and points to: pick is
   a resolver, also named pick_resolver, that returns triple. */
int triple(int x)
{
    return 3 * x;
}

#ifdef __x86_64__
__asm__(".text\n.globl pick, pick_resolver\n.type pick,@gnu_indirect_function\n"
        "pick_resolver:\npick:\nleaq triple(%rip),%rax\nret\n");
#else
__asm__(".text\n.globl pick, pick_resolver\n.type pick,@gnu_indirect_function\n"
        "pick_resolver:\npick:\nmovl $triple,%eax\nret\n");
#endif

extern int pick(int);
int (*fp)(int) = pick;

/** Stand for the entry point that -nostdlib leaves out; the check only reads
    the executable. */
void _start(void)
{
    for (;;)
        ;
}
