# sudo(8)

NAME: sudo - run a command as root, if the real user is permitted. Source: utils/sudo.c header and usage.
* `sudo COMMAND [ARG ...]`: the reference implementation of the coursework exercise, staged only in the test image (the probe `sudoprobe` runs it); os-real ships `su` and leaves this to the student. Set-user-id root; it reads `/etc/sudoers`, one numeric uid per line, `#` a comment, and checks the caller's real uid, the identity the caller cannot forge; a permitted caller gets a clean environment (`PATH` and `HOME` only), inherited descriptors 3 through 63 closed, full root through the saved id, and the command by its explicit path; anyone else is refused; a missing or unreadable sudoers permits no one. Left for the exercise: per-command rules, a password prompt, terminal handling. `usage: sudo <command> [args...]`.
* FILES: /etc/sudoers.
* SEE ALSO: su(1), passwd(5).
