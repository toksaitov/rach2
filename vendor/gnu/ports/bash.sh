# GNU bash with the official patches. The bundled termcap and readline are
# used, and bash's own malloc is not.
VERSION=5.3
PATCH_STRIP=0
PATCHES="bash-patch-001 bash-patch-002 bash-patch-003 bash-patch-004 bash-patch-005 bash-patch-006 bash-patch-007 bash-patch-008 bash-patch-009 bash-patch-010 bash-patch-011 bash-patch-012 bash-patch-013 bash-patch-014 bash-patch-015 bash-patch-016 bash-patch-017 bash-patch-018 bash-patch-019 bash-patch-020"
CONFIGURE_ARGS="--disable-nls --without-bash-malloc"
