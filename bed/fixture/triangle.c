/*
 * triangle.c: draw a textured background and a triangle off screen with
 * OpenGL ES 2 on EGL's surfaceless platform, wait on a fence, read the
 * pixels back and check them, so that two drivers' pictures can be compared.
 * It links only libc and libdl: EGL and GLES come from libEGL.so.1 by
 * eglGetProcAddress, so that it builds where no development headers are, as
 * in Rach2's GNU root.
 */

#define _POSIX_C_SOURCE 200809L

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef void *EGLSyncKHR;
typedef int32_t EGLint;
typedef unsigned EGLBoolean;
typedef unsigned EGLenum;
typedef uint64_t EGLTimeKHR;

typedef unsigned GLuint;
typedef int GLint;
typedef unsigned GLenum;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLubyte;
typedef unsigned char GLboolean;
typedef char GLchar;

/* The values of Khronos's registry (egl.h, eglext.h, gl2.h). */
#define EGL_NO_DISPLAY                  ((EGLDisplay)0)
#define EGL_DEFAULT_DISPLAY             ((void *)0)
#define EGL_NO_CONFIG_KHR               ((EGLConfig)0)
#define EGL_NO_CONTEXT                  ((EGLContext)0)
#define EGL_NO_SURFACE                  ((EGLSurface)0)
#define EGL_NO_SYNC_KHR                 ((EGLSyncKHR)0)
#define EGL_NONE                        0x3038
#define EGL_EXTENSIONS                  0x3055
#define EGL_CONTEXT_CLIENT_VERSION      0x3098
#define EGL_OPENGL_ES_API               0x30A0
#define EGL_CONDITION_SATISFIED_KHR     0x30F6
#define EGL_SYNC_FENCE_KHR              0x30F9
#define EGL_PLATFORM_SURFACELESS_MESA   0x31DD
#define EGL_SYNC_FLUSH_COMMANDS_BIT_KHR 0x0001
#define EGL_FOREVER_KHR                 0xFFFFFFFFFFFFFFFFULL

#define GL_NO_ERROR             0
#define GL_FALSE                0
#define GL_TRIANGLES            0x0004
#define GL_TEXTURE_2D           0x0DE1
#define GL_RENDERER             0x1F01
#define GL_UNSIGNED_BYTE        0x1401
#define GL_FLOAT                0x1406
#define GL_RGBA                 0x1908
#define GL_NEAREST              0x2600
#define GL_TEXTURE_MAG_FILTER   0x2800
#define GL_TEXTURE_MIN_FILTER   0x2801
#define GL_TEXTURE0             0x84C0
#define GL_FRAGMENT_SHADER      0x8B30
#define GL_VERTEX_SHADER        0x8B31
#define GL_COMPILE_STATUS       0x8B81
#define GL_LINK_STATUS          0x8B82
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0    0x8CE0
#define GL_FRAMEBUFFER          0x8D40

/* Each entry point the program calls: its type, name and parameters. */
#define ENTRY_POINTS(X)                                                                       \
    X(EGLDisplay, eglGetPlatformDisplayEXT, (EGLenum, void *, const EGLint *))                \
    X(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *))                            \
    X(const char *, eglQueryString, (EGLDisplay, EGLint))                                     \
    X(EGLBoolean, eglBindAPI, (EGLenum))                                                      \
    X(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *))      \
    X(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))           \
    X(EGLBoolean, eglTerminate, (EGLDisplay))                                                 \
    X(EGLSyncKHR, eglCreateSyncKHR, (EGLDisplay, EGLenum, const EGLint *))                    \
    X(EGLint, eglClientWaitSyncKHR, (EGLDisplay, EGLSyncKHR, EGLint, EGLTimeKHR))             \
    X(EGLBoolean, eglDestroySyncKHR, (EGLDisplay, EGLSyncKHR))                                \
    X(const GLubyte *, glGetString, (GLenum))                                                 \
    X(GLenum, glGetError, (void))                                                             \
    X(GLuint, glCreateProgram, (void))                                                        \
    X(GLuint, glCreateShader, (GLenum))                                                       \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))          \
    X(void, glCompileShader, (GLuint))                                                        \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *))                                         \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                       \
    X(void, glAttachShader, (GLuint, GLuint))                                                 \
    X(void, glDeleteShader, (GLuint))                                                         \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *))                           \
    X(void, glLinkProgram, (GLuint))                                                          \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *))                                        \
    X(void, glUseProgram, (GLuint))                                                           \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *))                                  \
    X(void, glUniform1i, (GLint, GLint))                                                      \
    X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                         \
    X(void, glGenTextures, (GLsizei, GLuint *))                                               \
    X(void, glBindTexture, (GLenum, GLuint))                                                  \
    X(void, glActiveTexture, (GLenum))                                                        \
    X(void, glTexParameteri, (GLenum, GLenum, GLint))                                         \
    X(void, glTexImage2D,                                                                     \
      (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))          \
    X(void, glGenFramebuffers, (GLsizei, GLuint *))                                           \
    X(void, glBindFramebuffer, (GLenum, GLuint))                                              \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                  \
    X(GLenum, glCheckFramebufferStatus, (GLenum))                                             \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                     \
    X(void, glEnableVertexAttribArray, (GLuint))                                              \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei))                                           \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))

#define POINTER(type, name, params) static type(*name) params;
ENTRY_POINTS(POINTER)
#undef POINTER

/* The picture's side, which the texture's side equals. */
#define SIDE 256

/* The fence's wait is unbounded; the runner stops a draw that never ends. */
#define FENCE_WAIT_NS EGL_FOREVER_KHR

/* The colors, as RGBA bytes: the triangle and the texture's two squares. */
static const unsigned char tri_color[4] = {255, 64, 0, 255};
static const unsigned char dark[4] = {0, 0, 96, 255};
static const unsigned char light[4] = {0, 160, 224, 255};

/* The texture's pixel at X, Y: squares of 16 pixels in turn. */
static const unsigned char *texel(int x, int y)
{
    return ((x / 16) + (y / 16)) % 2 ? light : dark;
}

/*
 * Each pixel's center falls on the center of the texel under it, so the
 * nearest sample is exact on every driver.
 */
static const char *const vertex_src = "attribute vec2 pos;\n"
                                      "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *const back_src =
    "precision mediump float;\n"
    "uniform sampler2D tex;\n"
    "void main() { gl_FragColor = texture2D(tex, gl_FragCoord.xy / 256.0); }\n";
static const char *const tri_src = "precision mediump float;\n"
                                   "uniform vec4 color;\n"
                                   "void main() { gl_FragColor = color; }\n";

/** Print MSG as the program's failure and exit with status 1. */
static void die(const char *msg)
{
    fprintf(stderr, "triangle: error: %s\n", msg);
    exit(1);
}

/**
 * Open libEGL.so.1 and resolve every entry point of ENTRY_POINTS by its
 * eglGetProcAddress, which answers core EGL and GLES functions as well as
 * extensions (EGL_KHR_get_all_proc_addresses). Exit on a missing one.
 */
static void load(void)
{
    void *(*get)(const char *);
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);

    if (egl == NULL) {
        fprintf(stderr, "triangle: libEGL.so.1: %s\n", dlerror());
        die("EGL did not load");
    }
    *(void **)&get = dlsym(egl, "eglGetProcAddress");
    if (get == NULL)
        die("libEGL.so.1 has no eglGetProcAddress");
#define RESOLVE(type, name, params)                        \
    *(void **)&name = get(#name);                          \
    if (name == NULL) {                                    \
        fprintf(stderr, "triangle: missing: %s\n", #name); \
        die("an entry point is missing");                  \
    }
    ENTRY_POINTS(RESOLVE)
#undef RESOLVE
}

/** Compile and link VS and FS into a program with `pos` at attribute 0. */
static GLuint program(const char *vs, const char *fs)
{
    const char *src[2] = {vs, fs};
    GLenum kind[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
    GLuint prog = glCreateProgram();
    GLint ok;
    char log[512];
    int i;

    for (i = 0; i < 2; i++) {
        GLuint sh = glCreateShader(kind[i]);

        glShaderSource(sh, 1, &src[i], NULL);
        glCompileShader(sh);
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            glGetShaderInfoLog(sh, sizeof(log), NULL, log);
            fprintf(stderr, "triangle: shader: %s\n", log);
            die("a shader did not compile");
        }
        glAttachShader(prog, sh);
        glDeleteShader(sh);
    }
    glBindAttribLocation(prog, 0, "pos");
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
        die("the program did not link");
    return prog;
}

/** Open EGL's surfaceless display and make an ES 2 context current on it. */
static EGLDisplay open_context(void)
{
    static const EGLint attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    const char *exts;
    EGLDisplay dpy;
    EGLContext ctx;

    dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL))
        die("the surfaceless display did not open");
    exts = eglQueryString(dpy, EGL_EXTENSIONS);
    if (strstr(exts, "EGL_KHR_no_config_context") == NULL ||
        strstr(exts, "EGL_KHR_surfaceless_context") == NULL ||
        strstr(exts, "EGL_KHR_fence_sync") == NULL)
        die("the display lacks a context without a config or surface, or fences");
    if (!eglBindAPI(EGL_OPENGL_ES_API))
        die("no OpenGL ES");
    ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attribs);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
        die("no ES 2 context");
    return dpy;
}

/** Make the checkered texture, which reaches the driver as one upload. */
static GLuint make_texture(void)
{
    static unsigned char pixels[SIDE * SIDE * 4];
    GLuint tex;
    int x, y;

    for (y = 0; y < SIDE; y++)
        for (x = 0; x < SIDE; x++)
            memcpy(pixels + 4 * (y * SIDE + x), texel(x, y), 4);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SIDE, SIDE, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    return tex;
}

/** Make a framebuffer whose color is a new SIDE by SIDE texture, and bind it. */
static void make_target(void)
{
    GLuint fbo, color;

    glGenTextures(1, &color);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SIDE, SIDE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        die("the framebuffer is incomplete");
    glViewport(0, 0, SIDE, SIDE);
}

/** Draw the background from TEX over the whole target, then the triangle. */
static void draw(GLuint tex)
{
    static const GLfloat full[] = {-1, -1, 3, -1, -1, 3};
    /* Corners on pixel edges: (32, 32), (224, 32) and (128, 224). */
    static const GLfloat tri[] = {-0.75f, -0.75f, 0.75f, -0.75f, 0.0f, 0.75f};
    GLuint back = program(vertex_src, back_src);
    GLuint front = program(vertex_src, tri_src);

    glEnableVertexAttribArray(0);
    glUseProgram(back);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(back, "tex"), 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, full);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glUseProgram(front);
    glUniform4f(glGetUniformLocation(front, "color"), tri_color[0] / 255.0f, tri_color[1] / 255.0f,
                tri_color[2] / 255.0f, 1.0f);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/** Wait on a fence behind the draw; return 0 once it has signalled. */
static int wait_fence(EGLDisplay dpy)
{
    EGLSyncKHR sync;
    EGLint got;

    sync = eglCreateSyncKHR(dpy, EGL_SYNC_FENCE_KHR, NULL);
    if (sync == EGL_NO_SYNC_KHR)
        die("no fence");
    got = eglClientWaitSyncKHR(dpy, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, FENCE_WAIT_NS);
    eglDestroySyncKHR(dpy, sync);
    return got == EGL_CONDITION_SATISFIED_KHR ? 0 : -1;
}

/** Return the 64-bit FNV-1a hash of N bytes at P. */
static uint64_t fnv1a(const unsigned char *p, size_t n)
{
    uint64_t h = 14695981039346656037ULL;

    while (n-- > 0)
        h = (h ^ *p++) * 1099511628211ULL;
    return h;
}

/** Write the picture at PIX, bottom row first, to PATH as a binary PPM. */
static void write_ppm(const char *path, const unsigned char *pix)
{
    FILE *f = fopen(path, "wb");
    int x, y;

    if (f == NULL)
        die("the picture's file did not open");
    fprintf(f, "P6\n%d %d\n255\n", SIDE, SIDE);
    for (y = SIDE - 1; y >= 0; y--)
        for (x = 0; x < SIDE; x++)
            fwrite(pix + 4 * (y * SIDE + x), 1, 3, f);
    if (fclose(f) != 0)
        die("the picture's file did not write");
}

/**
 * Count the pixels at PIX by what they show and check them: every pixel is
 * the triangle's color or the texel under it, and the triangle's center and
 * the picture's corners are what they must be. Return 0 when all hold.
 */
static int check(const unsigned char *pix)
{
    long inside = 0, outside = 0, other = 0;
    int x, y, bad = 0;

    for (y = 0; y < SIDE; y++)
        for (x = 0; x < SIDE; x++) {
            const unsigned char *p = pix + 4 * (y * SIDE + x);

            if (memcmp(p, tri_color, 4) == 0)
                inside++;
            else if (memcmp(p, texel(x, y), 4) == 0)
                outside++;
            else
                other++;
        }
    printf("triangle: pixels: %ld triangle, %ld texture, %ld other\n", inside, outside, other);
    if (memcmp(pix + 4 * (96 * SIDE + 128), tri_color, 4) != 0) {
        printf("triangle: check: the triangle's center is not its color\n");
        bad = 1;
    }
    if (memcmp(pix, texel(0, 0), 4) != 0 ||
        memcmp(pix + 4 * (SIDE * SIDE - 1), texel(SIDE - 1, SIDE - 1), 4) != 0) {
        printf("triangle: check: a corner is not the texture's\n");
        bad = 1;
    }
    if (other != 0) {
        printf("triangle: check: %ld pixels are neither\n", other);
        bad = 1;
    }
    return bad ? -1 : 0;
}

int main(int argc, char **argv)
{
    static unsigned char pix[SIDE * SIDE * 4];
    const char *ppm = NULL;
    EGLDisplay dpy;
    GLuint tex;
    int c;

    while ((c = getopt(argc, argv, "o:")) != -1) {
        if (c != 'o') {
            fprintf(stderr, "usage: triangle [-o FILE.ppm]\n");
            return 2;
        }
        ppm = optarg;
    }
    load();
    dpy = open_context();
    printf("triangle: renderer: %s\n", (const char *)glGetString(GL_RENDERER));
    tex = make_texture();
    make_target();
    draw(tex);
    if (wait_fence(dpy) < 0)
        die("the fence did not signal");
    printf("triangle: fence: signalled\n");
    glReadPixels(0, 0, SIDE, SIDE, GL_RGBA, GL_UNSIGNED_BYTE, pix);
    if (glGetError() != GL_NO_ERROR)
        die("GL reported an error");
    printf("triangle: checksum: %016llx\n", (unsigned long long)fnv1a(pix, sizeof(pix)));
    if (ppm != NULL)
        write_ppm(ppm, pix);
    if (check(pix) < 0) {
        printf("triangle: result: failed\n");
        return 1;
    }
    printf("triangle: result: ok\n");
    eglTerminate(dpy);
    return 0;
}
