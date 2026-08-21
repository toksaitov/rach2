/**
 * The host stand-in for musl's `<crypt.h>`.
 *
 * `utils/pwauth.c` is a guest source: it is compiled against the ported musl,
 * where `crypt(3)` is declared in this header. A native host need not have the
 * header at all (Darwin declares `crypt` in `<unistd.h>` and ships no
 * `crypt.h`), so a host test that compiles the library natively cannot find it.
 * The declaration is repeated here and the definition is the host test's, over
 * the vendored SHA-512 crypt the guest verifies with.
 *
 * This directory is on the include path of `tools/hosttest.sh` alone. Nothing
 * the guest builds ever sees it, and nothing here may differ from what musl
 * declares.
 */
#ifndef _HOSTCOMPAT_CRYPT_H_
#define _HOSTCOMPAT_CRYPT_H_

char *crypt(const char *key, const char *setting);

#endif /* _HOSTCOMPAT_CRYPT_H_ */
