/* A shared library that refers to the IFUNC pick of the executable that
   loads it. */
extern int pick(int);

/** Return the address of pick that the library's own reference takes. */
void *ref_pick(void)
{
    return (void *)pick;
}
