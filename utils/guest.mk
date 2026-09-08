# Build the Rach2 utilities on Rach2 itself.
#
# The POSIX makefile pdpmake reads in the guest, driving the staged tcc
# through /bin/cc; the GNU Makefile beside it stays the host build. Each
# program is one hosted static link against the compiler's own sysroot
# (musl's headers and libc.a, the crt objects), which is exactly what the
# host build produces through the muslgcc wrapper. Both makefiles include
# programs.mk, so one file lists the programs, their companion sources,
# and their extra defines, and the two builds cannot drift apart.
#
# Run from a writable copy of this tree: make -f guest.mk

UCC = cc
UFLAGS = -static

include programs.mk

all: $(UTILITIES) $(SBIN_PROGS)

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
httpd: httpd.c
	$(UCC) $(UFLAGS) -o httpd httpd.c
halt: halt.c
	$(UCC) $(UFLAGS) -o halt halt.c

loopctl: loopctl.c
	$(UCC) $(UFLAGS) -o loopctl loopctl.c
growroot: growroot.c
	$(UCC) $(UFLAGS) -o growroot growroot.c

diskctl: diskctl.c
	$(UCC) $(UFLAGS) -o diskctl diskctl.c

update: update.c
	$(UCC) $(UFLAGS) -o update update.c
sudo: sudo.c
	$(UCC) $(UFLAGS) -o sudo sudo.c
ping: $(SRC_ping) $(HDR_ping)
	$(UCC) $(UFLAGS) -o ping $(SRC_ping)
nc: $(SRC_nc) $(HDR_nc)
	$(UCC) $(UFLAGS) -o nc $(SRC_nc)
dhcpc: $(SRC_dhcpc) $(HDR_dhcpc)
	$(UCC) $(UFLAGS) -o dhcpc $(SRC_dhcpc)
netup: $(SRC_netup) $(HDR_netup)
	$(UCC) $(UFLAGS) -o netup $(SRC_netup)
reflash: $(SRC_reflash) $(HDR_reflash)
	$(UCC) $(UFLAGS) -o reflash $(SRC_reflash)
init-shell: $(SRC_init_shell)
	$(UCC) $(UFLAGS) -o init-shell $(SRC_init_shell)
init-login: $(SRC_init_login)
	$(UCC) $(UFLAGS) $(DEF_init_login) -o init-login $(SRC_init_login)
init-metal: $(SRC_init_metal)
	$(UCC) $(UFLAGS) $(DEF_init_metal) -o init-metal $(SRC_init_metal)
login: $(SRC_login) $(HDR_login)
	$(UCC) $(UFLAGS) -o login $(SRC_login)
su: $(SRC_su) $(HDR_su)
	$(UCC) $(UFLAGS) -o su $(SRC_su)
passwd: $(SRC_passwd) $(HDR_passwd)
	$(UCC) $(UFLAGS) -o passwd $(SRC_passwd)
adduser: $(SRC_adduser) $(HDR_adduser)
	$(UCC) $(UFLAGS) -o adduser $(SRC_adduser)
adduser-crash: $(SRC_adduser_crash) $(HDR_adduser_crash)
	$(UCC) $(UFLAGS) $(DEF_adduser_crash) -o adduser-crash $(SRC_adduser_crash)
bootctl: $(SRC_bootctl) $(HDR_bootctl)
	$(UCC) $(UFLAGS) -o bootctl $(SRC_bootctl)

clean:
	rm -f $(UTILITIES) $(SBIN_PROGS)
