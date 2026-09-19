# su(1)

NAME: su - become another user.

Two programs, as for login:
* On os-real and the installed root, `/bin/su` is BusyBox's applet, set-user-id root from `busybox-suid`. Usage: `su [-lmp] [-s SH] [-] [USER [FILE ARGS | -c 'CMD' [ARG0 ARGS]]]`: `-`, `-l` clear the environment, enter the home and run a login shell; `-m`, `-p` preserve the environment; `-s SH` the shell to run; USER defaults to root. Authenticates against `/etc/shadow` unless the caller is root. Adapted from BusyBox, GPL-2.0.
* On os-simple, `/bin/su` is this project's (`utils/su.c`), set-user-id root. Usage: `su [-] [-l|--login] [user] [-c command]`; user defaults to root; an unrecognized option or a second operand is a usage error (exit 2). Root becomes anyone without proof; anyone else proves the target's password (`Password: `, no echo), and an unknown account is `su: unknown user NAME` (exit 1), reported before any password is asked; a wrong password and a locked account are one refusal: `su: authentication failure` (exit 1). It closes descriptors 3 to 63, sheds supplementary groups, sets the group then the user (`su: cannot assume the target identity`), in login mode enters the target's home (`su: cannot enter DIR` is fatal), builds the environment from nothing (`PATH=/bin:/usr/bin`, `HOME`, `SHELL`, `USER`, `LOGNAME`), and execs the target's shell, with `-c command` as `SHELL -c command`; in login mode `argv[0]` is `-` and the shell's base name. `su: cannot exec SHELL` on failure. It holds no credential lock.
* EXAMPLE: `su - test -c pwd` prints `/home/test` after the password; `tests/run account` runs it on os-simple and os-real.
* SEE ALSO: login(1), passwd(1), shadow(5).
