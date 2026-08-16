# Build the Rach2 utilities on Rach2 itself.
#
# The POSIX makefile pdpmake reads in the guest, driving the staged tcc
# through /bin/cc; the GNU Makefile beside it stays the host build. Each
# program is one hosted static link against the compiler's own sysroot
# (musl's headers and libc.a, the crt objects), which is exactly what the
# host build produces through the muslgcc wrapper. The per-program source
# map below must equal the host Makefile's; tools/check_guest.py holds the
# two maps equal on the host.
#
# Run from a writable copy of this tree: make -f guest.mk

UCC = cc
UFLAGS = -static

PROGS = true false echo yes cat pwd touch mkdir rm mv cp ls ping nc \
        httpd halt init-shell init-login init-metal login su sudo passwd adduser \
        adduser-crash install-kernel

all: $(PROGS)

true: true.c
	$(UCC) $(UFLAGS) -o true true.c
false: false.c
	$(UCC) $(UFLAGS) -o false false.c
echo: echo.c
	$(UCC) $(UFLAGS) -o echo echo.c
yes: yes.c
	$(UCC) $(UFLAGS) -o yes yes.c
cat: cat.c
	$(UCC) $(UFLAGS) -o cat cat.c
pwd: pwd.c
	$(UCC) $(UFLAGS) -o pwd pwd.c
touch: touch.c
	$(UCC) $(UFLAGS) -o touch touch.c
mkdir: mkdir.c
	$(UCC) $(UFLAGS) -o mkdir mkdir.c
rm: rm.c
	$(UCC) $(UFLAGS) -o rm rm.c
mv: mv.c
	$(UCC) $(UFLAGS) -o mv mv.c
cp: cp.c
	$(UCC) $(UFLAGS) -o cp cp.c
ls: ls.c
	$(UCC) $(UFLAGS) -o ls ls.c
ping: ping.c resolv.c netdb.h
	$(UCC) $(UFLAGS) -o ping ping.c resolv.c
nc: nc.c resolv.c netdb.h
	$(UCC) $(UFLAGS) -o nc nc.c resolv.c
httpd: httpd.c
	$(UCC) $(UFLAGS) -o httpd httpd.c
halt: halt.c
	$(UCC) $(UFLAGS) -o halt halt.c
init-shell: init.c
	$(UCC) $(UFLAGS) -o init-shell init.c
init-login: init.c
	$(UCC) $(UFLAGS) -DINIT_CONSOLE_LOGIN -o init-login init.c
init-metal: init.c
	$(UCC) $(UFLAGS) -DINIT_CONSOLE_LOGIN -DINIT_METAL -o init-metal init.c
login: login.c pwauth.c pwauth.h
	$(UCC) $(UFLAGS) -o login login.c pwauth.c
su: su.c pwauth.c pwauth.h
	$(UCC) $(UFLAGS) -o su su.c pwauth.c
sudo: sudo.c
	$(UCC) $(UFLAGS) -o sudo sudo.c
passwd: passwd.c pwauth.c pwauth.h
	$(UCC) $(UFLAGS) -o passwd passwd.c pwauth.c
adduser: adduser.c pwauth.c pwauth.h
	$(UCC) $(UFLAGS) -o adduser adduser.c pwauth.c
adduser-crash: adduser.c pwauth.c pwauth.h
	$(UCC) $(UFLAGS) -DADDUSER_CRASH_POINT -o adduser-crash adduser.c pwauth.c
install-kernel: install-kernel.c bootslot.c bootslot.h
	$(UCC) $(UFLAGS) -o install-kernel install-kernel.c bootslot.c

clean:
	rm -f $(PROGS)
