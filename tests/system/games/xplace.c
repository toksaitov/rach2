/* xplace: map a fullscreen X window with Wine's hints and report its place. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrandr.h>

/* The fields of _MOTIF_WM_HINTS, five CARD32s, and the flags Wine sets. */
#define MWM_HINTS_FUNCTIONS   1
#define MWM_HINTS_DECORATIONS 2
#define MWM_FUNC_RESIZE       2
#define MWM_FUNC_MOVE         4

/* The most source devices whose raw motion is summed apart. */
#define MAX_SOURCES 16

/* The colours of the window's halves, in a 24-bit TrueColor visual. */
#define LEFT_PIXEL  0xff0000UL
#define RIGHT_PIXEL 0x00ff00UL

/** The raw relative motion one source device has sent. */
struct source {
    int id;       /* XInput device id of the slave */
    long dx, dy;  /* sums of its unaccelerated raw deltas, in device units */
    long events;  /* raw motion events that moved valuator 0 or 1 */
    int relative; /* nonzero when its first two valuators are relative */
    char name[64];
};

static struct source sources[MAX_SOURCES];
static int nsources;

/** Print a diagnostic and exit with status 1. */
static void die(const char *what)
{
    fprintf(stderr, "xplace: %s\n", what);
    exit(1);
}

/**
 * Return the record of source device ID, made on first sight from
 * XIQueryDevice, or NULL when MAX_SOURCES are taken.
 */
static struct source *source_of(Display *dpy, int id)
{
    XIDeviceInfo *info;
    struct source *s;
    int i, n;

    for (i = 0; i < nsources; i++)
        if (sources[i].id == id)
            return &sources[i];
    if (nsources == MAX_SOURCES)
        return NULL;
    s = &sources[nsources++];
    memset(s, 0, sizeof(*s));
    s->id = id;
    info = XIQueryDevice(dpy, id, &n);
    if (info != NULL) {
        snprintf(s->name, sizeof(s->name), "%s", info->name);
        for (i = 0; i < info->num_classes; i++) {
            XIValuatorClassInfo *v = (XIValuatorClassInfo *)info->classes[i];

            if (v->type == XIValuatorClass && v->number < 2 && v->mode == XIModeRelative)
                s->relative = 1;
        }
        XIFreeDeviceInfo(info);
    }
    return s;
}

/** Add the deltas of the raw motion event EV to its source's sums. */
static void add_raw(Display *dpy, const XIRawEvent *ev)
{
    struct source *s = source_of(dpy, ev->sourceid);
    const double *value = ev->raw_values;
    int bit, moved = 0;

    if (s == NULL)
        return;
    for (bit = 0; bit < ev->valuators.mask_len * 8; bit++) {
        if (!XIMaskIsSet(ev->valuators.mask, bit))
            continue;
        if (bit == 0)
            s->dx += (long)*value, moved = 1;
        else if (bit == 1)
            s->dy += (long)*value, moved = 1;
        value++;
    }
    s->events += moved;
}

/**
 * Give the client an emulated mode of WIDTH by HEIGHT on the first CRTC
 * that drives an output, as Wine changes the display mode; Xwayland offers
 * such modes on each output. Xwayland keeps the mode for this client alone
 * and goes on reporting the output's own mode on the CRTC, so the CRTC's size
 * is printed for the record only. Returns 1 when the request succeeds.
 */
static int set_mode(Display *dpy, Window root, int width, int height)
{
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
    XRROutputInfo *out = NULL;
    XRRCrtcInfo *crtc = NULL;
    RRMode mode = None;
    int i, j, ok = 0;

    if (res == NULL)
        return 0;
    for (i = 0; i < res->noutput && mode == None; i++) {
        out = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (out->crtc != None)
            for (j = 0; j < out->nmode && mode == None; j++) {
                int k;

                for (k = 0; k < res->nmode; k++)
                    if (res->modes[k].id == out->modes[j] && (int)res->modes[k].width == width &&
                        (int)res->modes[k].height == height)
                        mode = out->modes[j];
            }
        if (mode == None) {
            XRRFreeOutputInfo(out);
            out = NULL;
        } else
            break;
    }
    if (mode == None)
        fprintf(stderr, "xplace: no output offers %dx%d among %d modes\n", width, height,
                res->nmode);
    else {
        Status status;

        crtc = XRRGetCrtcInfo(dpy, res, out->crtc);
        status = XRRSetCrtcConfig(dpy, res, out->crtc, CurrentTime, crtc->x, crtc->y, mode,
                                  crtc->rotation, &res->outputs[i], 1);
        XRRFreeCrtcInfo(crtc);
        crtc = XRRGetCrtcInfo(dpy, res, out->crtc);
        ok = status == RRSetConfigSuccess;
        printf("xplace: set %dx%d: status %d, the CRTC reports %ux%u\n", width, height, (int)status,
               crtc->width, crtc->height);
        XRRFreeCrtcInfo(crtc);
        XRRFreeOutputInfo(out);
    }
    XRRFreeScreenResources(res);
    return ok;
}

/** Fill WIN, WIDTH by HEIGHT, with its left half red and right half green. */
static void draw(Display *dpy, Window win, GC gc, int width, int height)
{
    XSetForeground(dpy, gc, LEFT_PIXEL);
    XFillRectangle(dpy, win, gc, 0, 0, (unsigned)(width / 2), (unsigned)height);
    XSetForeground(dpy, gc, RIGHT_PIXEL);
    XFillRectangle(dpy, win, gc, width / 2, 0, (unsigned)(width - width / 2), (unsigned)height);
    XFlush(dpy);
}

/** Set the hints Wine 11's winex11 sets on a fullscreen WS_POPUP window. */
static void set_hints(Display *dpy, Window win)
{
    XSizeHints size;
    XWMHints wm;
    XClassHint klass;
    Atom protocols[2];
    long motif[5] = {MWM_HINTS_FUNCTIONS | MWM_HINTS_DECORATIONS, MWM_FUNC_MOVE | MWM_FUNC_RESIZE,
                     0, 0, 0};
    long pid = (long)getpid();

    /* A fullscreen window counts as resizable, so no PMinSize or PMaxSize. */
    memset(&size, 0, sizeof(size));
    size.flags = PPosition | PWinGravity;
    size.win_gravity = StaticGravity;
    XSetWMNormalHints(dpy, win, &size);
    /* Wine takes focus itself, when the manager sends WM_TAKE_FOCUS. */
    memset(&wm, 0, sizeof(wm));
    wm.flags = InputHint | StateHint;
    wm.input = False;
    wm.initial_state = NormalState;
    XSetWMHints(dpy, win, &wm);
    klass.res_name = "xplace";
    klass.res_class = "Xplace";
    XSetClassHint(dpy, win, &klass);
    XStoreName(dpy, win, "xplace");
    protocols[0] = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    protocols[1] = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
    XSetWMProtocols(dpy, win, protocols, 2);
    XChangeProperty(dpy, win, XInternAtom(dpy, "_MOTIF_WM_HINTS", False),
                    XInternAtom(dpy, "_MOTIF_WM_HINTS", False), 32, PropModeReplace,
                    (unsigned char *)motif, 5);
    XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_PID", False), XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&pid, 1);
}

/** Return 1 when the manager put _NET_WM_STATE_FULLSCREEN on WIN. */
static int wm_fullscreen(Display *dpy, Window win)
{
    Atom type, full = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    int format, found = 0;
    unsigned long n, after, i;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, win, XInternAtom(dpy, "_NET_WM_STATE", False), 0, 64, False,
                           XA_ATOM, &type, &format, &n, &after, &data) == Success &&
        data != NULL) {
        for (i = 0; i < n; i++)
            if (((Atom *)data)[i] == full)
                found = 1;
        XFree(data);
    }
    return found;
}

/**
 * Print the first rectangle of _XWAYLAND_RANDR_EMU_MONITOR_RECTS on WIN, the
 * emulated mode Xwayland scales the window from, or "none".
 */
static void emulated(Display *dpy, Window win)
{
    Atom rects = XInternAtom(dpy, "_XWAYLAND_RANDR_EMU_MONITOR_RECTS", False), type;
    unsigned long n = 0, after;
    unsigned char *data = NULL;
    int format;

    if (XGetWindowProperty(dpy, win, rects, 0, 4, False, XA_CARDINAL, &type, &format, &n, &after,
                           &data) == Success &&
        type == XA_CARDINAL && format == 32 && n == 4) {
        long *v = (long *)data;

        printf("xplace: emulated %ld,%ld %ldx%ld\n", v[0], v[1], v[2], v[3]);
    } else
        printf("xplace: emulated none\n");
    if (data != NULL)
        XFree(data);
}

/** Return WIN's _NET_WM_PID, or 0. */
static unsigned long window_pid(Display *dpy, Window win)
{
    Atom type;
    int format;
    unsigned long n, left, pid = 0;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, win, XInternAtom(dpy, "_NET_WM_PID", False), 0, 1, False,
                           XA_CARDINAL, &type, &format, &n, &left, &data) == Success &&
        data != NULL && format == 32 && n == 1)
        pid = *(unsigned long *)data;
    if (data != NULL)
        XFree(data);
    return pid;
}

/**
 * Print whether the input focus is WIN, a window inside it or another window
 * of WIN's process, which Wine moves the focus among, and name the window
 * that holds it when it is not WIN.
 */
static void report_focus(Display *dpy, Window win)
{
    Window focus, at, r, parent, *kids = NULL;
    unsigned int nkids;
    char *name = NULL;
    int revert;

    XGetInputFocus(dpy, &focus, &revert);
    for (at = focus; at != win && at != None && at != PointerRoot; at = parent) {
        if (!XQueryTree(dpy, at, &r, &parent, &kids, &nkids))
            break;
        if (kids != NULL)
            XFree(kids);
        if (parent == r)
            break;
    }
    if (at == win) {
        printf("xplace: focus yes\n");
        return;
    }
    if (focus != None && focus != PointerRoot)
        XFetchName(dpy, focus, &name);
    printf("xplace: focus %s: 0x%lx %s\n",
           focus != None && focus != PointerRoot && window_pid(dpy, win) != 0 &&
                   window_pid(dpy, focus) == window_pid(dpy, win)
               ? "yes, a window of the same process"
               : "no",
           (unsigned long)focus, name != NULL ? name : "");
    if (name != NULL)
        XFree(name);
}

/** Print where WIN sits on the root and in its manager's frame. */
static void report_place(Display *dpy, Window root, Window win)
{
    XWindowAttributes wa, fa;
    Window parent, child, *kids = NULL, r;
    unsigned int nkids;
    int x, y;

    XGetWindowAttributes(dpy, win, &wa);
    XTranslateCoordinates(dpy, win, root, 0, 0, &x, &y, &child);
    XQueryTree(dpy, win, &r, &parent, &kids, &nkids);
    if (kids != NULL)
        XFree(kids);
    printf("xplace: window at %d,%d size %dx%d\n", x, y, wa.width, wa.height);
    if (parent != root) {
        XGetWindowAttributes(dpy, parent, &fa);
        printf("xplace: frame at %d,%d size %dx%d child at %d,%d\n", fa.x, fa.y, fa.width,
               fa.height, wa.x, wa.y);
    } else
        printf("xplace: frame none\n");
    report_focus(dpy, win);
    printf("xplace: wm-fullscreen %s\n", wm_fullscreen(dpy, win) ? "yes" : "no");
    emulated(dpy, win);
}

/**
 * Return 1 while events still come: whether one is queued or arrives within
 * two seconds. Only -n uses it, where no seat may give focus, to see where
 * the manager left the window once it stopped configuring it.
 */
static int quiet(Display *dpy)
{
    struct timeval tv = {2, 0};
    fd_set fds;

    if (XPending(dpy))
        return 1;
    FD_ZERO(&fds);
    FD_SET(ConnectionNumber(dpy), &fds);
    return select(ConnectionNumber(dpy) + 1, &fds, NULL, NULL, &tv) > 0;
}

/**
 * Return the mapped client window named NAME among the root's children and
 * their children, the frames' clients, or None.
 */
static Window find_client(Display *dpy, Window root, const char *name)
{
    Window r, p, *top = NULL, *kids = NULL, found = None;
    unsigned int ntop, nkids, i, j;
    XWindowAttributes wa;
    char *title;

    if (!XQueryTree(dpy, root, &r, &p, &top, &ntop))
        return None;
    for (i = ntop; i-- > 0 && found == None;) {
        Window cand[64];
        unsigned int n = 0;

        cand[n++] = top[i];
        if (XQueryTree(dpy, top[i], &r, &p, &kids, &nkids)) {
            for (j = 0; j < nkids && n < 64; j++)
                cand[n++] = kids[j];
            if (kids != NULL)
                XFree(kids);
        }
        for (j = 0; j < n && found == None; j++) {
            if (!XGetWindowAttributes(dpy, cand[j], &wa) || wa.map_state != IsViewable)
                continue;
            if (XFetchName(dpy, cand[j], &title) && title != NULL) {
                if (strcmp(title, name) == 0)
                    found = cand[j];
                XFree(title);
            }
        }
    }
    if (top != NULL)
        XFree(top);
    return found;
}

/** Grab the pointer into WIN with a blank cursor, as a game does. */
static int grab(Display *dpy, Window win)
{
    static char empty[1];
    XColor black;
    Pixmap bits = XCreateBitmapFromData(dpy, win, empty, 1, 1);
    Cursor blank;

    memset(&black, 0, sizeof(black));
    blank = XCreatePixmapCursor(dpy, bits, bits, &black, &black, 0, 0);
    XDefineCursor(dpy, win, blank);
    return XGrabPointer(dpy, win, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                        GrabModeAsync, GrabModeAsync, win, blank, CurrentTime);
}

/**
 * Map WIN and wait until it is mapped and, unless NOWAIT, focused, answering
 * WM_TAKE_FOCUS; with NOWAIT, until the display has been quiet for a while.
 * Then take what the manager sent with its configuration and draw.
 */
static void map_window(Display *dpy, Window win, GC gc, int width, int height, int nowait)
{
    Atom take_focus = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
    int mapped = 0, focused = 0;
    XEvent ev;

    XMapWindow(dpy, win);
    while (!mapped || (!focused && !nowait) || (nowait && quiet(dpy))) {
        XNextEvent(dpy, &ev);
        if (ev.type == MapNotify && ev.xmap.window == win)
            mapped = 1;
        else if (ev.type == FocusIn && ev.xfocus.window == win)
            focused = 1;
        else if (ev.type == Expose && ev.xexpose.count == 0)
            draw(dpy, win, gc, width, height);
        else if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == take_focus)
            XSetInputFocus(dpy, win, RevertToParent, (Time)ev.xclient.data.l[1]);
    }
    /* The manager configures the window before it gives focus; take what
     * it sent with that configuration before reading the place. */
    XSync(dpy, False);
    while (XPending(dpy)) {
        XNextEvent(dpy, &ev);
        if (ev.type == Expose && ev.xexpose.count == 0)
            draw(dpy, win, gc, width, height);
    }
    draw(dpy, win, gc, width, height);
}

/**
 * Sum the raw motion of every master device's sources until a key is pressed
 * in WIN, then print the sums and where the pointer is. OPCODE is XInput's.
 */
static void count_motion(Display *dpy, Window win, GC gc, int width, int height, int opcode)
{
    Window root = DefaultRootWindow(dpy);
    unsigned char bits[XIMaskLen(XI_LASTEVENT)];
    XIEventMask mask;
    Window child;
    unsigned int buttons;
    int x, y, wx, wy, i, motions = 0, mx = 0, my = 0;
    XEvent ev;

    memset(bits, 0, sizeof(bits));
    XISetMask(bits, XI_RawMotion);
    mask.deviceid = XIAllMasterDevices;
    mask.mask_len = sizeof(bits);
    mask.mask = bits;
    if (XISelectEvents(dpy, root, &mask, 1) != Success)
        die("cannot select raw motion");
    XSync(dpy, False);
    printf("xplace: ready\n");
    for (;;) {
        XNextEvent(dpy, &ev);
        if (ev.type == GenericEvent && ev.xcookie.extension == opcode &&
            XGetEventData(dpy, &ev.xcookie)) {
            if (ev.xcookie.evtype == XI_RawMotion)
                add_raw(dpy, ev.xcookie.data);
            XFreeEventData(dpy, &ev.xcookie);
        } else if (ev.type == Expose && ev.xexpose.count == 0)
            draw(dpy, win, gc, width, height);
        else if (ev.type == MotionNotify) {
            if (motions++ == 0)
                printf("xplace: first core motion at %d,%d\n", ev.xmotion.x_root,
                       ev.xmotion.y_root);
            mx = ev.xmotion.x_root, my = ev.xmotion.y_root;
        } else if (ev.type == KeyPress) {
            printf("xplace: key %u\n", ev.xkey.keycode);
            break;
        }
    }
    printf("xplace: core motion events %d, the last at %d,%d\n", motions, mx, my);
    for (i = 0; i < nsources; i++)
        printf("xplace: raw %s dx %ld dy %ld events %ld %s\n", sources[i].name, sources[i].dx,
               sources[i].dy, sources[i].events, sources[i].relative ? "relative" : "absolute");
    XQueryPointer(dpy, win, &root, &child, &x, &y, &wx, &wy, &buttons);
    printf("xplace: pointer %d,%d in window %d,%d\n", x, y, wx, wy);
}

/**
 * Wait until the server has an enabled pointer device of the seat's, a slave
 * other than the XTEST one, print its name and return 0. Xwayland adds the
 * device when the compositor's seat gains a pointer, and Weston's seat gains
 * one only at a mouse's first event. Until then Xwayland has no wl_pointer,
 * asks for no confinement on a grab and hears no motion, so a grab taken
 * before the first move loses the moves that come before the device.
 */
static int wait_pointer(void)
{
    Display *dpy = XOpenDisplay(NULL);
    unsigned char bits[XIMaskLen(XI_LASTEVENT)];
    XIEventMask mask;
    XIDeviceInfo *info;
    XEvent ev;
    int opcode, event, error, major = 2, minor = 2, n, i, found = 0;

    if (dpy == NULL)
        die("cannot open the display");
    if (!XQueryExtension(dpy, "XInputExtension", &opcode, &event, &error) ||
        XIQueryVersion(dpy, &major, &minor) != Success)
        die("no XInput 2.2");
    memset(bits, 0, sizeof(bits));
    XISetMask(bits, XI_HierarchyChanged);
    mask.deviceid = XIAllDevices;
    mask.mask_len = sizeof(bits);
    mask.mask = bits;
    if (XISelectEvents(dpy, DefaultRootWindow(dpy), &mask, 1) != Success)
        die("cannot select hierarchy changes");
    for (;;) {
        info = XIQueryDevice(dpy, XIAllDevices, &n);
        for (i = 0; info != NULL && i < n && !found; i++)
            if (info[i].use == XISlavePointer && info[i].enabled &&
                strstr(info[i].name, "XTEST") == NULL) {
                printf("xplace: pointer device %s\n", info[i].name);
                found = 1;
            }
        if (info != NULL)
            XIFreeDeviceInfo(info);
        if (found)
            break;
        /* Each change of the device list wakes the loop to look again. */
        XNextEvent(dpy, &ev);
        if (XGetEventData(dpy, &ev.xcookie))
            XFreeEventData(dpy, &ev.xcookie);
    }
    XCloseDisplay(dpy);
    return 0;
}

/** Report the place of the mapped client window named NAME; return 0. */
static int query(const char *name)
{
    Display *dpy = XOpenDisplay(NULL);
    Window root, win;

    if (dpy == NULL)
        die("cannot open the display");
    root = DefaultRootWindow(dpy);
    if ((win = find_client(dpy, root, name)) == None)
        die("no mapped window of that name");
    report_place(dpy, root, win);
    return 0;
}

/**
 * Usage: xplace [-n] WIDTH HEIGHT, xplace -q NAME or xplace -p. -q reports
 * the place, frame and focus of another client's mapped window named NAME, a
 * Wine program's, and exits. -p waits until the server has the seat's pointer
 * device, names it and exits. Map a WIDTH by HEIGHT window at 0,0 with
 * the hints Wine sets on a fullscreen window and no _NET_WM_STATE, after an
 * emulated mode of that size when the screen's is another, and wait for
 * focus. Report its place, its frame, its focus and a confined pointer grab,
 * print "xplace: ready", and sum the raw motion each device sends until a
 * key is pressed. With -n it waits for the map and two seconds without events
 * instead of focus, and exits after the grab at the end of its standard
 * input. Exits 0, or 1 on a failure to
 * open the display, set the mode or select raw events.
 */
int main(int argc, char **argv)
{
    Display *dpy;
    Window root, win;
    XSetWindowAttributes attr;
    GC gc;
    int width, height, nowait = 0, opcode, event, error, major = 2, minor = 2;

    if (argc == 3 && strcmp(argv[1], "-q") == 0)
        return query(argv[2]);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 2 && strcmp(argv[1], "-p") == 0)
        return wait_pointer();
    if (argc > 1 && strcmp(argv[1], "-n") == 0)
        nowait = 1, argc--, argv++;
    if (argc != 3)
        die("usage: xplace [-n] WIDTH HEIGHT | xplace -q NAME | xplace -p");
    width = atoi(argv[1]);
    height = atoi(argv[2]);
    if ((dpy = XOpenDisplay(NULL)) == NULL)
        die("cannot open the display");
    root = DefaultRootWindow(dpy);
    printf("xplace: screen %dx%d\n", DisplayWidth(dpy, DefaultScreen(dpy)),
           DisplayHeight(dpy, DefaultScreen(dpy)));
    if (!XQueryExtension(dpy, "XInputExtension", &opcode, &event, &error) ||
        XIQueryVersion(dpy, &major, &minor) != Success)
        die("no XInput 2.2");
    if (width != DisplayWidth(dpy, DefaultScreen(dpy)) ||
        height != DisplayHeight(dpy, DefaultScreen(dpy))) {
        if (!set_mode(dpy, root, width, height))
            die("cannot set the emulated mode");
        printf("xplace: mode %dx%d\n", width, height);
    }
    attr.background_pixel = BlackPixel(dpy, DefaultScreen(dpy));
    attr.event_mask = ExposureMask | StructureNotifyMask | FocusChangeMask | KeyPressMask;
    win = XCreateWindow(dpy, root, 0, 0, (unsigned)width, (unsigned)height, 0, CopyFromParent,
                        InputOutput, CopyFromParent, CWBackPixel | CWEventMask, &attr);
    set_hints(dpy, win);
    gc = XCreateGC(dpy, win, 0, NULL);
    map_window(dpy, win, gc, width, height, nowait);
    report_place(dpy, root, win);
    printf("xplace: grab %d\n", grab(dpy, win));
    if (nowait) {
        /* The window stays while a caller reads the compositor's state. */
        while (getchar() != EOF)
            continue;
        return 0;
    }
    count_motion(dpy, win, gc, width, height, opcode);
    /* The manager may still move the frame after the focus arrives; the
     * place it holds by the key is the one that counts. */
    report_place(dpy, root, win);
    XCloseDisplay(dpy);
    return 0;
}
