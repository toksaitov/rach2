/*
 * glreset.c: a robust OpenGL ES context that draws until its GPU is lost and
 * reports what it saw of the loss, for virgl-lost.sh.
 *
 *   glreset BEFORE AFTER FRAMES
 *
 * Makes an OpenGL ES 3 context on EGL's surfaceless platform, the most
 * robust kind the driver gives: one that asks for robust access and to lose
 * the context on a reset; failing that, robust access alone, which gets no
 * notice of a reset; failing that, a plain one. It draws frame after frame
 * into a 64x64 texture: a clear to a colour of the frame's number, and the
 * read of a pixel back, which waits for the host. Each frame it asks
 * glGetGraphicsResetStatus and glGetError. It prints `glreset: drawing` once
 * the first frame is read back right. Its caller makes the file BEFORE,
 * waits for the `before` line, takes the device away and makes the file
 * AFTER; the program then draws FRAMES frames more, makes a new display and
 * context as a program started after the loss would, and ends. It prints
 *
 *   glreset: context: KIND
 *   glreset: before: pixels right|wrong, status STATUS, errors ERRORS
 *   glreset: after: frames N, pixels right|wrong, status STATUS, errors ERRORS
 *   glreset: new context: KIND on RENDERER | none (WHY)
 *
 * KIND is `lose context on reset`, `robust access, no reset notification` or
 * `plain`, with the EGL error that refused each kind before it. `before` is
 * of the frames drawn until BEFORE appeared, and `after` of every frame
 * since, the last FRAMES of them drawn after AFTER appeared: `right` when
 * each frame's pixel read back as drawn, the first reset status the context
 * gave other than GL_NO_ERROR, and the first GL error. The lines hold no
 * count that depends on the machine's speed, so two guests that see the same
 * loss print the same lines. The status is 0 unless the first context could
 * not be made or drew wrong before the loss.
 *
 * It declares the few EGL and OpenGL ES names it uses itself, since neither
 * guest has their headers, and is linked against the libraries by path.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef int EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef intptr_t EGLAttrib;
typedef void (*EGLproc)(void);

#define EGL_PLATFORM_SURFACELESS_MESA                  0x31DD
#define EGL_OPENGL_ES_API                              0x30A0
#define EGL_OPENGL_ES3_BIT                             0x0040
#define EGL_PBUFFER_BIT                                0x0001
#define EGL_SURFACE_TYPE                               0x3033
#define EGL_RENDERABLE_TYPE                            0x3040
#define EGL_NONE                                       0x3038
#define EGL_EXTENSIONS                                 0x3055
#define EGL_CONTEXT_MAJOR_VERSION                      0x3098
#define EGL_CONTEXT_OPENGL_ROBUST_ACCESS               0x31B2
#define EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY 0x31BD
#define EGL_LOSE_CONTEXT_ON_RESET                      0x31BF

EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native, const EGLAttrib *attribs);
EGLBoolean eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor);
EGLBoolean eglTerminate(EGLDisplay d);
EGLBoolean eglBindAPI(EGLenum api);
EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *attribs, EGLConfig *configs, EGLint size,
                           EGLint *count);
EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *attribs);
EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext c);
EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext c);
EGLint eglGetError(void);
const char *eglQueryString(EGLDisplay d, EGLint name);
EGLproc eglGetProcAddress(const char *name);

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned int GLbitfield;

#define GL_NO_ERROR                      0
#define GL_RENDERER                      0x1F01
#define GL_TEXTURE_2D                    0x0DE1
#define GL_RGBA                          0x1908
#define GL_RGBA8                         0x8058
#define GL_UNSIGNED_BYTE                 0x1401
#define GL_FRAMEBUFFER                   0x8D40
#define GL_COLOR_ATTACHMENT0             0x8CE0
#define GL_FRAMEBUFFER_COMPLETE          0x8CD5
#define GL_COLOR_BUFFER_BIT              0x4000
#define GL_GUILTY_CONTEXT_RESET          0x8253
#define GL_INNOCENT_CONTEXT_RESET        0x8254
#define GL_UNKNOWN_CONTEXT_RESET         0x8255
#define GL_CONTEXT_LOST                  0x0507
#define GL_OUT_OF_MEMORY                 0x0505
#define GL_INVALID_OPERATION             0x0502
#define GL_INVALID_FRAMEBUFFER_OPERATION 0x0506

GLenum glGetError(void);
const unsigned char *glGetString(GLenum name);
void glGenTextures(GLsizei n, GLuint *names);
void glBindTexture(GLenum target, GLuint name);
void glTexStorage2D(GLenum target, GLsizei levels, GLenum format, GLsizei w, GLsizei h);
void glGenFramebuffers(GLsizei n, GLuint *names);
void glBindFramebuffer(GLenum target, GLuint name);
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
                            GLint level);
GLenum glCheckFramebufferStatus(GLenum target);
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glClear(GLbitfield mask);
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *data);

static GLenum (*reset_status)(void);

/** The name of a reset status or a GL error. */
static const char *gl_name(GLenum e)
{
    static char other[24];

    switch (e) {
    case GL_NO_ERROR:
        return "GL_NO_ERROR";
    case GL_GUILTY_CONTEXT_RESET:
        return "GL_GUILTY_CONTEXT_RESET";
    case GL_INNOCENT_CONTEXT_RESET:
        return "GL_INNOCENT_CONTEXT_RESET";
    case GL_UNKNOWN_CONTEXT_RESET:
        return "GL_UNKNOWN_CONTEXT_RESET";
    case GL_CONTEXT_LOST:
        return "GL_CONTEXT_LOST";
    case GL_OUT_OF_MEMORY:
        return "GL_OUT_OF_MEMORY";
    case GL_INVALID_OPERATION:
        return "GL_INVALID_OPERATION";
    case GL_INVALID_FRAMEBUFFER_OPERATION:
        return "GL_INVALID_FRAMEBUFFER_OPERATION";
    }
    snprintf(other, sizeof(other), "0x%04x", e);
    return other;
}

/** The name of an EGL error. */
static const char *egl_name(EGLint e)
{
    static char other[24];

    switch (e) {
    case 0x3003:
        return "EGL_BAD_ALLOC";
    case 0x3004:
        return "EGL_BAD_ATTRIBUTE";
    case 0x3005:
        return "EGL_BAD_CONFIG";
    case 0x3009:
        return "EGL_BAD_MATCH";
    case 0x3001:
        return "EGL_NOT_INITIALIZED";
    }
    snprintf(other, sizeof(other), "EGL error 0x%04x", e);
    return other;
}

/**
 * Make a surfaceless display and an OpenGL ES 3 context current on it, the
 * most robust kind the driver gives. Return NULL and the reason in `kind`,
 * or the context with its display in `*display` and its kind in `kind`,
 * after the error that refused each kind before it.
 */
static EGLContext context(EGLDisplay *display, char *kind, size_t size)
{
    static const EGLint config_attribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                            EGL_OPENGL_ES3_BIT, EGL_NONE};
    static const EGLint attribs[3][7] = {
        {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_OPENGL_ROBUST_ACCESS, 1,
         EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY, EGL_LOSE_CONTEXT_ON_RESET, EGL_NONE},
        {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_OPENGL_ROBUST_ACCESS, 1, EGL_NONE},
        {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE}};
    static const char *const kinds[3] = {"lose context on reset",
                                         "robust access, no reset notification", "plain"};
    EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
    EGLConfig config;
    EGLContext c = NULL;
    EGLint count = 0;
    size_t used = 0;
    int i;

    kind[0] = '\0';
    if (d == NULL || !eglInitialize(d, NULL, NULL)) {
        snprintf(kind, size, "no display, %s", egl_name(eglGetError()));
        return NULL;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API) || !eglChooseConfig(d, config_attribs, &config, 1, &count) ||
        count < 1) {
        snprintf(kind, size, "no config, %s", egl_name(eglGetError()));
        eglTerminate(d);
        return NULL;
    }
    for (i = 0; i < 3 && c == NULL; i++) {
        c = eglCreateContext(d, config, NULL, attribs[i]);
        if (c != NULL && !eglMakeCurrent(d, NULL, NULL, c)) {
            eglDestroyContext(d, c);
            c = NULL;
        }
        if (used < size)
            used += (size_t)snprintf(kind + used, size - used,
                                     c != NULL ? "%s%s" : "%s%s refused with %s",
                                     used > 0 ? "; " : "", kinds[i], egl_name(eglGetError()));
    }
    if (c == NULL) {
        eglTerminate(d);
        return NULL;
    }
    *display = d;
    return c;
}

/** Draw frame `n`, a clear to a colour of its number, and read a pixel back;
 * return 1 when the pixel is the colour drawn. */
static int frame(unsigned n)
{
    unsigned char want[4], got[4] = {0, 0, 0, 0};

    want[0] = (unsigned char)(n * 37u);
    want[1] = (unsigned char)(n * 101u + 17u);
    want[2] = (unsigned char)(n * 13u + 5u);
    want[3] = 255;
    glClearColor(want[0] / 255.0f, want[1] / 255.0f, want[2] / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
    return memcmp(want, got, 4) == 0;
}

int main(int argc, char **argv)
{
    EGLDisplay d;
    EGLContext c;
    GLuint texture, fbo;
    GLenum status = GL_NO_ERROR, error = GL_NO_ERROR, e;
    char kind[200];
    struct stat st;
    unsigned n = 0, after = 0, frames;
    int right = 1, marked = 0;

    if (argc != 4) {
        fprintf(stderr, "usage: glreset BEFORE AFTER FRAMES\n");
        return 2;
    }
    frames = (unsigned)strtoul(argv[3], NULL, 10);
    setvbuf(stdout, NULL, _IOLBF, 0);
    c = context(&d, kind, sizeof(kind));
    if (c == NULL) {
        fprintf(stderr, "glreset: context: none (%s)\n", kind);
        return 1;
    }
    printf("glreset: context: %s\n", kind);
    reset_status = (GLenum (*)(void))eglGetProcAddress("glGetGraphicsResetStatusKHR");
    if (reset_status == NULL)
        reset_status = (GLenum (*)(void))eglGetProcAddress("glGetGraphicsResetStatusEXT");
    while (glGetError() != GL_NO_ERROR)
        ;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 64, 64);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "glreset: framebuffer: incomplete\n");
        return 1;
    }
    glViewport(0, 0, 64, 64);
    /* Frames until AFTER exists and FRAMES more; the line for the frames
     * before the loss is printed when BEFORE appears, and the counts begin
     * again for those after it. */
    while (after < frames) {
        right = frame(n) && right;
        if (reset_status != NULL && status == GL_NO_ERROR)
            status = reset_status();
        e = glGetError();
        if (error == GL_NO_ERROR)
            error = e;
        if (n++ == 0 && right)
            printf("glreset: drawing\n");
        if (!marked && stat(argv[1], &st) == 0) {
            printf("glreset: before: pixels %s, status %s", right ? "right" : "wrong",
                   gl_name(status));
            printf(", errors %s\n", error == GL_NO_ERROR ? "none" : gl_name(error));
            if (!right)
                return 1;
            marked = 1;
            status = error = GL_NO_ERROR;
        } else if (marked && stat(argv[2], &st) == 0) {
            after++;
        }
    }
    printf("glreset: after: frames %u, pixels %s, status %s", after, right ? "right" : "wrong",
           gl_name(status));
    printf(", errors %s\n", error == GL_NO_ERROR ? "none" : gl_name(error));
    eglMakeCurrent(d, NULL, NULL, NULL);
    eglDestroyContext(d, c);
    eglTerminate(d);
    /* What a program started now gets. */
    c = context(&d, kind, sizeof(kind));
    if (c == NULL)
        printf("glreset: new context: none (%s)\n", kind);
    else
        printf("glreset: new context: %s on %s\n", kind, (const char *)glGetString(GL_RENDERER));
    return 0;
}
