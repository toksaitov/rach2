# passwd(5)

NAME: passwd, shadow, group - the account files. Sources: utils/pwauth.c and pwauth.h (the formats read and written, the lock, the replacement idiom), utils/adduser.c (the four publications and their order), utils/Readme.md "Credential transactions"; the staged /etc/passwd text in the manifests; BusyBox's login/su/passwd read the same files with shadow support and SHA crypt.
* `/etc/passwd`: `name:x:uid:gid:gecos:home:shell`, mode 0644. `/etc/shadow`: `name:hash:...` (state the fields this system writes and reads; the hash is `$6$salt$...` SHA-512 crypt; a locked account's field starts with `!`; an empty field), root only. `/etc/group`: `name:x:gid:members`. Names at most 63 bytes, passwords at most 100 bytes. adduser publishes shadow (locked) first, then group, then the 0700 home, then passwd, so an interruption leaves no passwd account without a locked shadow record.
* The writers' lock `/etc/.pwlock` (0600, holds the pid, never broken; `rm /etc/.pwlock` by root after a killed writer); the replacement written as `FILE.tmp.PID` and renamed; readers take no lock.
* The demonstration accounts the images carry (root and test) and that the installer removes them from the installed root.
* SEE ALSO: login(1), su(1), passwd(1), adduser(8) (phase 4), sshd.conf(5).
