# passwd(1)

NAME: passwd - change an account's password. Source: utils/passwd.c header, utils/Readme.md; BusyBox loginutils/passwd.c usage for os-real.
* This project's `passwd [--] [USER]` (os-simple, set-user-id root): the account defaults to the caller's; naming another needs a real uid of 0; root changes any password without the old one, everyone else proves the current one first; the new password twice; the records locked and rechecked before `/etc/shadow` is replaced (passwd(5): the lock, the temporary and the rename); passwords at most 100 bytes.
* On os-real `passwd` is BusyBox's applet from the set-id build: `passwd [-a ALG] [-dlu] [USER]`: `-a ALG` the crypt algorithm, `-d` set an empty password, `-l` lock, `-u` unlock.
* EXAMPLE: `passwd`; `tests/run account` changes a password and logs in with it.
* SEE ALSO: passwd(5), login(1), su(1), adduser(8).
