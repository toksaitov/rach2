/* Give the private headless compositor a keyboard and pointer seat. */
#include <stdlib.h>
#include <libweston/libweston.h>

/* Weston 14 exports these backend entry points from libweston-14. */
void weston_seat_init(struct weston_seat *, struct weston_compositor *, const char *);
void weston_seat_release(struct weston_seat *);
int weston_seat_init_pointer(struct weston_seat *);
int weston_seat_init_keyboard(struct weston_seat *, struct xkb_keymap *);

struct acceptance_seat {
    struct weston_seat seat;
    struct wl_listener destroy;
};

/** Release the test seat when its compositor is destroyed. */
static void destroy(struct wl_listener *listener, void *data)
{
    struct acceptance_seat *s = wl_container_of(listener, s, destroy);

    (void)data;
    wl_list_remove(&s->destroy.link);
    weston_seat_release(&s->seat);
    free(s);
}

/**
 * Add an input seat to the private Weston 14 compositor.
 * Return -1 on allocation or keymap failure and 0 after registering cleanup.
 */
WL_EXPORT int wet_module_init(struct weston_compositor *compositor, int *argc, char *argv[])
{
    struct acceptance_seat *s = calloc(1, sizeof(*s));

    (void)argc;
    (void)argv;
    if (s == NULL)
        return -1;
    weston_seat_init(&s->seat, compositor, "acceptance");
    if (weston_seat_init_pointer(&s->seat) < 0 || weston_seat_init_keyboard(&s->seat, NULL) < 0) {
        weston_seat_release(&s->seat);
        free(s);
        return -1;
    }
    s->destroy.notify = destroy;
    wl_signal_add(&compositor->destroy_signal, &s->destroy);
    return 0;
}
