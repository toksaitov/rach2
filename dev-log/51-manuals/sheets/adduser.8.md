# adduser(8)

NAME: adduser - create an account. Source: utils/adduser.c header and usage, utils/Readme.md; BusyBox loginutils/adduser.c usage for os-real.
* This project's `adduser [-u UID] [-s SHELL] NAME` (os-simple; root only, not set-id: a caller whose real uid is not 0 is refused before anything is opened): the shell defaults to `/bin/sh` and is not checked; the number is the lowest free as both uid and gid from 1000 to 60000 unless `-u`; one transaction under the credential lock across the four publications, in this order: a locked shadow entry (`!`), the group entry, the 0700 home, the passwd entry, so an interruption leaves no visible account without a locked record, and then `passwd NAME` is run after the lock is released. A prefix published before a failure is reported and not removed: `adduser: NAME exists and cannot be entered; run passwd NAME` and the others in the source.
* On os-real `adduser` is BusyBox's applet (its usage: `adduser [OPTIONS] USER [GROUP]` with `-h DIR -g GECOS -s SHELL -G GRP -S -D -H -u UID -k SKEL`), as the standing decision names BusyBox's account tools.
* EXAMPLE: `adduser alice` then the password prompts; `tests/run account` runs it on both images.
* SEE ALSO: passwd(1), passwd(5), busybox-loginutils(8).
