/* Keep the existing listener regression in the default native inventory. */
#define fix_dir    listener_fix_dir
#define fix_path   listener_fix_path
#define fix_passed listener_fix_passed
#define fix_format listener_fix_format
#define main       etude_listener_fixture_main
#include "../../../tools/ci/fixtures/routine_listener_test.c"
#undef main

int etude_test_routine_listener(void)
{
    char *argv[] = {"routine-listener", NULL};
    return etude_listener_fixture_main(1, argv);
}
