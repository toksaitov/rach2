/*
 * d3d11probe.c: makes a Direct3D 11 device on the first DXGI adapter, draws an
 * orange triangle on a navy 64x64 target and reads two pixels back.
 *
 * Prints the adapter and the feature level, then "draw OK" when (32,48) is
 * 255,128,0 and (2,2) is 0,0,128, each within 1. Output also goes to the file
 * PROBE_OUT names, since Proton does not pass a program's stdout through.
 * Build: x86_64-w64-mingw32-gcc -O2 d3d11probe.c -ld3d11 -ldxgi
 * -ld3dcompiler_47 (or i686-w64-mingw32-gcc for the 32-bit program).
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static FILE *out;

/** Print a line to stdout and to PROBE_OUT. */
static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (out) {
        va_start(ap, fmt);
        vfprintf(out, fmt, ap);
        va_end(ap);
        fflush(out);
    }
}

/** Report a failed call and exit with status 1. */
static void die(const char *what, HRESULT hr)
{
    say("d3d11probe: %s failed, hr=0x%08lx\n", what, (unsigned long)hr);
    exit(1);
}

static const char shader[] =
    "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p[3] = { float2(0, 1), float2(1, -1), float2(-1, -1) };\n"
    "  return float4(p[id], 0, 1);\n"
    "}\n"
    "float4 ps() : SV_Target { return float4(1, 0.5, 0, 1); }\n";

/** Compile one entry point of shader[] for the given profile. */
static ID3DBlob *compile(const char *entry, const char *profile)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr;

    hr = D3DCompile(shader, sizeof shader - 1, "probe", NULL, NULL, entry,
                    profile, 0, 0, &code, &err);
    if (FAILED(hr)) {
        if (err)
            say("d3d11probe: %s\n", (char *)ID3D10Blob_GetBufferPointer(err));
        die("D3DCompile", hr);
    }
    return code;
}

/** Return 1 when the pixel's red, green and blue are each within 1. */
static int close_to(const unsigned char *px, int r, int g, int b)
{
    return abs(px[0] - r) <= 1 && abs(px[1] - g) <= 1 && abs(px[2] - b) <= 1;
}

int main(void)
{
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    static const float navy[4] = {0.0f, 0.0f, 0.5f, 1.0f};
    const char *path = getenv("PROBE_OUT");
    IDXGIFactory1 *factory;
    IDXGIAdapter1 *adapter;
    DXGI_ADAPTER_DESC1 ad;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    D3D_FEATURE_LEVEL fl;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *rt, *staging;
    ID3D11RenderTargetView *rtv;
    ID3DBlob *vsb, *psb;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    D3D11_RASTERIZER_DESC rd;
    ID3D11RasterizerState *rs;
    D3D11_VIEWPORT vp;
    D3D11_MAPPED_SUBRESOURCE map;
    const unsigned char *a, *b;
    HRESULT hr;
    int ok;

    out = path ? fopen(path, "w") : NULL;
    say("d3d11probe: %d-bit\n", (int)(sizeof(void *) * 8));
    hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
    if (FAILED(hr))
        die("CreateDXGIFactory1", hr);
    hr = IDXGIFactory1_EnumAdapters1(factory, 0, &adapter);
    if (FAILED(hr)) {
        say("d3d11probe: no DXGI adapter\n");
        return 1;
    }
    IDXGIAdapter1_GetDesc1(adapter, &ad);
    say("d3d11probe: adapter \"%ls\" vendor 0x%04x device 0x%04x, %lu MiB "
        "dedicated\n",
        ad.Description, ad.VendorId, ad.DeviceId,
        (unsigned long)(ad.DedicatedVideoMemory >> 20));
    hr = D3D11CreateDevice((IDXGIAdapter *)adapter, D3D_DRIVER_TYPE_UNKNOWN,
                           NULL, 0, levels, 4, D3D11_SDK_VERSION, &dev, &fl,
                           &ctx);
    if (FAILED(hr))
        die("D3D11CreateDevice", hr);
    say("d3d11probe: feature level %d_%d\n", fl >> 12, (fl >> 8) & 0xf);

    ZeroMemory(&td, sizeof td);
    td.Width = td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt);
    if (FAILED(hr))
        die("CreateTexture2D", hr);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &staging);
    if (FAILED(hr))
        die("CreateTexture2D staging", hr);
    hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL,
                                             &rtv);
    if (FAILED(hr))
        die("CreateRenderTargetView", hr);

    vsb = compile("vs", "vs_4_0");
    psb = compile("ps", "ps_4_0");
    hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb),
                                         ID3D10Blob_GetBufferSize(vsb), NULL,
                                         &vs);
    if (FAILED(hr))
        die("CreateVertexShader", hr);
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb),
                                        ID3D10Blob_GetBufferSize(psb), NULL,
                                        &ps);
    if (FAILED(hr))
        die("CreatePixelShader", hr);
    ZeroMemory(&rd, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = ID3D11Device_CreateRasterizerState(dev, &rd, &rs);
    if (FAILED(hr))
        die("CreateRasterizerState", hr);

    vp.TopLeftX = vp.TopLeftY = 0.0f;
    vp.Width = vp.Height = 64.0f;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, rs);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, navy);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging,
                                     (ID3D11Resource *)rt);
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &map);
    if (FAILED(hr))
        die("Map", hr);
    a = (const unsigned char *)map.pData + 48 * map.RowPitch + 32 * 4;
    b = (const unsigned char *)map.pData + 2 * map.RowPitch + 2 * 4;
    say("d3d11probe: (32,48) %d %d %d %d, (2,2) %d %d %d %d\n", a[0], a[1],
        a[2], a[3], b[0], b[1], b[2], b[3]);
    ok = close_to(a, 255, 128, 0) && close_to(b, 0, 0, 128);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    say("d3d11probe: %s\n", ok ? "draw OK" : "draw FAIL");
    if (out)
        fclose(out);
    return ok ? 0 : 2;
}
