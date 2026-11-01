#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
pthread_mutex_t m=PTHREAD_MUTEX_INITIALIZER; pthread_cond_t c=PTHREAD_COND_INITIALIZER; int go=0;
void* w(void*a){pthread_mutex_lock(&m);while(!go)pthread_cond_wait(&c,&m);pthread_mutex_unlock(&m);return 0;}
int main(int ac,char**av){int n=atoi(av[1]);pthread_t*t=malloc(n*sizeof*t);double t0=now();
 for(int i=0;i<n;i++)pthread_create(&t[i],0,w,0); double t1=now();
 pthread_mutex_lock(&m);go=1;pthread_cond_broadcast(&c);pthread_mutex_unlock(&m);
 for(int i=0;i<n;i++)pthread_join(t[i],0); double t2=now();
 printf("C   n=%5d create %.3fs  broadcast+join %.3fs\n",n,t1-t0,t2-t1);}
