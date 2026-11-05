#ifndef EXT2_HOST_FIXTURE_H
#define EXT2_HOST_FIXTURE_H
int ext2_fixture_tree(const char *root);
int ext2_fixture_image(const char *image, int block, int inode, const char *features,
                       unsigned long mib, const char *tree);
int ext2_fixture_image_groups(const char *image, int block, int inode, const char *features,
                              unsigned long mib, const char *tree, unsigned long per_group);
int ext2_fixture_debugfs(const char *image, const char *request, int write, const char *output);
int ext2_fixture_fsck(const char *image, const char *flags, const char *output);
int ext2_fixture_capture_child(void);
int ext2_fixture_capture_test(const char *self, const char *root);
int ext2_fixture_findings(const char *input, const char *output);
int ext2_fixture_contains(const char *path, const char *text);
void ext2_fixture_head(const char *path, unsigned lines);
int ext2_fixture_noise(const char *path, unsigned long bytes);
int ext2_fixture_line(const char *image, const char *tool, const char *request, const char *match,
                      int after, const char *output);
#endif
