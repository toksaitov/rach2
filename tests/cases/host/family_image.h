/* family_image.h: actual image publication under controlled process faults. */
#ifndef HOST_FAMILY_IMAGE_H
#define HOST_FAMILY_IMAGE_H
#include "family_check.h"
int family_image(struct fixture *f, const char *id);
int family_image_mock(int argc, char **argv);
int family_image_tree(struct fixture *f);
int family_image_make(struct fixture *f, const char *target);
int family_image_build(struct fixture *f, struct fx_child *p, const char *const env[]);
int family_image_left(struct fixture *f);
int family_image_old(const char *root);
int family_image_prerequisites(struct fixture *f);
const char *family_image_build_path(void);
int family_image_root_copy(struct fixture *f);
int family_image_root_tools(struct fixture *f);
int family_image_root_make(struct fixture *f, const char *target, const char *option);
int family_image_probe(struct fixture *f);
#endif
