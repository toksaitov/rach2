#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
#define B(name,N,body) do{double t0=now();for(int i=0;i<(N);i++){body;}double dt=now()-t0;printf("%-34s %10.2f us/op\n",name,dt/(N)*1e6);}while(0)
int main(int argc,char**argv){
  const char*dir=argc>1?argv[1]:".";char p[4096],q[4096];struct stat st;char buf[4096];
  snprintf(p,sizeof p,"%s/exists",dir);close(open(p,O_CREAT|O_WRONLY,0644));
  snprintf(q,sizeof q,"%s/a/b/c/d/e/f/g/h",dir);
  B("syscall(getpid)",200000,syscall(SYS_getpid));
  B("clock_gettime (vdso?)",200000,{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);});
  B("stat existing (short path)",20000,stat(p,&st));
  B("stat missing",20000,stat(q,&st));
  B("open+close existing",20000,close(open(p,O_RDONLY)));
  int fd=open(p,O_RDWR);
  B("pread 4KB",20000,pread(fd,buf,4096,0));
  B("pwrite 4KB",20000,pwrite(fd,buf,4096,0));
  B("fstat",100000,fstat(fd,&st));
  B("lseek",100000,lseek(fd,0,SEEK_SET));
  int pp[2];pipe(pp);
  B("pipe write+read 1B",50000,{write(pp[1],"x",1);read(pp[0],buf,1);});
  B("mmap+munmap 64KB anon",20000,munmap(mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0),65536));
  B("mmap+touch 16 pages+munmap",5000,{char*m=mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);for(int k=0;k<65536;k+=4096)m[k]=1;munmap(m,65536);});
  int n=0;
  B("create+unlink file",2000,{snprintf(buf,sizeof buf,"%s/t%d",dir,i);close(open(buf,O_CREAT|O_WRONLY,0644));unlink(buf);});
  B("mkdir+rmdir",2000,{snprintf(buf,sizeof buf,"%s/d%d",dir,i);mkdir(buf,0755);rmdir(buf);});
  B("rename",2000,{snprintf(buf,sizeof buf,"%s/r%d",dir,i&1);snprintf(q,sizeof q,"%s/r%d",dir,!(i&1));if(i==0)close(open(buf,O_CREAT|O_WRONLY,0644));rename(buf,q);});
  B("opendir+readdir+closedir",5000,{DIR*d=opendir(dir);while(readdir(d))n++;closedir(d);});
  B("fork+wait (child _exit)",500,{pid_t c=fork();if(!c)_exit(0);waitpid(c,0,0);});
  B("vfork+exec /bin/true",300,{pid_t c=vfork();if(!c){execl("/bin/true","true",(char*)0);_exit(1);}waitpid(c,0,0);});
  return 0;}
