/* installer_repair_fixture.h: disk edits in the prepared recovery helper. */
#ifndef INSTALLER_REPAIR_FIXTURE_H
#define INSTALLER_REPAIR_FIXTURE_H
#include <stddef.h>
#include <stdint.h>
struct repair_fixture {
    const char *disk, *name, *key;
    char work[4096], volume[4200];
    uint64_t boot, recovery;
};
const char *ir_path(struct repair_fixture *f, const char *name);
int ir_run(const char *const argv[]);
int ir_mtools(struct repair_fixture *f, const char *tool, const char *const args[]);
int ir_text(struct repair_fixture *f, const char *name, const char *text);
int ir_copy_contents(struct repair_fixture *f, const char *directory, const char *target);
int ir_root_keys(struct repair_fixture *f);
int ir_legacy(struct repair_fixture *f);
int ir_slot(struct repair_fixture *f);
int ir_keep(struct repair_fixture *f);
#endif
