/*
 * latency.c: the time of one round trip through a GL driver, a clear of a
 * small framebuffer and the read of one pixel back, which waits until the
 * driver's whole path, the VM or the host's GPU among it, has run the
 * clear. It links only libc and libdl, as bed/fixture/triangle.c does, so
 * that it builds in Rach2's GNU root, which has no development headers.
 */

#define _POSIX_C_SOURCE 200809L

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef int32_t EGLint;
typedef unsigned EGLBoolean;
typedef unsigned EGLenum;

typedef unsigned GLuint;
typedef int GLint;
typedef unsigned GLenum;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned GLbitfield;
typedef unsigned char GLubyte;

/* The values of Khronos's registry (egl.h, eglext.h, gl2.h). */
#define EGL_NO_DISPLAY                ((EGLDisplay)0)
#define EGL_DEFAULT_DISPLAY           ((void *)0)
#define EGL_NO_CONFIG_KHR             ((EGLConfig)0)
#define EGL_NO_CONTEXT                ((EGLContext)0)
#define EGL_NO_SURFACE                ((EGLSurface)0)
#define EGL_NONE                      0x3038
#define EGL_CONTEXT_CLIENT_VERSION    0x3098
#define EGL_OPENGL_ES_API             0x30A0
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD

#define GL_COLOR_BUFFER_BIT     0x4000
#define GL_TEXTURE_2D           0x0DE1
#define GL_RENDERER             0x1F01
#define GL_UNSIGNED_BYTE        0x1401
#define GL_RGBA                 0x1908
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0    0x8CE0
#define GL_FRAMEBUFFER          0x8D40

/* Each entry point the program calls: its type, name and parameters. */
#define ENTRY_POINTS(X)                                                                  \
    X(EGLDisplay, eglGetPlatformDisplayEXT, (EGLenum, void *, const EGLint *))           \
    X(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *))                       \
    X(EGLBoolean, eglBindAPI, (EGLenum))                                                 \
    X(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *)) \
    X(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))      \
    X(EGLBoolean, eglTerminate, (EGLDisplay))                                            \
    X(const GLubyte *, glGetString, (GLenum))                                            \
    X(void, glGenTextures, (GLsizei, GLuint *))                                          \
    X(void, glBindTexture, (GLenum, GLuint))                                             \
    X(void, glTexImage2D,                                                                \
      (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))     \
    X(void, glGenFramebuffers, (GLsizei, GLuint *))                                      \
    X(void, glBindFramebuffer, (GLenum, GLuint))                                         \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))             \
    X(GLenum, glCheckFramebufferStatus, (GLenum))                                        \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                          \
    X(void, glClear, (GLbitfield))                                                       \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))

#define POINTER(type, name, params) static type(*name) params;
ENTRY_POINTS(POINTER)
#undef POINTER

/* The framebuffer's side, small so that the clear costs little beside the
 * round trip. */
#define SIDE 64

/* Round trips before those timed, which pay for the first compiles and
 * allocations. */
#define WARMUP 5

/** Print MSG as the program's failure and exit with status 1. */
static void die(const char *msg)
{
    fprintf(stderr, "latency: error: %s\n", msg);
    exit(1);
}

/** Resolve every entry point of ENTRY_POINTS from libEGL.so.1 by its
 * eglGetProcAddress; exit on a missing one. */
static void load(void)
{
    void *(*get)(const char *);
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);

    if (egl == NULL) {
        fprintf(stderr, "latency: libEGL.so.1: %s\n", dlerror());
        die("EGL did not load");
    }
    *(void **)&get = dlsym(egl, "eglGetProcAddress");
    if (get == NULL)
        die("libEGL.so.1 has no eglGetProcAddress");
#define RESOLVE(type, name, params)                       \
    *(void **)&name = get(#name);                         \
    if (name == NULL) {                                   \
        fprintf(stderr, "latency: missing: %s\n", #name); \
        die("an entry point is missing");                 \
    }
    ENTRY_POINTS(RESOLVE)
#undef RESOLVE
}

/** The monotonic clock in milliseconds. */
static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/** qsort's order of doubles. */
static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;

    return x < y ? -1 : x > y;
}

/**
 * Time N round trips on the device EGL's surfaceless platform finds, or on
 * llvmpipe when LIBGL_ALWAYS_SOFTWARE is set, and print one line:
 * `latency: N round trips, median X ms, p99 Y ms, renderer R`.
 */
int main(int argc, char **argv)
{
    static const EGLint attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    int n = argc > 1 ? atoi(argv[1]) : 200, i;
    unsigned char px[4];
    double *t, start;
    EGLDisplay dpy;
    EGLContext ctx;
    GLuint tex, fb;

    if (n < 1 || n > 100000)
        die("usage: latency [N], N from 1 to 100000");
    t = malloc(sizeof(*t) * (size_t)n);
    if (t == NULL)
        die("no memory");
    load();
    dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL))
        die("the surfaceless display did not open");
    if (!eglBindAPI(EGL_OPENGL_ES_API))
        die("no OpenGL ES");
    ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attribs);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
        die("no ES 2 context");
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SIDE, SIDE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        die("the framebuffer is incomplete");
    glViewport(0, 0, SIDE, SIDE);
    for (i = -WARMUP; i < n; i++) {
        start = now_ms();
        glClearColor((float)(i & 1), 0.5f, 0.25f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if (px[0] != ((i & 1) ? 255 : 0))
            die("the pixel read back is not the clear's");
        if (i >= 0)
            t[i] = now_ms() - start;
    }
    qsort(t, (size_t)n, sizeof(*t), cmp);
    printf("latency: %d round trips, median %.3f ms, p99 %.3f ms, renderer %s\n", n, t[n / 2],
           t[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1], (const char *)glGetString(GL_RENDERER));
    eglTerminate(dpy);
    free(t);
    return 0;
}
