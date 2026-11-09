/*
 * nctxcheck: the native-context checks of the bed's Linux reference, run in
 * a guest of the bed's QEMU on GL ES 3 through EGL's surfaceless platform.
 * It links only libc and libdl; EGL and GL come from libEGL.so.1 by
 * eglGetProcAddress, so the guest needs no development headers.
 *
 *   nctxcheck info               the EGL and GL strings and the device
 *   nctxcheck triangle           an orange triangle over navy, pixels read
 *   nctxcheck pressure MIB       textures of 16 MiB up to MIB, each drawn
 *                                into, then read back in reverse order
 *   nctxcheck submit N           N draws, each flushed alone, fenced every
 *                                64, the last colour read back
 *   nctxcheck fence              GL syncs in order, a native fence fd and a
 *                                server wait on it
 *   nctxcheck hang LOOPS         a robust context's draw that loops LOOPS
 *                                times a pixel, then its reset status and a
 *                                triangle in a new context
 *
 * Each prints `nctxcheck: NAME: ...` lines and a last line
 * `nctxcheck: NAME: result: pass` or `... fail: WHY`; the exit status is 0
 * on a pass, 1 on a failure and 2 on bad usage.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLSync;
typedef void *EGLDeviceEXT;
typedef int32_t EGLint;
typedef unsigned EGLBoolean;
typedef unsigned EGLenum;
typedef intptr_t EGLAttrib;
typedef uint64_t EGLTime;

typedef unsigned GLuint;
typedef int GLint;
typedef unsigned GLenum;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLubyte;
typedef unsigned GLbitfield;
typedef intptr_t GLsizeiptr;
typedef void *GLsync;
typedef uint64_t GLuint64;

#define EGL_NONE                                       0x3038
#define EGL_SUCCESS                                    0x3000
#define EGL_VENDOR                                     0x3053
#define EGL_VERSION                                    0x3054
#define EGL_EXTENSIONS                                 0x3055
#define EGL_OPENGL_ES_API                              0x30A0
#define EGL_CONTEXT_MAJOR_VERSION                      0x3098
#define EGL_CONTEXT_MINOR_VERSION                      0x30FB
#define EGL_CONTEXT_OPENGL_ROBUST_ACCESS               0x31B2
#define EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY 0x31BD
#define EGL_LOSE_CONTEXT_ON_RESET                      0x31BF
#define EGL_PLATFORM_SURFACELESS_MESA                  0x31DD
#define EGL_DEVICE_EXT                                 0x322C
#define EGL_DRM_DEVICE_FILE_EXT                        0x3233
#define EGL_DRM_RENDER_NODE_FILE_EXT                   0x3377
#define EGL_SYNC_NATIVE_FENCE_ANDROID                  0x3144
#define EGL_SYNC_NATIVE_FENCE_FD_ANDROID               0x3145
#define EGL_SYNC_FLUSH_COMMANDS_BIT                    0x0001
#define EGL_CONDITION_SATISFIED                        0x30F6
#define EGL_FOREVER                                    0xFFFFFFFFFFFFFFFFull

#define GL_NO_ERROR                   0
#define GL_VENDOR                     0x1F00
#define GL_RENDERER                   0x1F01
#define GL_VERSION                    0x1F02
#define GL_TRIANGLES                  0x0004
#define GL_TRIANGLE_STRIP             0x0005
#define GL_UNSIGNED_BYTE              0x1401
#define GL_FLOAT                      0x1406
#define GL_RGBA                       0x1908
#define GL_RGBA8                      0x8058
#define GL_TEXTURE_2D                 0x0DE1
#define GL_SCISSOR_TEST               0x0C11
#define GL_COLOR_BUFFER_BIT           0x4000
#define GL_FRAMEBUFFER                0x8D40
#define GL_FRAMEBUFFER_COMPLETE       0x8CD5
#define GL_COLOR_ATTACHMENT0          0x8CE0
#define GL_ARRAY_BUFFER               0x8892
#define GL_STATIC_DRAW                0x88E4
#define GL_FRAGMENT_SHADER            0x8B30
#define GL_VERTEX_SHADER              0x8B31
#define GL_COMPILE_STATUS             0x8B81
#define GL_LINK_STATUS                0x8B82
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT    0x0001
#define GL_ALREADY_SIGNALED           0x911A
#define GL_TIMEOUT_EXPIRED            0x911B
#define GL_CONDITION_SATISFIED        0x911C
#define GL_WAIT_FAILED                0x911D
#define GL_GUILTY_CONTEXT_RESET       0x8253
#define GL_INNOCENT_CONTEXT_RESET     0x8254
#define GL_UNKNOWN_CONTEXT_RESET      0x8255

static void *(*eglGetProcAddress_)(const char *);
static EGLDisplay (*eglGetPlatformDisplay_)(EGLenum, void *, const EGLAttrib *);
static EGLBoolean (*eglInitialize_)(EGLDisplay, EGLint *, EGLint *);
static const char *(*eglQueryString_)(EGLDisplay, EGLint);
static EGLBoolean (*eglBindAPI_)(EGLenum);
static EGLContext (*eglCreateContext_)(EGLDisplay, void *, EGLContext, const EGLint *);
static EGLBoolean (*eglMakeCurrent_)(EGLDisplay, void *, void *, EGLContext);
static EGLBoolean (*eglDestroyContext_)(EGLDisplay, EGLContext);
static EGLint (*eglGetError_)(void);
static EGLBoolean (*eglQueryDisplayAttribEXT_)(EGLDisplay, EGLint, EGLAttrib *);
static const char *(*eglQueryDeviceStringEXT_)(EGLDeviceEXT, EGLint);
static EGLSync (*eglCreateSync_)(EGLDisplay, EGLenum, const EGLAttrib *);
static EGLint (*eglClientWaitSync_)(EGLDisplay, EGLSync, EGLint, EGLTime);
static EGLBoolean (*eglWaitSync_)(EGLDisplay, EGLSync, EGLint);
static EGLBoolean (*eglDestroySync_)(EGLDisplay, EGLSync);
static EGLint (*eglDupNativeFenceFDANDROID_)(EGLDisplay, EGLSync);

static const GLubyte *(*glGetString_)(GLenum);
static GLenum (*glGetError_)(void);
static void (*glViewport_)(GLint, GLint, GLsizei, GLsizei);
static void (*glClearColor_)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*glClear_)(GLbitfield);
static void (*glEnable_)(GLenum);
static void (*glDisable_)(GLenum);
static void (*glScissor_)(GLint, GLint, GLsizei, GLsizei);
static void (*glFlush_)(void);
static void (*glFinish_)(void);
static void (*glReadPixels_)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void (*glGenTextures_)(GLsizei, GLuint *);
static void (*glDeleteTextures_)(GLsizei, const GLuint *);
static void (*glBindTexture_)(GLenum, GLuint);
static void (*glTexStorage2D_)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static void (*glTexSubImage2D_)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                const void *);
static void (*glGenFramebuffers_)(GLsizei, GLuint *);
static void (*glDeleteFramebuffers_)(GLsizei, const GLuint *);
static void (*glBindFramebuffer_)(GLenum, GLuint);
static void (*glFramebufferTexture2D_)(GLenum, GLenum, GLenum, GLuint, GLint);
static GLenum (*glCheckFramebufferStatus_)(GLenum);
static void (*glGenBuffers_)(GLsizei, GLuint *);
static void (*glBindBuffer_)(GLenum, GLuint);
static void (*glBufferData_)(GLenum, GLsizeiptr, const void *, GLenum);
static void (*glVertexAttribPointer_)(GLuint, GLint, GLenum, GLubyte, GLsizei, const void *);
static void (*glEnableVertexAttribArray_)(GLuint);
static void (*glDrawArrays_)(GLenum, GLint, GLsizei);
static GLuint (*glCreateShader_)(GLenum);
static void (*glShaderSource_)(GLuint, GLsizei, const char *const *, const GLint *);
static void (*glCompileShader_)(GLuint);
static void (*glGetShaderiv_)(GLuint, GLenum, GLint *);
static void (*glGetShaderInfoLog_)(GLuint, GLsizei, GLsizei *, char *);
static GLuint (*glCreateProgram_)(void);
static void (*glAttachShader_)(GLuint, GLuint);
static void (*glBindAttribLocation_)(GLuint, GLuint, const char *);
static void (*glLinkProgram_)(GLuint);
static void (*glGetProgramiv_)(GLuint, GLenum, GLint *);
static void (*glUseProgram_)(GLuint);
static GLint (*glGetUniformLocation_)(GLuint, const char *);
static void (*glUniform4f_)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
static void (*glUniform1i_)(GLint, GLint);
static GLsync (*glFenceSync_)(GLenum, GLbitfield);
static GLenum (*glClientWaitSync_)(GLsync, GLbitfield, GLuint64);
static void (*glDeleteSync_)(GLsync);
static GLenum (*glGetGraphicsResetStatus_)(void);

static const char *check = "start";
static EGLDisplay dpy;

/** fail - print the check's failure with WHY and return 1. */
static int fail(const char *why)
{
    printf("nctxcheck: %s: result: fail: %s\n", check, why);
    return 1;
}

/** pass - print the check's pass and return 0. */
static int pass(void)
{
    printf("nctxcheck: %s: result: pass\n", check);
    return 0;
}

/** now - the monotonic clock in seconds. */
static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/**
 * load - resolve one entry point by eglGetProcAddress.
 *
 * Mesa answers every core EGL and GL function through it
 * (EGL_KHR_get_all_proc_addresses). Returns NULL when the name is unknown;
 * the caller decides whether that is fatal.
 */
static void *load(const char *name)
{
    return eglGetProcAddress_(name);
}

#define NEED(var, name)                                        \
    do {                                                       \
        *(void **)&var = load(name);                           \
        if (!var) {                                            \
            fprintf(stderr, "nctxcheck: missing: %s\n", name); \
            return -1;                                         \
        }                                                      \
    } while (0)

/** resolve - load every entry point the checks use; 0, or -1 when one lacks. */
static int resolve(void)
{
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);

    if (!egl) {
        fprintf(stderr, "nctxcheck: libEGL.so.1: %s\n", dlerror());
        return -1;
    }
    *(void **)&eglGetProcAddress_ = dlsym(egl, "eglGetProcAddress");
    if (!eglGetProcAddress_)
        return -1;
    NEED(eglGetPlatformDisplay_, "eglGetPlatformDisplay");
    NEED(eglInitialize_, "eglInitialize");
    NEED(eglQueryString_, "eglQueryString");
    NEED(eglBindAPI_, "eglBindAPI");
    NEED(eglCreateContext_, "eglCreateContext");
    NEED(eglMakeCurrent_, "eglMakeCurrent");
    NEED(eglDestroyContext_, "eglDestroyContext");
    NEED(eglGetError_, "eglGetError");
    NEED(eglCreateSync_, "eglCreateSync");
    NEED(eglClientWaitSync_, "eglClientWaitSync");
    NEED(eglWaitSync_, "eglWaitSync");
    NEED(eglDestroySync_, "eglDestroySync");
    /* Extensions: absent ones are reported by the check that needs them. */
    *(void **)&eglQueryDisplayAttribEXT_ = load("eglQueryDisplayAttribEXT");
    *(void **)&eglQueryDeviceStringEXT_ = load("eglQueryDeviceStringEXT");
    *(void **)&eglDupNativeFenceFDANDROID_ = load("eglDupNativeFenceFDANDROID");
    NEED(glGetString_, "glGetString");
    NEED(glGetError_, "glGetError");
    NEED(glViewport_, "glViewport");
    NEED(glClearColor_, "glClearColor");
    NEED(glClear_, "glClear");
    NEED(glEnable_, "glEnable");
    NEED(glDisable_, "glDisable");
    NEED(glScissor_, "glScissor");
    NEED(glFlush_, "glFlush");
    NEED(glFinish_, "glFinish");
    NEED(glReadPixels_, "glReadPixels");
    NEED(glGenTextures_, "glGenTextures");
    NEED(glDeleteTextures_, "glDeleteTextures");
    NEED(glBindTexture_, "glBindTexture");
    NEED(glTexStorage2D_, "glTexStorage2D");
    NEED(glTexSubImage2D_, "glTexSubImage2D");
    NEED(glGenFramebuffers_, "glGenFramebuffers");
    NEED(glDeleteFramebuffers_, "glDeleteFramebuffers");
    NEED(glBindFramebuffer_, "glBindFramebuffer");
    NEED(glFramebufferTexture2D_, "glFramebufferTexture2D");
    NEED(glCheckFramebufferStatus_, "glCheckFramebufferStatus");
    NEED(glGenBuffers_, "glGenBuffers");
    NEED(glBindBuffer_, "glBindBuffer");
    NEED(glBufferData_, "glBufferData");
    NEED(glVertexAttribPointer_, "glVertexAttribPointer");
    NEED(glEnableVertexAttribArray_, "glEnableVertexAttribArray");
    NEED(glDrawArrays_, "glDrawArrays");
    NEED(glCreateShader_, "glCreateShader");
    NEED(glShaderSource_, "glShaderSource");
    NEED(glCompileShader_, "glCompileShader");
    NEED(glGetShaderiv_, "glGetShaderiv");
    NEED(glGetShaderInfoLog_, "glGetShaderInfoLog");
    NEED(glCreateProgram_, "glCreateProgram");
    NEED(glAttachShader_, "glAttachShader");
    NEED(glBindAttribLocation_, "glBindAttribLocation");
    NEED(glLinkProgram_, "glLinkProgram");
    NEED(glGetProgramiv_, "glGetProgramiv");
    NEED(glUseProgram_, "glUseProgram");
    NEED(glGetUniformLocation_, "glGetUniformLocation");
    NEED(glUniform4f_, "glUniform4f");
    NEED(glUniform1i_, "glUniform1i");
    NEED(glFenceSync_, "glFenceSync");
    NEED(glClientWaitSync_, "glClientWaitSync");
    NEED(glDeleteSync_, "glDeleteSync");
    *(void **)&glGetGraphicsResetStatus_ = load("glGetGraphicsResetStatusKHR");
    if (!glGetGraphicsResetStatus_)
        *(void **)&glGetGraphicsResetStatus_ = load("glGetGraphicsResetStatusEXT");
    return 0;
}

/** display - initialize the surfaceless display once; 0, or -1. */
static int display(void)
{
    EGLint major, minor;

    if (dpy)
        return 0;
    dpy = eglGetPlatformDisplay_(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
    if (!dpy || !eglInitialize_(dpy, &major, &minor)) {
        fprintf(stderr, "nctxcheck: no surfaceless display: egl error 0x%x\n", eglGetError_());
        dpy = NULL;
        return -1;
    }
    eglBindAPI_(EGL_OPENGL_ES_API);
    return 0;
}

/**
 * context - a GL ES 3.0 context made current with no surface.
 *
 * @robust: ask for robust access with loss of the context on a reset, as a
 * program that recovers from a GPU fault does. Returns NULL on failure.
 */
static EGLContext context(int robust)
{
    EGLint plain[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    EGLint strong[] = {EGL_CONTEXT_MAJOR_VERSION,
                       3,
                       EGL_CONTEXT_MINOR_VERSION,
                       0,
                       EGL_CONTEXT_OPENGL_ROBUST_ACCESS,
                       1,
                       EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY,
                       EGL_LOSE_CONTEXT_ON_RESET,
                       EGL_NONE};
    EGLContext ctx;

    if (display())
        return NULL;
    /* No config: EGL_KHR_no_config_context, with EGL_KHR_surfaceless_context. */
    ctx = eglCreateContext_(dpy, NULL, NULL, robust ? strong : plain);
    if (!ctx) {
        fprintf(stderr, "nctxcheck: no %scontext: egl error 0x%x\n", robust ? "robust " : "",
                eglGetError_());
        return NULL;
    }
    if (!eglMakeCurrent_(dpy, NULL, NULL, ctx)) {
        fprintf(stderr, "nctxcheck: make current: egl error 0x%x\n", eglGetError_());
        eglDestroyContext_(dpy, ctx);
        return NULL;
    }
    return ctx;
}

static const char vs_src[] = "#version 300 es\n"
                             "in vec2 p;\n"
                             "void main() { gl_Position = vec4(p, 0.0, 1.0); }\n";

static const char fs_flat[] = "#version 300 es\n"
                              "precision mediump float;\n"
                              "uniform vec4 c;\n"
                              "out vec4 o;\n"
                              "void main() { o = c; }\n";

/* A loop the compiler cannot shorten: its count is a uniform and every
 * step feeds the next. */
static const char fs_spin[] = "#version 300 es\n"
                              "precision highp float;\n"
                              "uniform int n;\n"
                              "out vec4 o;\n"
                              "void main() {\n"
                              "    float a = gl_FragCoord.x * 0.001;\n"
                              "    for (int i = 0; i < n; i++)\n"
                              "        a = fract(a * 1.0001 + 0.1234);\n"
                              "    o = vec4(a, 0.5, 0.25, 1.0);\n"
                              "}\n";

/** shader - compile SRC as a shader of TYPE; 0, with its log, on failure. */
static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader_(type);
    GLint ok = 0;
    char log[1024];

    glShaderSource_(s, 1, &src, NULL);
    glCompileShader_(s);
    glGetShaderiv_(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog_(s, sizeof(log), NULL, log);
        fprintf(stderr, "nctxcheck: shader: %s\n", log);
        return 0;
    }
    return s;
}

/** program - link the vertex shader with FS; 0 on failure. */
static GLuint program(const char *fs)
{
    GLuint v = shader(GL_VERTEX_SHADER, vs_src), f = shader(GL_FRAGMENT_SHADER, fs);
    GLuint p;
    GLint ok = 0;

    if (!v || !f)
        return 0;
    p = glCreateProgram_();
    glAttachShader_(p, v);
    glAttachShader_(p, f);
    glBindAttribLocation_(p, 0, "p");
    glLinkProgram_(p);
    glGetProgramiv_(p, GL_LINK_STATUS, &ok);
    if (!ok)
        return 0;
    glUseProgram_(p);
    return p;
}

/** geometry - load the triangle, then a full quad, into one buffer. */
static void geometry(void)
{
    static const GLfloat v[] = {
        -0.5f, -0.5f, 0.5f, -0.5f, 0.0f,  0.5f,            /* triangle */
        -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f /* quad */
    };
    GLuint b;

    glGenBuffers_(1, &b);
    glBindBuffer_(GL_ARRAY_BUFFER, b);
    glBufferData_(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    glVertexAttribPointer_(0, 2, GL_FLOAT, 0, 0, NULL);
    glEnableVertexAttribArray_(0);
}

/**
 * target - a texture of W x H RGBA8 attached to a new framebuffer, bound.
 *
 * Returns the framebuffer, with the texture in *tex, or 0 when it is not
 * complete.
 */
static GLuint target(GLsizei w, GLsizei h, GLuint *tex)
{
    GLuint fb;

    glGenTextures_(1, tex);
    glBindTexture_(GL_TEXTURE_2D, *tex);
    glTexStorage2D_(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glGenFramebuffers_(1, &fb);
    glBindFramebuffer_(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    if (glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 0;
    glViewport_(0, 0, w, h);
    return fb;
}

/** info - print the EGL and GL strings, the render node and two extensions. */
static int info(void)
{
    EGLAttrib dev = 0;
    const char *s;

    if (!context(0))
        return fail("no context");
    printf("nctxcheck: info: egl vendor: %s\n", eglQueryString_(dpy, EGL_VENDOR));
    printf("nctxcheck: info: egl version: %s\n", eglQueryString_(dpy, EGL_VERSION));
    if (eglQueryDisplayAttribEXT_ && eglQueryDeviceStringEXT_ &&
        eglQueryDisplayAttribEXT_(dpy, EGL_DEVICE_EXT, &dev) && dev) {
        s = eglQueryDeviceStringEXT_((EGLDeviceEXT)dev, EGL_DRM_RENDER_NODE_FILE_EXT);
        printf("nctxcheck: info: render node: %s\n", s ? s : "(none)");
    }
    printf("nctxcheck: info: gl vendor: %s\n", glGetString_(GL_VENDOR));
    printf("nctxcheck: info: gl renderer: %s\n", glGetString_(GL_RENDERER));
    printf("nctxcheck: info: gl version: %s\n", glGetString_(GL_VERSION));
    s = eglQueryString_(dpy, EGL_EXTENSIONS);
    printf("nctxcheck: info: native fence: %s\n",
           s && strstr(s, "EGL_ANDROID_native_fence_sync") ? "yes" : "no");
    printf("nctxcheck: info: robustness: %s\n",
           s && strstr(s, "EGL_EXT_create_context_robustness") ? "yes" : "no");
    return pass();
}

/** pixel - read one pixel of the bound framebuffer into RGBA[4]. */
static void pixel(GLint x, GLint y, GLubyte *rgba)
{
    glReadPixels_(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

/**
 * same - whether RGBA is within one step of R, G, B, A, the rounding the
 * GL allows a normalized colour.
 */
static int same(const GLubyte *p, int r, int g, int b, int a)
{
    return abs(p[0] - r) <= 1 && abs(p[1] - g) <= 1 && abs(p[2] - b) <= 1 && abs(p[3] - a) <= 1;
}

/**
 * draw_triangle - clear the bound 64 x 64 target to navy, draw the orange
 * triangle and check its middle and a corner. 0 when both read right.
 */
static int draw_triangle(GLuint prog)
{
    GLubyte in[4], out[4];

    glClearColor_(0.0f, 0.0f, 128 / 255.0f, 1.0f);
    glClear_(GL_COLOR_BUFFER_BIT);
    glUniform4f_(glGetUniformLocation_(prog, "c"), 1.0f, 128 / 255.0f, 0.0f, 1.0f);
    glDrawArrays_(GL_TRIANGLES, 0, 3);
    pixel(32, 28, in);
    pixel(2, 61, out);
    printf("nctxcheck: %s: inside %u,%u,%u,%u outside %u,%u,%u,%u\n", check, in[0], in[1], in[2],
           in[3], out[0], out[1], out[2], out[3]);
    return !(same(in, 255, 128, 0, 255) && same(out, 0, 0, 128, 255));
}

/** triangle - the orange triangle over navy in a new 64 x 64 target. */
static int triangle(void)
{
    GLuint prog, tex;

    if (!context(0))
        return fail("no context");
    if (!(prog = program(fs_flat)))
        return fail("no program");
    geometry();
    if (!target(64, 64, &tex))
        return fail("framebuffer incomplete");
    if (draw_triangle(prog))
        return fail("pixels differ");
    if (glGetError_() != GL_NO_ERROR)
        return fail("gl error");
    return pass();
}

/* The pressure textures: 2048 x 2048 RGBA8, 16 MiB each. */
#define SIDE    2048
#define TEX_MIB 16

/** word - the pattern's value at texel I of texture T, never 0. */
static uint32_t word(unsigned t, unsigned i)
{
    uint32_t x = (t + 1) * 2654435761u ^ i * 40503u;

    return (x ^ x >> 15) | 1;
}

/**
 * pressure - textures of 16 MiB up to MIB, each filled from the CPU and then
 * drawn into by the GPU (a 64 x 64 corner in a colour of its own), then all
 * read back in the reverse order, newest last bound first.
 *
 * Once the textures outgrow what the host's GPU can keep bound, its driver
 * must evict older ones to bind newer and bind them again for the
 * read-back; the host's counters around this run show how many times.
 * Every texel is compared with what was written.
 */
static int pressure(int mib)
{
    unsigned n = mib / TEX_MIB, t, i, x, y, bad = 0;
    uint32_t *buf = malloc((size_t)SIDE * SIDE * 4);
    GLuint *tex = calloc(n, sizeof(*tex)), *fb = calloc(n, sizeof(*fb)), prog;
    double t0;

    if (!buf || !tex || !fb || n == 0)
        return fail("bad size or no memory");
    if (!context(0))
        return fail("no context");
    if (!(prog = program(fs_flat)))
        return fail("no program");
    geometry();
    t0 = now();
    for (t = 0; t < n; t++) {
        for (i = 0; i < (unsigned)SIDE * SIDE; i++)
            buf[i] = word(t, i);
        if (!(fb[t] = target(SIDE, SIDE, &tex[t])))
            return fail("framebuffer incomplete");
        glTexSubImage2D_(GL_TEXTURE_2D, 0, 0, 0, SIDE, SIDE, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        glEnable_(GL_SCISSOR_TEST);
        glScissor_(0, 0, 64, 64);
        glClearColor_((t & 255) / 255.0f, 1.0f, ((t >> 8) & 255) / 255.0f, 1.0f);
        glClear_(GL_COLOR_BUFFER_BIT);
        glDisable_(GL_SCISSOR_TEST);
        glFlush_();
        if (glGetError_() != GL_NO_ERROR) {
            printf("nctxcheck: pressure: gl error at texture %u, %u MiB\n", t, t * TEX_MIB);
            return fail("gl error while allocating");
        }
    }
    glFinish_();
    printf("nctxcheck: pressure: %u textures, %u MiB, written in %.2f s\n", n, n * TEX_MIB,
           now() - t0);
    t0 = now();
    for (t = n; t-- > 0;) {
        glBindFramebuffer_(GL_FRAMEBUFFER, fb[t]);
        memset(buf, 0, (size_t)SIDE * SIDE * 4);
        glReadPixels_(0, 0, SIDE, SIDE, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        for (y = 0; y < SIDE; y++)
            for (x = 0; x < SIDE; x++) {
                uint32_t want = word(t, y * SIDE + x);
                const GLubyte *p = (const GLubyte *)&buf[y * SIDE + x];

                if (x < 64 && y < 64) {
                    if (!same(p, t & 255, 255, (t >> 8) & 255, 255))
                        bad++;
                } else if (buf[y * SIDE + x] != want) {
                    bad++;
                }
            }
    }
    printf("nctxcheck: pressure: read back in %.2f s, %u texels differ\n", now() - t0, bad);
    for (t = 0; t < n; t++) {
        glDeleteFramebuffers_(1, &fb[t]);
        glDeleteTextures_(1, &tex[t]);
    }
    if (bad)
        return fail("texels differ");
    return pass();
}

/**
 * submit - N draws of the triangle, each flushed alone so that each is a
 * submission of its own, with a GL fence after every 64th waited on; then
 * the last colour read back.
 */
static int submit(int n)
{
    GLuint prog, tex;
    GLint c;
    GLubyte p[4];
    GLsync s;
    GLenum r;
    double t0;
    int i, waits = 0;

    if (!context(0))
        return fail("no context");
    if (!(prog = program(fs_flat)))
        return fail("no program");
    geometry();
    if (!target(64, 64, &tex))
        return fail("framebuffer incomplete");
    c = glGetUniformLocation_(prog, "c");
    t0 = now();
    for (i = 0; i < n; i++) {
        glUniform4f_(c, (i & 255) / 255.0f, ((i >> 8) & 255) / 255.0f, 0.5f, 1.0f);
        glDrawArrays_(GL_TRIANGLES, 0, 3);
        glFlush_();
        if (i % 64 == 63) {
            s = glFenceSync_(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            r = glClientWaitSync_(s, GL_SYNC_FLUSH_COMMANDS_BIT, 10000000000ull);
            glDeleteSync_(s);
            if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) {
                printf("nctxcheck: submit: draw %d: wait gave 0x%x\n", i, r);
                return fail("fence not signalled in 10 s");
            }
            waits++;
        }
    }
    pixel(32, 28, p);
    printf("nctxcheck: submit: %d draws, %d fences waited, %.2f s, last %u,%u,%u\n", n, waits,
           now() - t0, p[0], p[1], p[2]);
    i = n - 1;
    if (!same(p, i & 255, (i >> 8) & 255, 128, 255))
        return fail("last colour differs");
    if (glGetError_() != GL_NO_ERROR)
        return fail("gl error");
    return pass();
}

/**
 * fence - GL syncs created after successive draws must signal in order; a
 * native fence of the last draw must export a sync file that polls ready;
 * that file imported again and waited on by the GPU must hold a draw after
 * it, which then reads back right.
 */
static int fence(void)
{
    GLuint prog, tex;
    GLsync s[16];
    GLenum r;
    EGLAttrib a[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, -1, EGL_NONE};
    EGLSync es, imp;
    struct pollfd pf;
    int i, fd;

    if (!context(0))
        return fail("no context");
    if (!(prog = program(fs_flat)))
        return fail("no program");
    geometry();
    if (!target(64, 64, &tex))
        return fail("framebuffer incomplete");
    for (i = 0; i < 16; i++) {
        glUniform4f_(glGetUniformLocation_(prog, "c"), i / 15.0f, 0.0f, 0.0f, 1.0f);
        glDrawArrays_(GL_TRIANGLES, 0, 3);
        s[i] = glFenceSync_(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush_();
    }
    for (i = 15; i >= 0; i--) {
        /* Waiting on the last first: every earlier one must then be done. */
        r = glClientWaitSync_(s[i], 0, i == 15 ? 10000000000ull : 0);
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) {
            printf("nctxcheck: fence: sync %d gave 0x%x after sync 15 signalled\n", i, r);
            return fail("syncs out of order");
        }
        glDeleteSync_(s[i]);
    }
    printf("nctxcheck: fence: 16 GL syncs signalled in order\n");
    if (!eglDupNativeFenceFDANDROID_)
        return fail("no EGL_ANDROID_native_fence_sync");
    es = eglCreateSync_(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (!es)
        return fail("no native fence");
    glFlush_();
    fd = eglDupNativeFenceFDANDROID_(dpy, es);
    if (fd < 0)
        return fail("no sync file");
    pf.fd = fd;
    pf.events = POLLIN;
    i = poll(&pf, 1, 10000);
    printf("nctxcheck: fence: sync file %d polled %s\n", fd, i == 1 ? "ready" : "not ready");
    if (i != 1)
        return fail("sync file not ready in 10 s");
    if (eglClientWaitSync_(dpy, es, EGL_SYNC_FLUSH_COMMANDS_BIT, EGL_FOREVER) !=
        EGL_CONDITION_SATISFIED)
        return fail("native fence wait");
    eglDestroySync_(dpy, es);
    a[1] = fd; /* the import takes the descriptor */
    imp = eglCreateSync_(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, a);
    if (!imp || !eglWaitSync_(dpy, imp, 0))
        return fail("server wait on an imported sync file");
    if (draw_triangle(prog))
        return fail("pixels differ after the server wait");
    eglDestroySync_(dpy, imp);
    printf("nctxcheck: fence: native fence exported, polled, imported and waited on\n");
    return pass();
}

/**
 * hang - a robust context's full-target draw whose every pixel loops LOOPS
 * times, waited on for up to 120 s; then the context's reset status, and a
 * triangle in a new context to show the GPU serves again.
 *
 * With a large LOOPS the draw outlasts the host's hang check, so the host's
 * driver resets the GPU and fails the batch's fence: how that reaches the
 * guest (the wait's result, the reset status, a lost context) is what this
 * reports. The pass needs only the new context's triangle.
 */
static int hang(int loops)
{
    EGLContext ctx;
    GLuint prog, tex;
    GLsync s;
    GLenum r, st = 0;
    double t0;
    int robust = 1;

    if (!(ctx = context(1))) {
        robust = 0;
        if (!(ctx = context(0)))
            return fail("no context");
    }
    printf("nctxcheck: hang: robust context: %s\n", robust ? "yes" : "no");
    if (!(prog = program(fs_spin)))
        return fail("no program");
    geometry();
    if (!target(1024, 1024, &tex))
        return fail("framebuffer incomplete");
    glUniform1i_(glGetUniformLocation_(prog, "n"), loops);
    t0 = now();
    glDrawArrays_(GL_TRIANGLE_STRIP, 3, 4);
    s = glFenceSync_(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    r = glClientWaitSync_(s, GL_SYNC_FLUSH_COMMANDS_BIT, 120000000000ull);
    printf("nctxcheck: hang: wait gave %s after %.2f s\n",
           r == GL_ALREADY_SIGNALED      ? "already signalled"
           : r == GL_CONDITION_SATISFIED ? "condition satisfied"
           : r == GL_TIMEOUT_EXPIRED     ? "timeout expired"
           : r == GL_WAIT_FAILED         ? "wait failed"
                                         : "other",
           now() - t0);
    if (glGetGraphicsResetStatus_) {
        st = glGetGraphicsResetStatus_();
        printf("nctxcheck: hang: reset status: %s\n", st == GL_NO_ERROR                 ? "none"
                                                      : st == GL_GUILTY_CONTEXT_RESET   ? "guilty"
                                                      : st == GL_INNOCENT_CONTEXT_RESET ? "innocent"
                                                      : st == GL_UNKNOWN_CONTEXT_RESET  ? "unknown"
                                                                                        : "other");
    } else {
        printf("nctxcheck: hang: reset status: no query\n");
    }
    printf("nctxcheck: hang: gl error after: 0x%x\n", glGetError_());
    eglMakeCurrent_(dpy, NULL, NULL, NULL);
    eglDestroyContext_(dpy, ctx);
    if (!context(0))
        return fail("no new context after the fault");
    if (!(prog = program(fs_flat)))
        return fail("no program after the fault");
    geometry();
    if (!target(64, 64, &tex))
        return fail("framebuffer incomplete after the fault");
    if (draw_triangle(prog))
        return fail("a new context's triangle differs after the fault");
    return pass();
}

int main(int argc, char **argv)
{
    int arg = argc > 2 ? atoi(argv[2]) : 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2)
        goto usage;
    check = argv[1];
    if (resolve())
        return fail("entry points missing");
    if (!strcmp(check, "info"))
        return info();
    if (!strcmp(check, "triangle"))
        return triangle();
    if (!strcmp(check, "pressure") && arg > 0)
        return pressure(arg);
    if (!strcmp(check, "submit") && arg > 0)
        return submit(arg);
    if (!strcmp(check, "fence"))
        return fence();
    if (!strcmp(check, "hang") && arg > 0)
        return hang(arg);
usage:
    fprintf(stderr, "usage: nctxcheck info | triangle | pressure MIB | submit N | "
                    "fence | hang LOOPS\n");
    return 2;
}
