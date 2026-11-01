#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <netinet/in.h>
int main(int c, char** v){
  if (c > 1) { /* exec'd child: report which fds survived */
    for (int i = 2; i < c; i++) { int fd = atoi(v[i]); struct stat st; printf("  fd %-3d %-26s after exec: %s\n", fd, v[1+0]==0?"":getenv(v[i]) , fstat(fd,&st)==0 ? "OPEN  <-- CLOEXEC NOT HONORED" : "closed (ok)"); }
    return 0; }
  int sp[2]; socketpair(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0, sp);
  int pp[2]; pipe2(pp, O_CLOEXEC);
  int of = open("/etc/passwd", O_RDONLY|O_CLOEXEC);
  int ep = epoll_create1(EPOLL_CLOEXEC);
  int ev = eventfd(0, EFD_CLOEXEC);
  int dp = fcntl(0, F_DUPFD_CLOEXEC, 50);
  int d3 = dup3(of, 60, O_CLOEXEC);
  int sk = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, 0);
  int fs = open("/etc/hostname", O_RDONLY); fcntl(fs, F_SETFD, FD_CLOEXEC);
  int ls = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a = {AF_INET}; a.sin_addr.s_addr = htonl(0x7f000001); bind(ls,(void*)&a,sizeof a); listen(ls,1); socklen_t al=sizeof a; getsockname(ls,(void*)&a,&al);
  int cl = socket(AF_INET, SOCK_STREAM, 0); connect(cl,(void*)&a,sizeof a); int ac = accept4(ls, 0, 0, SOCK_CLOEXEC);
  int rc[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, rc); int sent = open("/etc/hosts", O_RDONLY);
  char cb[CMSG_SPACE(sizeof(int))] = {0}; struct iovec io = {"x",1}; struct msghdr m = {0}; m.msg_iov=&io; m.msg_iovlen=1; m.msg_control=cb; m.msg_controllen=sizeof cb;
  struct cmsghdr* cm = CMSG_FIRSTHDR(&m); cm->cmsg_level=SOL_SOCKET; cm->cmsg_type=SCM_RIGHTS; cm->cmsg_len=CMSG_LEN(sizeof(int)); *(int*)CMSG_DATA(cm)=sent; sendmsg(rc[0],&m,0);
  struct msghdr r = {0}; char rb[1]; struct iovec ri={rb,1}; char rcb[CMSG_SPACE(sizeof(int))]; r.msg_iov=&ri; r.msg_iovlen=1; r.msg_control=rcb; r.msg_controllen=sizeof rcb; recvmsg(rc[1],&r,MSG_CMSG_CLOEXEC); int got=*(int*)CMSG_DATA(CMSG_FIRSTHDR(&r));
  char b[16][16]; int fds[] = {sp[0], sp[1], pp[0], pp[1], of, ep, ev, dp, d3, sk, fs, ac, got}; const char* names[] = {"socketpair SOCK_CLOEXEC[0]","socketpair SOCK_CLOEXEC[1]","pipe2 O_CLOEXEC[0]","pipe2 O_CLOEXEC[1]","open O_CLOEXEC","epoll_create1 CLOEXEC","eventfd EFD_CLOEXEC","F_DUPFD_CLOEXEC","dup3 O_CLOEXEC","socket SOCK_CLOEXEC","F_SETFD FD_CLOEXEC","accept4 SOCK_CLOEXEC","recvmsg MSG_CMSG_CLOEXEC"};
  char* av[20]; av[0] = v[0]; av[1] = "x"; int n = 13;
  for (int i = 0; i < n; i++) { snprintf(b[i], 16, "%d", fds[i]); av[i+2] = b[i]; setenv(b[i], names[i], 1); } av[n+2] = 0;
  printf("fd flags before exec (FD_CLOEXEC bit):"); for (int i=0;i<n;i++) printf(" %d", fcntl(fds[i], F_GETFD)); printf("\n"); fflush(stdout);
  pid_t p = fork(); if (!p) { execv(v[0], av); _exit(127); } waitpid(p, 0, 0); return 0; }
