/* fullprobe: hold a Direct3D 9 fullscreen device, as a game does, until told */
#define COBJMACROS
#include "probe.h"
#include <d3d9.h>

/** Stop with status 1 on a failed call, naming WHAT and its HRESULT. */
static void check(HRESULT hr, const char *what)
{
    if (FAILED(hr)) {
        printf("fullprobe: %s: hr=0x%08lx\n", what, (unsigned long)hr);
        exit(1);
    }
}

/** Dispatch the thread's queued messages. */
static void pump(void)
{
    MSG msg;

    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/**
 * Usage: fullprobe.exe WIDTH HEIGHT. Make a WS_POPUP window and a Direct3D 9
 * device on it with Windowed false at WIDTH by HEIGHT, as a game's exclusive
 * fullscreen is, so that WineD3D sets the display mode, the window's style
 * and its place. Present frames with the left half red and the right half
 * green, print "full: ready" with the window's rectangle once the first is
 * presented, and go on until the file PROBE_GO names exists. Returns 0, or 1
 * on a failed call or without PROBE_GO.
 */
int main(int argc, char **argv)
{
    const char *go = getenv("PROBE_GO");
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice9 *dev = NULL;
    IDirect3D9 *d3d;
    WNDCLASSA wc;
    D3DRECT half[2];
    RECT r;
    HWND w;
    int width, height, ready = 0;

    probe_output();
    if (argc != 3 || go == NULL) {
        printf("fullprobe: usage: fullprobe.exe WIDTH HEIGHT, with PROBE_GO\n");
        return 1;
    }
    width = atoi(argv[1]);
    height = atoi(argv[2]);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "fullprobe";
    if (!RegisterClassA(&wc))
        return 1;
    w = CreateWindowA("fullprobe", "fullprobe", WS_POPUP | WS_VISIBLE, 0, 0, width, height, NULL,
                      NULL, wc.hInstance, NULL);
    if (w == NULL)
        return 1;
    pump();
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (d3d == NULL) {
        printf("fullprobe: Direct3DCreate9 failed\n");
        return 1;
    }
    memset(&pp, 0, sizeof pp);
    pp.Windowed = FALSE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = (UINT)width;
    pp.BackBufferHeight = (UINT)height;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.FullScreen_RefreshRateInHz = 0;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
    pp.hDeviceWindow = w;
    check(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w,
                                  D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev),
          "CreateDevice");
    half[0].x1 = 0, half[0].y1 = 0, half[0].x2 = width / 2, half[0].y2 = height;
    half[1].x1 = width / 2, half[1].y1 = 0, half[1].x2 = width, half[1].y2 = height;
    while (GetFileAttributesA(go) == INVALID_FILE_ATTRIBUTES) {
        check(IDirect3DDevice9_Clear(dev, 1, &half[0], D3DCLEAR_TARGET, 0xffff0000, 1.0f, 0),
              "Clear");
        check(IDirect3DDevice9_Clear(dev, 1, &half[1], D3DCLEAR_TARGET, 0xff00ff00, 1.0f, 0),
              "Clear");
        check(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL), "Present");
        pump();
        if (!ready) {
            GetWindowRect(w, &r);
            printf("full: ready window %ld,%ld %ldx%ld screen %dx%d\n", r.left, r.top,
                   r.right - r.left, r.bottom - r.top, GetSystemMetrics(SM_CXSCREEN),
                   GetSystemMetrics(SM_CYSCREEN));
            ready = 1;
        }
        Sleep(16);
    }
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    puts("FULL OK");
    return 0;
}
