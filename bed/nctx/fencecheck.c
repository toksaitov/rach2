/* Exercise the backend's asynchronous fence callbacks with renderer threads. */
#include "../vm/backend/virgl.c"

static struct vr renderer;
static pthread_t serving_thread;
static int completions, wrong_thread, create_error, immediate;

/** Count completions, which must run on the serving thread alone. */
void vd_fence_retired(struct vd *d, uint32_t ctx, int per_ctx, uint32_t ring, uint64_t id)
{
    (void)d;
    (void)ctx;
    (void)per_ctx;
    (void)ring;
    (void)id;
    completions++;
    wrong_thread |= !pthread_equal(pthread_self(), serving_thread);
}

/** Stand in for a renderer whose callbacks come from another thread. */
void virgl_renderer_poll(void)
{}

/** Inject a rejected fence or a callback before creation returns. */
int virgl_renderer_context_create_fence(uint32_t ctx, uint32_t flags, uint32_t ring, uint64_t id)
{
    (void)flags;
    if (immediate && !create_error) {
        write_context_fence(&renderer, ctx, ring, id);
    }
    return create_error;
}

/** Accept the global timeline's fence. */
int virgl_renderer_create_fence(int id, uint32_t ctx)
{
    (void)id;
    (void)ctx;
    return create_error;
}

/** Retire one context ring and the global timeline across its wrap. */
static void *render_thread(void *unused)
{
    (void)unused;
    write_context_fence(&renderer, 7, 2, 9);
    write_fence(&renderer, 1);
    return NULL;
}

/** Report a failed condition without continuing a damaged fixture. */
static void require(int condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "fencecheck: failed: %s\n", what);
        exit(1);
    }
}

/** Check callback ownership, timeline isolation, failed creation and reuse. */
int main(void)
{
    pthread_t worker;
    unsigned i;

    serving_thread = pthread_self();
    require(!pthread_mutex_init(&renderer.fence_lock, NULL), "mutex creation");
    renderer.fence_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    require(renderer.fence_fd >= 0, "eventfd creation");
    require(!op_fence(&renderer, 7, 1, 2, 8), "first ring fence");
    require(!op_fence(&renderer, 7, 1, 2, 9), "second ring fence");
    require(!op_fence(&renderer, 7, 1, 3, 8), "another ring");
    require(!op_fence(&renderer, 8, 1, 2, 8), "another context");
    require(!op_fence(&renderer, 0, 0, 0, 0xffffffffu), "before wrap");
    require(!op_fence(&renderer, 0, 0, 0, 1), "after wrap");
    require(!pthread_create(&worker, NULL, render_thread, NULL), "renderer thread");
    require(!pthread_join(worker, NULL), "renderer thread join");
    require(completions == 0, "callbacks must not complete device commands");
    vr_poll(&renderer);
    require(completions == 4 && !wrong_thread, "only the retired timelines complete");
    write_context_fence(&renderer, 7, 3, 8);
    write_context_fence(&renderer, 8, 2, 8);
    vr_poll(&renderer);
    require(completions == 6, "the other ring and context complete separately");
    create_error = EINVAL;
    for (i = 0; i < VD_PENDING + 1; i++) {
        require(op_fence(&renderer, 7, 1, 2, i) == EINVAL, "rejected creation frees its slot");
    }
    create_error = 0;
    immediate = 1;
    for (i = 0; i < VD_PENDING; i++) {
        require(!op_fence(&renderer, 7, 1, 2, i), "immediate callback keeps its slot until poll");
    }
    require(completions == 6, "immediate callbacks wait for poll too");
    vr_poll(&renderer);
    require(completions == 6 + VD_PENDING && !wrong_thread, "all pending commands complete");
    require(!op_fence(&renderer, 7, 1, 2, VD_PENDING), "completed slots are reusable");
    vr_poll(&renderer);
    close(renderer.fence_fd);
    pthread_mutex_destroy(&renderer.fence_lock);
    puts("fencecheck: result: pass");
    return 0;
}
