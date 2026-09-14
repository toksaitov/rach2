# utils/programs.mk: the one utility program map, read by the host Makefile
# and by guest.mk under pdpmake on Rach2. UTILITIES and SBIN_PROGS name the
# programs; SRC_* lists a program's compile operands, HDR_* its headers that
# are prerequisites beside the sources, and DEF_* its extra compile defines.
# A program without an SRC_* entry builds from NAME.c alone. Macro names
# replace `-` with `_` because a POSIX macro name cannot contain a dash.

UTILITIES = true false echo yes cat pwd touch mkdir rm mv cp ls ping nc \
            httpd dhcpc netup reflash halt update loopctl growroot diskctl

SBIN_PROGS = init-shell init-login init-metal init-recovery login su sudo passwd adduser \
             adduser-crash bootctl

SRC_ping = ping.c resolv.c
HDR_ping = netdb.h
SRC_nc = nc.c resolv.c
HDR_nc = netdb.h
SRC_dhcpc = dhcpc.c dhcpmsg.c ipcheck.c
HDR_dhcpc = dhcpmsg.h ipcheck.h
SRC_netup = netup.c netcfg.c ipcheck.c
HDR_netup = netcfg.h ipcheck.h
SRC_reflash = reflash.c sectorcopy.c sha256.c
HDR_reflash = sectorcopy.h sha256.h
SRC_init_shell = init.c
SRC_init_login = init.c
DEF_init_login = -DINIT_CONSOLE_LOGIN
SRC_init_metal = init.c
DEF_init_metal = -DINIT_CONSOLE_LOGIN -DINIT_METAL
# The recovery system's init: a shell at the console, and the rest of the metal
# init beside it, which is the network and the optional SSH server. Neither of
# the other two does both. `init-shell` gives a shell and supervises nothing;
# `init-metal` supervises everything and demands a login that a system with no
# passwords cannot answer, which would have left the rescue system impossible
# to enter by any route.
SRC_init_recovery = init.c
DEF_init_recovery = -DINIT_METAL -DINIT_RECOVERY
SRC_login = login.c pwauth.c
HDR_login = pwauth.h
SRC_su = su.c pwauth.c
HDR_su = pwauth.h
SRC_passwd = passwd.c pwauth.c
HDR_passwd = pwauth.h
SRC_adduser = adduser.c pwauth.c
HDR_adduser = pwauth.h
SRC_adduser_crash = adduser.c pwauth.c
HDR_adduser_crash = pwauth.h
DEF_adduser_crash = -DADDUSER_CRASH_POINT
SRC_bootctl = bootctl.c bootslot.c bootverb.c grubslot.c
HDR_bootctl = bootslot.h bootverb.h grubslot.h
