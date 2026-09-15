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
# Each program is linked to a name of its own and renamed into place, since
# the compiler writes the executable into its destination: a link the machine
# stops part way would otherwise leave a file newer than its source, and the
# next make would call it built.
#
# Run from a writable copy of this tree: make -f guest.mk

UCC = cc
UFLAGS = -static

include programs.mk

all: $(UTILITIES) $(SBIN_PROGS)

true: true.c
	$(UCC) $(UFLAGS) -o true.new true.c
	mv true.new true
false: false.c
	$(UCC) $(UFLAGS) -o false.new false.c
	mv false.new false
echo: echo.c
	$(UCC) $(UFLAGS) -o echo.new echo.c
	mv echo.new echo
yes: yes.c
	$(UCC) $(UFLAGS) -o yes.new yes.c
	mv yes.new yes
cat: cat.c
	$(UCC) $(UFLAGS) -o cat.new cat.c
	mv cat.new cat
pwd: pwd.c
	$(UCC) $(UFLAGS) -o pwd.new pwd.c
	mv pwd.new pwd
touch: touch.c
	$(UCC) $(UFLAGS) -o touch.new touch.c
	mv touch.new touch
mkdir: mkdir.c
	$(UCC) $(UFLAGS) -o mkdir.new mkdir.c
	mv mkdir.new mkdir
rm: rm.c
	$(UCC) $(UFLAGS) -o rm.new rm.c
	mv rm.new rm
mv: mv.c
	$(UCC) $(UFLAGS) -o mv.new mv.c
	mv mv.new mv
cp: cp.c
	$(UCC) $(UFLAGS) -o cp.new cp.c
	mv cp.new cp
ls: ls.c
	$(UCC) $(UFLAGS) -o ls.new ls.c
	mv ls.new ls
httpd: httpd.c
	$(UCC) $(UFLAGS) -o httpd.new httpd.c
	mv httpd.new httpd
halt: halt.c
	$(UCC) $(UFLAGS) -o halt.new halt.c
	mv halt.new halt

loopctl: loopctl.c
	$(UCC) $(UFLAGS) -o loopctl.new loopctl.c
	mv loopctl.new loopctl
growroot: growroot.c
	$(UCC) $(UFLAGS) -o growroot.new growroot.c
	mv growroot.new growroot

growset: growset.c
	$(UCC) $(UFLAGS) -o growset.new growset.c
	mv growset.new growset

growboot: $(SRC_growboot) $(HDR_growboot)
	$(UCC) $(UFLAGS) -o growboot.new $(SRC_growboot)
	mv growboot.new growboot

diskctl: diskctl.c
	$(UCC) $(UFLAGS) -o diskctl.new diskctl.c
	mv diskctl.new diskctl

update: update.c
	$(UCC) $(UFLAGS) -o update.new update.c
	mv update.new update
sudo: sudo.c
	$(UCC) $(UFLAGS) -o sudo.new sudo.c
	mv sudo.new sudo
ping: $(SRC_ping) $(HDR_ping)
	$(UCC) $(UFLAGS) -o ping.new $(SRC_ping)
	mv ping.new ping
nc: $(SRC_nc) $(HDR_nc)
	$(UCC) $(UFLAGS) -o nc.new $(SRC_nc)
	mv nc.new nc
dhcpc: $(SRC_dhcpc) $(HDR_dhcpc)
	$(UCC) $(UFLAGS) -o dhcpc.new $(SRC_dhcpc)
	mv dhcpc.new dhcpc
netup: $(SRC_netup) $(HDR_netup)
	$(UCC) $(UFLAGS) -o netup.new $(SRC_netup)
	mv netup.new netup
reflash: $(SRC_reflash) $(HDR_reflash)
	$(UCC) $(UFLAGS) -o reflash.new $(SRC_reflash)
	mv reflash.new reflash
init-shell: $(SRC_init_shell)
	$(UCC) $(UFLAGS) -o init-shell.new $(SRC_init_shell)
	mv init-shell.new init-shell
init-login: $(SRC_init_login)
	$(UCC) $(UFLAGS) $(DEF_init_login) -o init-login.new $(SRC_init_login)
	mv init-login.new init-login
init-metal: $(SRC_init_metal)
	$(UCC) $(UFLAGS) $(DEF_init_metal) -o init-metal.new $(SRC_init_metal)
	mv init-metal.new init-metal
init-recovery: $(SRC_init_recovery)
	$(UCC) $(UFLAGS) $(DEF_init_recovery) -o init-recovery.new $(SRC_init_recovery)
	mv init-recovery.new init-recovery
login: $(SRC_login) $(HDR_login)
	$(UCC) $(UFLAGS) -o login.new $(SRC_login)
	mv login.new login
su: $(SRC_su) $(HDR_su)
	$(UCC) $(UFLAGS) -o su.new $(SRC_su)
	mv su.new su
passwd: $(SRC_passwd) $(HDR_passwd)
	$(UCC) $(UFLAGS) -o passwd.new $(SRC_passwd)
	mv passwd.new passwd
adduser: $(SRC_adduser) $(HDR_adduser)
	$(UCC) $(UFLAGS) -o adduser.new $(SRC_adduser)
	mv adduser.new adduser
adduser-crash: $(SRC_adduser_crash) $(HDR_adduser_crash)
	$(UCC) $(UFLAGS) $(DEF_adduser_crash) -o adduser-crash.new $(SRC_adduser_crash)
	mv adduser-crash.new adduser-crash
bootctl: $(SRC_bootctl) $(HDR_bootctl)
	$(UCC) $(UFLAGS) -o bootctl.new $(SRC_bootctl)
	mv bootctl.new bootctl

clean:
	rm -f $(UTILITIES) $(SBIN_PROGS) $(UTILITIES:=.new) $(SBIN_PROGS:=.new)
