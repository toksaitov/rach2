/* d3d9probe: name the adapter, draw one triangle off screen, read it back. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include "probe.h"

/* A pre-transformed vertex: screen position and one colour. */
struct vtx {
    float x, y, z, rhw;
    DWORD c;
};

/**
 * Create a Direct3D 9 device on a hidden 64x64 window, draw an orange triangle
 * over a navy clear into a render target, and read two pixels back.
 * Returns 0 when both pixels are as drawn, 1 on an API failure, 2 when wrong.
 */
int main(void)
{
    struct vtx tri[3] = {
        {0.0f, 64.0f, 0.5f, 1.0f, 0xffff8000},
        {32.0f, 0.0f, 0.5f, 1.0f, 0xffff8000},
        {64.0f, 64.0f, 0.5f, 1.0f, 0xffff8000},
    };
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = NULL;
    D3DADAPTER_IDENTIFIER9 id;
    D3DPRESENT_PARAMETERS pp;
    D3DCAPS9 caps;
    IDirect3DSurface9 *rt, *sys;
    D3DLOCKED_RECT lr;
    unsigned char *c, *k;
    HWND w;
    HRESULT hr;
    int ok;

    probe_output();
    w = CreateWindowA("STATIC", "d3d9probe", WS_POPUP, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        printf("d3d9probe: Direct3DCreate9 failed\n");
        return 1;
    }
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &id)))
        printf("adapter: %s driver %s vendor 0x%04lx device 0x%04lx\n", id.Description, id.Driver,
               id.VendorId, id.DeviceId);
    memset(&pp, 0, sizeof pp);
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = w;
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr))
        hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    printf("CreateDevice: hr=0x%08lx\n", hr);
    if (FAILED(hr))
        return 1;
    check(IDirect3DDevice9_GetDeviceCaps(dev, &caps), "IDirect3DDevice9_GetDeviceCaps");
    printf("caps: vs %lu.%lu ps %lu.%lu texture memory %u MiB\n",
           (caps.VertexShaderVersion >> 8) & 0xff, caps.VertexShaderVersion & 0xff,
           (caps.PixelShaderVersion >> 8) & 0xff, caps.PixelShaderVersion & 0xff,
           IDirect3DDevice9_GetAvailableTextureMem(dev) >> 20);
    hr = IDirect3DDevice9_CreateRenderTarget(dev, 64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0,
                                             FALSE, &rt, NULL);
    if (FAILED(hr)) {
        printf("d3d9probe: CreateRenderTarget: hr=0x%08lx\n", hr);
        return 1;
    }
    check(IDirect3DDevice9_SetRenderTarget(dev, 0, rt), "IDirect3DDevice9_SetRenderTarget");
    check(IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE),
          "IDirect3DDevice9_SetRenderState");
    check(IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE),
          "IDirect3DDevice9_SetRenderState");
    check(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE),
          "IDirect3DDevice9_SetRenderState");
    check(IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000080, 1.0f, 0),
          "IDirect3DDevice9_Clear");
    check(IDirect3DDevice9_BeginScene(dev), "IDirect3DDevice9_BeginScene");
    check(IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE), "IDirect3DDevice9_SetFVF");
    hr = IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof tri[0]);
    check(IDirect3DDevice9_EndScene(dev), "IDirect3DDevice9_EndScene");
    if (FAILED(hr)) {
        printf("d3d9probe: DrawPrimitiveUP: hr=0x%08lx\n", hr);
        return 1;
    }
    check(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 64, 64, D3DFMT_A8R8G8B8,
                                                       D3DPOOL_SYSTEMMEM, &sys, NULL),
          "IDirect3DDevice9_CreateOffscreenPlainSurface");
    hr = IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
    if (FAILED(hr) || FAILED(hr = IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        printf("d3d9probe: readback: hr=0x%08lx\n", hr);
        return 1;
    }
    /* A8R8G8B8 is stored B, G, R, A; row 48 is inside the triangle. */
    c = (unsigned char *)lr.pBits + 48 * lr.Pitch + 32 * 4;
    k = (unsigned char *)lr.pBits + 2 * lr.Pitch + 2 * 4;
    ok = c[2] == 255 && c[1] == 128 && c[0] == 0 && c[3] == 255 && k[2] == 0 && k[1] == 0 &&
         k[0] == 128 && k[3] == 255;
    printf("center %d %d %d %d corner %d %d %d %d draw %s\n", c[2], c[1], c[0], c[3], k[2], k[1],
           k[0], k[3], ok ? "OK" : "WRONG");
    if (ok)
        puts("D3D9 OK");
    IDirect3DSurface9_UnlockRect(sys);
    return ok ? 0 : 2;
}
