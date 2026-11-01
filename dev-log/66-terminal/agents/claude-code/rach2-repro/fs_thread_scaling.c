#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static const char* P; static int N=3000;
void* w(void*a){struct stat st;for(int i=0;i<N;i++)stat(P,&st);return 0;}
int main(int c,char**v){
  /* run from a dir containing deep/f, deep/1/2/3/f, deep/1/2/3/4/5/6/7/f (run_all.sh creates them) */
  const char* paths[]={"deep/f","deep/1/2/3/f","deep/1/2/3/4/5/6/7/f",0};
  for(int k=0;paths[k];k++){P=paths[k];double t=now();w(0);printf("1 thread  stat %-24s %7.1f us/op\n",P,(now()-t)/N*1e6);}
  int fd=open("deep",O_RDONLY|O_DIRECTORY); struct stat st; double t=now(); for(int i=0;i<N;i++) fstatat(fd,"f",&st,0); printf("1 thread  fstatat(dirfd,\"f\")            %7.1f us/op\n",(now()-t)/N*1e6);
  P="deep/1/2/3/f";
  for(int n=1;n<=4;n*=2){pthread_t th[8];t=now();for(int i=0;i<n;i++)pthread_create(&th[i],0,w,0);for(int i=0;i<n;i++)pthread_join(th[i],0);double dt=now()-t;printf("%d threads: aggregate %7.0f stats/s (wall %.2fs)\n",n,n*N/dt,dt);}
}
