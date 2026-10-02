# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# rsync, with OpenSSL's digests and zstd's compression beside its own zlib.
# xxHash, LZ4 and libidn2 are not ported, and ACLs would need libacl. The
# manual pages are the ones the release carries. configure runs test
# programs to learn what the C library and the kernel do, which a cross
# build cannot run; CACHE gives the answers glibc and this kernel give, where
# configure's own guess for a cross build would make rsync use its
# replacements.
VERSION=3.5.1
DEPENDS="openssl zstd"
CONFIGURE_ARGS="--disable-xxhash --disable-lz4 --disable-idn --disable-acl-support --disable-md2man"
CACHE="rsync_cv_HAVE_BROKEN_LARGEFILE=no rsync_cv_HAVE_SOCKETPAIR=yes rsync_cv_HAVE_BROKEN_READDIR=no rsync_cv_HAVE_C99_VSNPRINTF=yes rsync_cv_HAVE_SECURE_MKSTEMP=yes"
