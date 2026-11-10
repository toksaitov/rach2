/* Stage the GNU products helper's changing files from its raw products disk. */
#ifndef HOST_ASSETS_H
#define HOST_ASSETS_H
#include "etude/scenario.h"
/**
 * Unpack the products archive, gnu-test-products.tar on the helper's raw
 * disk1, and the GNU-built products, gnu-host-products.tar on disk2, into
 * /host-products, and check the source archive staged there against the
 * case's declared artifact `scripts-assets`.
 *
 * The first archive holds the source archive and the architecture's two
 * kernel profiles (tests/system/suites.mk), the second the cases' controllers
 * and tools (mk/gnu-image.mk). mk keeps both off gnu-test.disk so that a
 * change to them does not remake the image. It runs as the helper's root
 * before any reader of those files.
 *
 * @return 1 when the archive was unpacked and its source archive has the
 * declared artifact's SHA-256, 0 otherwise.
 */
int host_assets_stage(struct etude_case *c, struct etude_case_machine *m);
#endif
