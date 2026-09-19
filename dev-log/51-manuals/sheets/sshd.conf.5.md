# sshd.conf(5)

NAME: sshd.conf - the SSH server's policy file. Source: utils/init.c root_password_login (lines 565-600), utils/rach2-install.txt (writes `root-password-login yes`), utils/Readme.md.
* `/etc/rach2/sshd.conf`: read by init before it starts Dropbear. One setting: the line `root-password-login yes`, matched whole (a line reading `root-password-login yesterday` is not it); with it init starts Dropbear without `-g`, so root may log in with a password; a file that is absent or says anything else leaves the refusal in place, and root arrives by key only. The RAM images and recovery carry no such file; `rach2-install` writes it on the installed root, and `--update` keeps it across a root replacement.
* Where the keys live: `/root/.ssh/authorized_keys`, `/home/NAME/.ssh/authorized_keys`; the host keys generated at boot for the RAM roots and kept on an installed root.
* SEE ALSO: rach2-install(8), recovery(7), dropbear(8) (phase 4).
