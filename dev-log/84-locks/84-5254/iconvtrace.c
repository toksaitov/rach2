/* iconvtrace.c: an LD_PRELOAD that reports each iconv_open and its callers. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <iconv.h>
#include <stdio.h>

iconv_t iconv_open(const char *to, const char *from)
{
    iconv_t (*real)(const char *, const char *) = (iconv_t (*)(const char *, const char *))dlsym(RTLD_NEXT, "iconv_open");
    void *bt[12];
    int n = backtrace(bt, 12);

    fprintf(stderr, "iconvtrace: iconv_open(\"%s\", \"%s\")\n", to, from);
    backtrace_symbols_fd(bt, n, 2);
    return real(to, from);
}
