/* fixture.h: filesystem and process fixtures in the prepared GNU guest. */
#ifndef HOST_FIXTURE_H
#define HOST_FIXTURE_H
#include <stddef.h>
#include <sys/types.h>
struct fixture {
    char root[4096], tree[4096], work[4096], bin[4096], self[4096];
    char *out, *err;
    int status, failed;
    int quiet; /* report a command's output by its size, not its text */
    unsigned command;
};
struct fx_child {
    pid_t pid;
    char out[4096], err[4096];
    int status, done;
};
int fx_init(struct fixture *f, const char *root, const char *name, const char *self);
void fx_destroy(struct fixture *f);
void fx_reap_groups(struct fixture *f);
int fx_external(const char *template, char *path, size_t size);
int fx_mkdir(const char *path);
int fx_write(const char *path, const void *data, size_t bytes, unsigned mode);
int fx_text(const char *path, const char *text);
char *fx_read(const char *path);
int fx_copy(const char *source, const char *target);
int fx_tree(const char *source, const char *target);
int fx_remove(const char *path);
int fx_exists(const char *path);
int fx_empty(const char *path);
int fx_count(const char *path);
int fx_same(const char *a, const char *b);
int fx_equal(const char *path, const char *text);
int fx_has(const char *path, const char *text);
int fx_regex(const char *text, const char *pattern);
int fx_replace(const char *source, const char *target, const char *from, const char *to);
void fx_env(const char *name, const char *value);
int fx_start(struct fixture *f, struct fx_child *p, const char *const argv[], const char *cwd,
             const char *const env[], const char *input);
struct fx_io {
    const char *input, *output;
};
int fx_start_io(struct fixture *f, struct fx_child *p, const char *const argv[], const char *cwd,
                const char *const env[], const struct fx_io *io);
int fx_poll(struct fx_child *p);
int fx_wait(struct fixture *f, struct fx_child *p);
void fx_stop(struct fx_child *p, int signal);
int fx_run(struct fixture *f, const char *const argv[], const char *cwd, const char *const env[],
           const char *input);
int fx_sh(struct fixture *f, const char *command);
int fx_mock(struct fixture *f, const char *name);
int fx_until(struct fx_child *p, const char *path);
void fx_pause(void);
int fx_check(struct fixture *f, int held, const char *label);
#endif
