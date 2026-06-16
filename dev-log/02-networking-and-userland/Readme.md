# Milestone 2: networking and userland

This milestone grew Rach2 from the foundation kernel into a networked system running a musl userland that accepts SSH logins: the TCP/IP stack, the ported musl C library, BusyBox, the Dropbear SSH server, and the modernized GNU Make build.

It was built from `plan.md`, executed one phase at a time under the `/goal` runner, with a harsh independent Codex review at each phase. Unlike milestone 1, the prompts were not recorded, so `plan.md` is the record.
