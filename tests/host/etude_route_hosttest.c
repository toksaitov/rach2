/**
 * Check the fixed native route actions of tools/etude/routine_route.c with
 * tools/ci/fixtures/routine_route_actions_test.c, which replaces the path
 * and remote transport functions the route calls and needs no endpoint or
 * authority.
 *
 * host-deps: tools/etude/authority_parse.c tools/etude/authority_run.c
 * host-deps: tools/etude/boot_identity.c tools/etude/capture.c
 * host-deps: tools/etude/collection_tools.c tools/etude/guest/wire.c
 * host-deps: tools/etude/history.c tools/etude/history_collect.c
 * host-deps: tools/etude/history_json.c tools/etude/host_prepare_build.c
 * host-deps: tools/etude/host_prepare_io.c tools/etude/json.c
 * host-deps: tools/etude/measurement_context.c
 * host-deps: tools/etude/measurement_prepare.c tools/etude/observe_import.c
 * host-deps: tools/etude/observe_sequence.c
 * host-deps: tools/etude/posix/authority_platform.c tools/etude/posix/exec.c
 * host-deps: tools/etude/posix/keeper.c tools/etude/posix/observe_sequence.c
 * host-deps: tools/etude/posix/process.c tools/etude/posix/remote.c
 * host-deps: tools/etude/posix/store.c tools/etude/preparation_import_data.c
 * host-deps: tools/etude/preparation_record.c tools/etude/retention.c
 * host-deps: tools/etude/retention_io.c tools/etude/retention_issues.c
 * host-deps: tools/etude/retention_proof.c tools/etude/routine_charge.c
 * host-deps: tools/etude/routine_direct.c tools/etude/routine_evidence.c
 * host-deps: tools/etude/routine_inputs.c tools/etude/routine_proof.c
 * host-deps: tools/etude/routine_remote.c tools/etude/routine_site.c
 * host-deps: tools/etude/tooling.c tools/etude/tooling_cache.c
 * host-deps: utils/sha256.c
 * host-cflags: -I tools/etude -I tools/etude/include -I tools/etude/posix
 * host-cflags: -I tests/host/compat -I utils -Wall -Wextra -Werror
 */

/* The fixture's replacements take other names than the linked sources'. */
#define routine_path             fixture_routine_path
#define etude_routine_remote_put fixture_remote_put
#define etude_routine_remote_get fixture_remote_get
#include "../../tools/ci/fixtures/routine_route_actions_test.c"
