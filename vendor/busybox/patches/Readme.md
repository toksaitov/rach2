# busybox patches

Every `.patch` here is applied, in name order, by `build_busybox.sh` after it
extracts the pinned tarball and before it configures. The vendored tree stays as
upstream shipped it; a change to it lives here, where the diff and its reasons
can be read.

A patch is for something the ported musl cannot supply and upstream cannot be
expected to know about. It is not the place to change what busybox does.

This file is tracked so the directory exists in a fresh checkout even when no
patch does: `BUSYBOX_STAMP` names the directory as a prerequisite, so that
removing the last patch invalidates the stamp, and a prerequisite that is not
there is an error rather than a rebuild.
