/* tripprobe: time 1000 Direct3D 11 round trips of a cleared target. */
#define COBJMACROS
#define INITGUID
#include "probe.h"
#include <d3d11.h>
#include <dxgi.h>

/* The round trips timed, as in the second graphics run's test under Linux. */
#define TRIPS 1000

/** Stop with status 1 on a failed call, naming WHAT and its HRESULT. */
static void check(HRESULT hr, const char *what)
{
    if (FAILED(hr)) {
        printf("tripprobe: %s: hr=0x%08lx\n", what, (unsigned long)hr);
        exit(1);
    }
}

/**
 * Create a Direct3D 11 device on the first DXGI adapter and, TRIPS times,
 * clear a 64x64 render target to green, copy it to a staging texture and map
 * that for reading. Prints the adapter, the feature level, the last pixel
 * read and the time of the round trips in milliseconds. Returns 0 when every
 * pixel read is green, 1 when a call fails, 2 when one is wrong.
 */
int main(void)
{
    static const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D_FEATURE_LEVEL fl = 0;
    DXGI_ADAPTER_DESC1 d;
    LARGE_INTEGER freq, t0, t1;
    IDXGIFactory1 *f;
    IDXGIAdapter1 *a;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11Texture2D *rt, *st;
    ID3D11RenderTargetView *rtv;
    const unsigned char *px = NULL;
    unsigned char last[4] = {0, 0, 0, 0};
    char name[128];
    int i, wrong = 0;

    probe_output();
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f)) ||
        FAILED(IDXGIFactory1_EnumAdapters1(f, 0, &a))) {
        printf("tripprobe: no DXGI adapter\n");
        return 1;
    }
    check(IDXGIAdapter1_GetDesc1(a, &d), "GetDesc1");
    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, NULL, NULL);
    printf("adapter: %s vendor 0x%04x device 0x%04x\n", name, d.VendorId, d.DeviceId);
    check(D3D11CreateDevice((IDXGIAdapter *)a, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
                            D3D11_SDK_VERSION, &dev, &fl, &ctx),
          "D3D11CreateDevice");
    printf("feature level: 0x%x\n", fl);
    memset(&td, 0, sizeof td);
    td.Width = td.Height = PROBE_SIZE;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    check(ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt), "CreateTexture2D");
    check(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv),
          "CreateRenderTargetView");
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(ID3D11Device_CreateTexture2D(dev, &td, NULL, &st), "CreateTexture2D");
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (i = 0; i < TRIPS; i++) {
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, green);
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)rt);
        check(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m), "Map");
        px = (const unsigned char *)m.pData + (PROBE_SIZE - 1) * m.RowPitch + (PROBE_SIZE - 1) * 4;
        memcpy(last, px, 4);
        if (last[0] != 0 || last[1] != 255 || last[2] != 0 || last[3] != 255)
            wrong++;
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    }
    QueryPerformanceCounter(&t1);
    printf("tripprobe: %d round trips in %.1f ms, last pixel %d %d %d %d, %d wrong\n", TRIPS,
           (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart, last[0], last[1],
           last[2], last[3], wrong);
    if (wrong == 0)
        puts("TRIP OK");
    ID3D11Texture2D_Release(st);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(rt);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    IDXGIAdapter1_Release(a);
    IDXGIFactory1_Release(f);
    return wrong == 0 ? 0 : 2;
}
