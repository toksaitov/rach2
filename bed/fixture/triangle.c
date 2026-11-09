/*
 * triangle.c: draw a textured background and a triangle off screen with
 * OpenGL ES 2 on EGL's surfaceless platform, wait on a fence, read the
 * pixels back and check them, so that two drivers' pictures can be compared.
 */

#define _POSIX_C_SOURCE 200809L

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The picture's side, which the texture's side equals. */
#define SIDE 256

/* The fence's wait, in nanoseconds: a minute, far past any draw here. */
#define FENCE_WAIT_NS 60000000000ULL

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
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
    const char *exts;
    EGLDisplay dpy;
    EGLContext ctx;

    get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_display == NULL)
        die("no eglGetPlatformDisplayEXT");
    dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
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
    PFNEGLCREATESYNCKHRPROC create = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    PFNEGLCLIENTWAITSYNCKHRPROC wait =
        (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
    PFNEGLDESTROYSYNCKHRPROC destroy =
        (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    EGLSyncKHR sync;
    EGLint got;

    if (create == NULL || wait == NULL || destroy == NULL)
        die("no fence functions");
    sync = create(dpy, EGL_SYNC_FENCE_KHR, NULL);
    if (sync == EGL_NO_SYNC_KHR)
        die("no fence");
    got = wait(dpy, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, FENCE_WAIT_NS);
    destroy(dpy, sync);
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
