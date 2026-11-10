/* d3d9probe: draw a shaded triangle with Direct3D 9, read it back, sum it. */
#define COBJMACROS
#include "probe.h"
#include <d3d9.h>

/* A pre-transformed vertex: screen position and one colour. */
struct vtx {
    float x, y, z, rhw;
    DWORD c;
};

/** Stop with status 1 on a failed call, naming WHAT and its HRESULT. */
static void check(HRESULT hr, const char *what)
{
    if (FAILED(hr)) {
        printf("d3d9probe: %s: hr=0x%08lx\n", what, (unsigned long)hr);
        exit(1);
    }
}

/**
 * Create a Direct3D 9 device on a visible window, draw red, green and blue
 * corners over a navy clear into a 64x64 render target, read it back, then
 * copy it to the back buffer and present it. Prints the adapter, the texture
 * memory and the readback's checksum line. Returns 0 when the corner is the
 * clear colour and the centre is covered, 1 when a call fails, 2 when wrong.
 */
int main(void)
{
    struct vtx tri[3] = {
        {0.0f, 64.0f, 0.5f, 1.0f, 0xffff0000},
        {32.0f, 0.0f, 0.5f, 1.0f, 0xff00ff00},
        {64.0f, 64.0f, 0.5f, 1.0f, 0xff0000ff},
    };
    static unsigned char row[PROBE_SIZE * 4];
    unsigned char centre[4], corner[4];
    D3DADAPTER_IDENTIFIER9 id;
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice9 *dev = NULL;
    IDirect3DSurface9 *rt, *sys, *back;
    IDirect3D9 *d3d;
    D3DLOCKED_RECT lr;
    unsigned long crc = 0;
    const unsigned char *s;
    HRESULT hr;
    HWND w;
    int x, y, ok;

    probe_output();
    w = probe_window("d3d9probe");
    if (w == NULL) {
        printf("d3d9probe: no window\n");
        return 1;
    }
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (d3d == NULL) {
        printf("d3d9probe: Direct3DCreate9 failed\n");
        return 1;
    }
    check(IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &id), "GetAdapterIdentifier");
    printf("adapter: %s vendor 0x%04lx device 0x%04lx\n", id.Description, id.VendorId, id.DeviceId);
    memset(&pp, 0, sizeof pp);
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = pp.BackBufferHeight = PROBE_SIZE;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = w;
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    check(hr, "CreateDevice");
    printf("texture memory: %u MiB\n", IDirect3DDevice9_GetAvailableTextureMem(dev) >> 20);
    check(IDirect3DDevice9_CreateRenderTarget(dev, PROBE_SIZE, PROBE_SIZE, D3DFMT_A8R8G8B8,
                                              D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL),
          "CreateRenderTarget");
    check(IDirect3DDevice9_SetRenderTarget(dev, 0, rt), "SetRenderTarget");
    check(IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE), "SetRenderState");
    check(IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE), "SetRenderState");
    check(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE), "SetRenderState");
    check(IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000080, 1.0f, 0), "Clear");
    check(IDirect3DDevice9_BeginScene(dev), "BeginScene");
    check(IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE), "SetFVF");
    check(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof tri[0]),
          "DrawPrimitiveUP");
    check(IDirect3DDevice9_EndScene(dev), "EndScene");
    check(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, PROBE_SIZE, PROBE_SIZE, D3DFMT_A8R8G8B8,
                                                       D3DPOOL_SYSTEMMEM, &sys, NULL),
          "CreateOffscreenPlainSurface");
    check(IDirect3DDevice9_GetRenderTargetData(dev, rt, sys), "GetRenderTargetData");
    check(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY), "LockRect");
    /* A8R8G8B8 is stored B, G, R, A; sum it as R, G, B, A from the top. */
    for (y = 0; y < PROBE_SIZE; y++) {
        s = (const unsigned char *)lr.pBits + (size_t)y * lr.Pitch;
        for (x = 0; x < PROBE_SIZE; x++) {
            row[x * 4] = s[x * 4 + 2];
            row[x * 4 + 1] = s[x * 4 + 1];
            row[x * 4 + 2] = s[x * 4];
            row[x * 4 + 3] = s[x * 4 + 3];
        }
        crc = probe_crc32(crc, row, sizeof row);
        if (y == PROBE_SIZE / 2)
            memcpy(centre, row + (PROBE_SIZE / 2) * 4, 4);
        if (y == 2)
            memcpy(corner, row + 2 * 4, 4);
    }
    IDirect3DSurface9_UnlockRect(sys);
    probe_report("d3d9probe", crc, centre, corner);
    /* Half intensity is 127 or 128, as the host's GL rounds it. */
    ok = corner[0] == 0 && corner[1] == 0 && (corner[2] == 127 || corner[2] == 128) &&
         corner[3] == 255 && centre[3] == 255 && (centre[0] | centre[1]) != 0;
    check(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back),
          "GetBackBuffer");
    check(IDirect3DDevice9_StretchRect(dev, rt, NULL, back, NULL, D3DTEXF_NONE), "StretchRect");
    check(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL), "Present");
    printf("draw %s\n", ok ? "OK" : "WRONG");
    if (ok)
        puts("D3D9 OK");
    IDirect3DSurface9_Release(back);
    IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(rt);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    DestroyWindow(w);
    return ok ? 0 : 2;
}
