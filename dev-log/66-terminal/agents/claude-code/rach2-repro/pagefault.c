#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(){ size_t sz=256UL<<20; char*m=mmap(0,sz,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); double t=now(); for(size_t i=0;i<sz;i+=4096)m[i]=1; double dt=now()-t;
 printf("anon page faults: %.2f us/fault (%zu faults)\n", dt/(sz/4096)*1e6, sz/4096);
 t=now(); for(size_t i=0;i<sz;i+=4096)m[i]=2; printf("re-touch (no fault): %.3f us/page\n",(now()-t)/(sz/4096)*1e6);
 munmap(m,sz); m=mmap(0,sz,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE,-1,0); t=now(); munmap(m,sz); printf("munmap 256MB: %.1f ms\n",(now()-t)*1e3);
 t=now(); m=mmap(0,sz,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE,-1,0); printf("MAP_POPULATE 256MB: %.1f ms (%.2f us/page)\n",(now()-t)*1e3,(now()-t)/(sz/4096)*1e6); return 0;}
