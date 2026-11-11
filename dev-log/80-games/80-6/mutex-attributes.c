/* Read the deployed libc's response to Winepulse's mutex attributes. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/** Compare robust PI initialization with GE-Proton's default fallback. */
int main(void)
{
    pthread_mutexattr_t a;
    pthread_mutex_t m;
    int rc;

    pthread_mutexattr_init(&a);
    pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
    pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST);
    rc = pthread_mutex_init(&m, &a);
    printf("mutex-attributes: result: pi+robust init: %d (%s)\n", rc, strerror(rc));
    if (rc == 0) pthread_mutex_destroy(&m);
    rc = pthread_mutex_init(&m, NULL);
    if (rc == 0) {
        printf("mutex-attributes: result: default init: 0, kind %d\n", m.__data.__kind);
        pthread_mutex_destroy(&m);
    } else {
        fprintf(stderr, "mutex-attributes: default initialization: %s\n", strerror(rc));
    }
    pthread_mutexattr_destroy(&a);
    return rc != 0;
}
