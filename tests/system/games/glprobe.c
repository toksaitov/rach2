/* glprobe: draw a shaded triangle with WGL's OpenGL, read it back, sum it. */
#include "probe.h"
#include <GL/gl.h>

/* OpenGL 3.0's framebuffer objects, which opengl32's 1.1 header lacks. */
#define PROBE_GL_FRAMEBUFFER          0x8D40
#define PROBE_GL_RENDERBUFFER         0x8D41
#define PROBE_GL_COLOR_ATTACHMENT0    0x8CE0
#define PROBE_GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define PROBE_GL_RGBA8                0x8058
typedef void(APIENTRY *gen_fn)(GLsizei, GLuint *);
typedef void(APIENTRY *bind_fn)(GLenum, GLuint);
typedef void(APIENTRY *storage_fn)(GLenum, GLenum, GLsizei, GLsizei);
typedef void(APIENTRY *attach_fn)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum(APIENTRY *status_fn)(GLenum);
/* The generic function type, which GCC lets a function pointer pass through. */
typedef void (*any_fn)(void);

/** Look up the OpenGL function NAME; stop with status 1 if it is absent. */
static any_fn need(const char *name)
{
    PROC p = wglGetProcAddress(name);

    if (p == NULL || p == (PROC)1 || p == (PROC)2 || p == (PROC)3 || p == (PROC)-1) {
        printf("glprobe: no %s\n", name);
        exit(1);
    }
    return (any_fn)p;
}

/** Draw red, green and blue corners over a navy clear in the viewport. */
static void draw(void)
{
    glClearColor(0.0f, 0.0f, 0.5f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glShadeModel(GL_SMOOTH);
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex2f(-1.0f, -1.0f);
    glColor3f(0.0f, 1.0f, 0.0f);
    glVertex2f(0.0f, 1.0f);
    glColor3f(0.0f, 0.0f, 1.0f);
    glVertex2f(1.0f, -1.0f);
    glEnd();
}

/**
 * Make a GL context on a visible window, draw the triangle into a 64x64
 * framebuffer object and read it back, then draw it in the window and swap.
 * Prints the renderer and the readback's checksum line. Returns 0 when the
 * corner is the clear colour and the centre is covered, 1 when a call fails,
 * 2 when the pixels are wrong.
 */
int main(void)
{
    static unsigned char px[PROBE_SIZE * PROBE_SIZE * 4];
    PIXELFORMATDESCRIPTOR pfd;
    gen_fn gen_fb, gen_rb;
    bind_fn bind_fb, bind_rb;
    storage_fn storage;
    attach_fn attach;
    status_fn status;
    unsigned long crc = 0;
    unsigned char *centre, *corner;
    GLuint fb, rb;
    HGLRC rc;
    HWND w;
    HDC dc;
    int y, format, ok;

    probe_output();
    w = probe_window("glprobe");
    if (w == NULL) {
        printf("glprobe: no window\n");
        return 1;
    }
    dc = GetDC(w);
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cAlphaBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    format = ChoosePixelFormat(dc, &pfd);
    if (format == 0 || !SetPixelFormat(dc, format, &pfd)) {
        printf("glprobe: no pixel format: %lu\n", GetLastError());
        return 1;
    }
    rc = wglCreateContext(dc);
    if (rc == NULL || !wglMakeCurrent(dc, rc)) {
        printf("glprobe: no context: %lu\n", GetLastError());
        return 1;
    }
    printf("renderer: %s vendor %s version %s\n", (const char *)glGetString(GL_RENDERER),
           (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_VERSION));
    gen_fb = (gen_fn)need("glGenFramebuffers");
    gen_rb = (gen_fn)need("glGenRenderbuffers");
    bind_fb = (bind_fn)need("glBindFramebuffer");
    bind_rb = (bind_fn)need("glBindRenderbuffer");
    storage = (storage_fn)need("glRenderbufferStorage");
    attach = (attach_fn)need("glFramebufferRenderbuffer");
    status = (status_fn)need("glCheckFramebufferStatus");
    gen_fb(1, &fb);
    gen_rb(1, &rb);
    bind_rb(PROBE_GL_RENDERBUFFER, rb);
    storage(PROBE_GL_RENDERBUFFER, PROBE_GL_RGBA8, PROBE_SIZE, PROBE_SIZE);
    bind_fb(PROBE_GL_FRAMEBUFFER, fb);
    attach(PROBE_GL_FRAMEBUFFER, PROBE_GL_COLOR_ATTACHMENT0, PROBE_GL_RENDERBUFFER, rb);
    if (status(PROBE_GL_FRAMEBUFFER) != PROBE_GL_FRAMEBUFFER_COMPLETE) {
        printf("glprobe: framebuffer incomplete\n");
        return 1;
    }
    glViewport(0, 0, PROBE_SIZE, PROBE_SIZE);
    draw();
    glFinish();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, PROBE_SIZE, PROBE_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (glGetError() != GL_NO_ERROR) {
        printf("glprobe: readback failed\n");
        return 1;
    }
    /* glReadPixels returns the bottom row first; sum from the top. */
    for (y = PROBE_SIZE - 1; y >= 0; y--)
        crc = probe_crc32(crc, px + (size_t)y * PROBE_SIZE * 4, PROBE_SIZE * 4);
    centre = px + ((size_t)(PROBE_SIZE / 2) * PROBE_SIZE + PROBE_SIZE / 2) * 4;
    corner = px + ((size_t)(PROBE_SIZE - 3) * PROBE_SIZE + 2) * 4;
    probe_report("glprobe", crc, centre, corner);
    /* Half intensity is 127 or 128, as the host's GL rounds it. */
    ok = corner[0] == 0 && corner[1] == 0 && (corner[2] == 127 || corner[2] == 128) &&
         corner[3] == 255 && centre[3] == 255 && (centre[0] | centre[1]) != 0;
    bind_fb(PROBE_GL_FRAMEBUFFER, 0);
    draw();
    SwapBuffers(dc);
    glFinish();
    printf("draw %s\n", ok ? "OK" : "WRONG");
    if (ok)
        puts("GL OK");
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(w, dc);
    DestroyWindow(w);
    return ok ? 0 : 2;
}
