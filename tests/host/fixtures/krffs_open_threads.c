/** Run one opener until it waits, then resume it after the writer returns. */
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
static HANDLE thread;
static SRWLOCK mutex = SRWLOCK_INIT;
static CONDITION_VARIABLE condition = CONDITION_VARIABLE_INIT;
#define lock()   AcquireSRWLockExclusive(&mutex)
#define unlock() ReleaseSRWLockExclusive(&mutex)
#define wake()   WakeAllConditionVariable(&condition)
static void wait(void)
{
    if (!SleepConditionVariableSRW(&condition, &mutex, INFINITE, 0))
        abort();
}
#else
#include <pthread.h>
static pthread_t thread;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
#define lock()   pthread_mutex_lock(&mutex)
#define unlock() pthread_mutex_unlock(&mutex)
#define wake()   pthread_cond_broadcast(&condition)
#define wait()   pthread_cond_wait(&condition, &mutex)
#endif
/* Under `mutex`: the opener waits or has returned; the writer let it go on. */
static int stopped, resumed;
static void (*opener)(void);

/** The opener's thread: run `opener`, then tell the writer it stopped. */
#ifdef _WIN32
static DWORD WINAPI run(void *unused)
#else
static void *run(void *unused)
#endif
{
    (void)unused;
    opener();
    lock();
    stopped = 1;
    wake();
    unlock();
    return 0;
}

/** Start `fn` on a thread of its own and wait until it waits or returns. */
void open_thread_start(void (*fn)(void))
{
    stopped = resumed = 0;
    opener = fn;
#ifdef _WIN32
    thread = CreateThread(NULL, 0, run, NULL, 0, NULL);
    if (!thread)
        abort();
#else
    if (pthread_create(&thread, NULL, run, NULL) != 0)
        abort();
#endif
    lock();
    while (!stopped)
        wait();
    unlock();
}

/** Called by the opener inside the kernel's namespace-lock wait: tell the
 * writer it stopped and block until `open_thread_finish`. */
void open_thread_wait(void)
{
    lock();
    stopped = 1;
    wake();
    while (!resumed)
        wait();
    unlock();
}

/** Resume the opener and join its thread, so that its open has returned. */
void open_thread_finish(void)
{
    lock();
    resumed = 1;
    wake();
    unlock();
#ifdef _WIN32
    if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0 || !CloseHandle(thread))
        abort();
#else
    if (pthread_join(thread, NULL) != 0)
        abort();
#endif
}
