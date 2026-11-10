/* d3d11probe: name the adapter, draw one triangle off screen, read it back. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include "probe.h"

static const char src[] = "float4 vs(uint i : SV_VertexID) : SV_Position {\n"
                          "  float2 p[3] = { float2(-1, -1), float2(0, 1), float2(1, -1) };\n"
                          "  return float4(p[i], 0, 1);\n"
                          "}\n"
                          "float4 ps() : SV_Target { return float4(1, 0.5, 0, 1); }\n";

/**
 * Compile one entry point of src for the given target.
 * Returns the blob, or NULL after printing the compiler's message.
 */
static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr =
        D3DCompile(src, sizeof src - 1, "probe", NULL, NULL, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        printf("d3d11probe: compile %s: hr=0x%08lx %s\n", entry, hr,
               err ? (char *)ID3D10Blob_GetBufferPointer(err) : "");
        return NULL;
    }
    return code;
}

/** Draw two pixels; return 0 on a match, 1 on an API error, 2 otherwise. */
int main(void)
{
    IDXGIFactory1 *f;
    IDXGIAdapter1 *a;
    DXGI_ADAPTER_DESC1 d;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    D3D_FEATURE_LEVEL fl = 0;
    HRESULT hr;
    char name[128];
    unsigned char *px;

    probe_output();
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f)) ||
        FAILED(IDXGIFactory1_EnumAdapters1(f, 0, &a))) {
        printf("d3d11probe: no DXGI adapter\n");
        return 1;
    }
    check(IDXGIAdapter1_GetDesc1(a, &d), "IDXGIAdapter1_GetDesc1");
    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, NULL, NULL);
    printf("adapter: %s vendor 0x%04x device 0x%04x dedicated %llu MiB "
           "shared %llu MiB\n",
           name, d.VendorId, d.DeviceId, (unsigned long long)d.DedicatedVideoMemory >> 20,
           (unsigned long long)d.SharedSystemMemory >> 20);
    hr = D3D11CreateDevice((IDXGIAdapter *)a, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
                           D3D11_SDK_VERSION, &dev, &fl, &ctx);
    printf("D3D11CreateDevice: hr=0x%08lx feature level 0x%x\n", hr, fl);
    if (FAILED(hr))
        return 1;

    ID3DBlob *vsb = compile("vs", "vs_4_0"), *psb = compile("ps", "ps_4_0");
    if (!vsb || !psb)
        return 1;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    check(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb),
                                          ID3D10Blob_GetBufferSize(vsb), NULL, &vs),
          "ID3D11Device_CreateVertexShader");
    check(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb),
                                         ID3D10Blob_GetBufferSize(psb), NULL, &ps),
          "ID3D11Device_CreatePixelShader");

    D3D11_TEXTURE2D_DESC td = {64,
                               64,
                               1,
                               1,
                               DXGI_FORMAT_R8G8B8A8_UNORM,
                               {1, 0},
                               D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET,
                               0,
                               0};
    ID3D11Texture2D *rt, *st;
    ID3D11RenderTargetView *rtv;
    check(ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt), "ID3D11Device_CreateTexture2D");
    check(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv),
          "ID3D11Device_CreateRenderTargetView");
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(ID3D11Device_CreateTexture2D(dev, &td, NULL, &st), "ID3D11Device_CreateTexture2D");

    const float clear[4] = {0, 0, 0.5f, 1};
    D3D11_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)rt);

    D3D11_MAPPED_SUBRESOURCE m;
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        printf("d3d11probe: map: hr=0x%08lx\n", hr);
        return 1;
    }
    px = m.pData;
    /* Row 48 is inside the triangle; row 2, column 2 is the clear colour. */
    unsigned char *c = px + 48 * m.RowPitch + 32 * 4;
    unsigned char *k = px + 2 * m.RowPitch + 2 * 4;
    int ok = c[0] == 255 && (c[1] == 127 || c[1] == 128) && c[2] == 0 && c[3] == 255 && k[0] == 0 &&
             k[1] == 0 && (k[2] == 127 || k[2] == 128) && k[3] == 255;
    printf("center %d %d %d %d corner %d %d %d %d draw %s\n", c[0], c[1], c[2], c[3], k[0], k[1],
           k[2], k[3], ok ? "OK" : "WRONG");
    if (ok)
        puts("D3D11 OK");
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    return ok ? 0 : 2;
}
