/* host_image.h: native controllers of image and installer product checks. */
#ifndef HOST_IMAGE_H
#define HOST_IMAGE_H
#include "../machine/netcase.h"

/** Run ASSERTION using the already booted products helper. The caller reports
 * the assertion. Guest scratch is removed before return; evidence stays. */
int host_image_check(struct etude_case *c, struct etude_case_machine *products,
                     const char *assertion);

/* The state and operations shared by the five controllers. */
struct image_check {
    struct net_case u;
    struct etude_case_machine *guest;
    const char *id;
    char dir[160];
    unsigned commands, inputs, failed;
};
char *image_command(struct image_check *t, const char *command, int *status);
int image_expect(struct image_check *t, const char *command, const char *expected);
int image_put(struct image_check *t, const char *relative, const void *bytes, size_t size,
              const char *mode);
int image_text(struct image_check *t, const char *relative, const char *text);
int image_source(struct image_check *t, const char *source, const char *relative);

int image_remove_tree(const char *path);
void image_row(struct image_check *t, const char *row, int held);
int image_accounts(struct image_check *t);
int image_krffs(struct image_check *t);
int image_overlay(struct image_check *t);
int image_tooling(struct image_check *t);
int image_collection(struct image_check *t);
#endif
