/*
 * fault_host.c: a page that faults on every access, and a hook for its
 * fault, for a host test that needs a real fault inside the kernel code it
 * includes, as a kernel fault handler would take it.
 */
#include <signal.h>
#include <stddef.h>
#include <sys/mman.h>
#include <unistd.h>

void *host_fault_page(void);
int host_fault_catch(void (*hook)(void));

/* What a fault runs; it must not return, since the access would run again. */
static void (*fault_hook)(void);

/** Run the hook for SIGSEGV or SIGBUS, which an access to the page raises
 * on Linux and on macOS. */
static void on_fault(int signo)
{
    (void)signo;
    fault_hook();
}

/** A page that faults on every access, mapped once and kept; NULL when it
 * cannot be had. */
void *host_fault_page(void)
{
    static void *page;

    if (page == NULL) {
        page = mmap(NULL, (size_t)sysconf(_SC_PAGESIZE), PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (page == MAP_FAILED) {
            page = NULL;
        }
    }
    return page;
}

/**
 * Have a fault run `hook`, or, for NULL, end the process again as by
 * default. The hook leaves by `longjmp`: SA_NODEFER keeps the signal
 * unblocked meanwhile, so that a jump out of the handler leaves the mask as
 * it was on every host. 0, or -1 when a handler could not be set.
 */
int host_fault_catch(void (*hook)(void))
{
    struct sigaction sa;

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sa.sa_handler = hook != NULL ? on_fault : SIG_DFL;
    fault_hook = hook;
    if (sigaction(SIGSEGV, &sa, NULL) != 0 || sigaction(SIGBUS, &sa, NULL) != 0) {
        return -1;
    }
    return 0;
}
