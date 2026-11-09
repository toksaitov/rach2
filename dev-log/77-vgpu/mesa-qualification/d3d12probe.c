/*
 * d3d12probe.c: makes a Direct3D 12 device on the first DXGI adapter, reports
 * its levels and tiers, draws an orange triangle on a navy 64x64 target and
 * reads two pixels back.
 *
 * Prints "draw OK" when (32,48) is 255,128,0 and (2,2) is 0,0,128, each within
 * 1; output also goes to the file PROBE_OUT names. Build:
 * x86_64-w64-mingw32-gcc -O2 d3d12probe.c -ld3d12 -ldxgi -ld3dcompiler_47
 * (or i686-w64-mingw32-gcc for the 32-bit program).
 */
#define COBJMACROS
#define INITGUID
/* mingw-w64 12's headers give C callers struct-returning methods only so. */
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
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
    say("d3d12probe: %s failed, hr=0x%08lx\n", what, (unsigned long)hr);
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
            say("d3d12probe: %s\n", (char *)ID3D10Blob_GetBufferPointer(err));
        die("D3DCompile", hr);
    }
    return code;
}

/** Return 1 when the pixel's red, green and blue are each within 1. */
static int close_to(const unsigned char *px, int r, int g, int b)
{
    return abs(px[0] - r) <= 1 && abs(px[1] - g) <= 1 && abs(px[2] - b) <= 1;
}

/** Print the highest feature level, shader model and the main tiers. */
static void caps(ID3D12Device *dev)
{
    static const D3D_FEATURE_LEVEL want[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};
    static const D3D_SHADER_MODEL models[] = {
        D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5,
        D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2,
        D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0, D3D_SHADER_MODEL_5_1};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fls;
    D3D12_FEATURE_DATA_SHADER_MODEL sm;
    D3D12_FEATURE_DATA_D3D12_OPTIONS o;
    unsigned i;

    fls.NumFeatureLevels = 5;
    fls.pFeatureLevelsRequested = want;
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(
            dev, D3D12_FEATURE_FEATURE_LEVELS, &fls, sizeof fls)))
        say("d3d12probe: highest level %d_%d\n", fls.MaxSupportedFeatureLevel >> 12,
            (fls.MaxSupportedFeatureLevel >> 8) & 0xf);
    for (i = 0; i < sizeof models / sizeof models[0]; i++) {
        sm.HighestShaderModel = models[i];
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(
                dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm))) {
            say("d3d12probe: shader model %x.%x\n", sm.HighestShaderModel >> 4,
                sm.HighestShaderModel & 0xf);
            break;
        }
    }
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS,
                                                   &o, sizeof o)))
        say("d3d12probe: binding tier %d, tiled tier %d, heap tier %d, "
            "typed UAV loads %d\n",
            o.ResourceBindingTier, o.TiledResourcesTier, o.ResourceHeapTier,
            o.TypedUAVLoadAdditionalFormats);
}

/** Make a committed resource on a heap of the given type. */
static ID3D12Resource *resource(ID3D12Device *dev, D3D12_HEAP_TYPE type,
                                const D3D12_RESOURCE_DESC *rd,
                                D3D12_RESOURCE_STATES state,
                                const D3D12_CLEAR_VALUE *clear)
{
    D3D12_HEAP_PROPERTIES hp;
    ID3D12Resource *r;
    HRESULT hr;

    ZeroMemory(&hp, sizeof hp);
    hp.Type = type;
    hr = ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, rd,
                                              state, clear, &IID_ID3D12Resource,
                                              (void **)&r);
    if (FAILED(hr))
        die("CreateCommittedResource", hr);
    return r;
}

/** Record a whole-resource transition barrier. */
static void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
                    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b;

    ZeroMemory(&b, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
}

int main(void)
{
    static const float navy[4] = {0.0f, 0.0f, 0.5f, 1.0f};
    const char *path = getenv("PROBE_OUT");
    IDXGIFactory4 *factory;
    IDXGIAdapter1 *adapter;
    DXGI_ADAPTER_DESC1 ad;
    ID3D12Device *dev;
    ID3D12CommandQueue *q;
    ID3D12CommandAllocator *alloc;
    ID3D12GraphicsCommandList *cl;
    ID3D12DescriptorHeap *heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    D3D12_COMMAND_QUEUE_DESC qd;
    D3D12_RESOURCE_DESC rd;
    D3D12_CLEAR_VALUE cv;
    ID3D12Resource *rt, *rb;
    D3D12_ROOT_SIGNATURE_DESC rsd;
    ID3DBlob *sig, *err, *vsb, *psb;
    ID3D12RootSignature *root;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    ID3D12PipelineState *pso;
    D3D12_VIEWPORT vp;
    D3D12_RECT sc;
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    ID3D12Fence *fence;
    HANDLE ev;
    D3D12_RANGE range;
    unsigned char *p;
    HRESULT hr;
    int ok;

    out = path ? fopen(path, "w") : NULL;
    say("d3d12probe: %d-bit\n", (int)(sizeof(void *) * 8));
    hr = CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory);
    if (FAILED(hr))
        die("CreateDXGIFactory2", hr);
    hr = IDXGIFactory4_EnumAdapters1(factory, 0, &adapter);
    if (FAILED(hr)) {
        say("d3d12probe: no DXGI adapter\n");
        return 1;
    }
    IDXGIAdapter1_GetDesc1(adapter, &ad);
    say("d3d12probe: adapter \"%ls\" vendor 0x%04x device 0x%04x, %lu MiB "
        "dedicated\n",
        ad.Description, ad.VendorId, ad.DeviceId,
        (unsigned long)(ad.DedicatedVideoMemory >> 20));
    hr = D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0,
                           &IID_ID3D12Device, (void **)&dev);
    if (FAILED(hr))
        die("D3D12CreateDevice", hr);
    caps(dev);

    ZeroMemory(&qd, sizeof qd);
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue,
                                         (void **)&q);
    if (FAILED(hr))
        die("CreateCommandQueue", hr);
    hr = ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             &IID_ID3D12CommandAllocator,
                                             (void **)&alloc);
    if (FAILED(hr))
        die("CreateCommandAllocator", hr);

    ZeroMemory(&hd, sizeof hd);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    hr = ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap,
                                           (void **)&heap);
    if (FAILED(hr))
        die("CreateDescriptorHeap", hr);
    rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);

    ZeroMemory(&rd, sizeof rd);
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = rd.Height = 64;
    rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    cv.Format = rd.Format;
    memcpy(cv.Color, navy, sizeof navy);
    rt = resource(dev, D3D12_HEAP_TYPE_DEFAULT, &rd,
                  D3D12_RESOURCE_STATE_RENDER_TARGET, &cv);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    /* 64 pixels of 4 bytes make a 256-byte row, the copy's required pitch. */
    ZeroMemory(&rd, sizeof rd);
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 64 * 256;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rb = resource(dev, D3D12_HEAP_TYPE_READBACK, &rd,
                  D3D12_RESOURCE_STATE_COPY_DEST, NULL);

    ZeroMemory(&rsd, sizeof rsd);
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig,
                                     &err);
    if (FAILED(hr))
        die("D3D12SerializeRootSignature", hr);
    hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(sig),
                                          ID3D10Blob_GetBufferSize(sig),
                                          &IID_ID3D12RootSignature,
                                          (void **)&root);
    if (FAILED(hr))
        die("CreateRootSignature", hr);
    vsb = compile("vs", "vs_5_0");
    psb = compile("ps", "ps_5_0");
    ZeroMemory(&pd, sizeof pd);
    pd.pRootSignature = root;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vsb);
    pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vsb);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(psb);
    pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(psb);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xffffffffu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd,
                                                  &IID_ID3D12PipelineState,
                                                  (void **)&pso);
    if (FAILED(hr))
        die("CreateGraphicsPipelineState", hr);

    hr = ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        alloc, pso,
                                        &IID_ID3D12GraphicsCommandList,
                                        (void **)&cl);
    if (FAILED(hr))
        die("CreateCommandList", hr);
    vp.TopLeftX = vp.TopLeftY = 0.0f;
    vp.Width = vp.Height = 64.0f;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    sc.left = sc.top = 0;
    sc.right = sc.bottom = 64;
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, root);
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv, navy, 0, NULL);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(
        cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
    barrier(cl, rt, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    ZeroMemory(&dst, sizeof dst);
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = 64;
    dst.PlacedFootprint.Footprint.Height = 64;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = 256;
    ZeroMemory(&src, sizeof src);
    src.pResource = rt;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    hr = ID3D12GraphicsCommandList_Close(cl);
    if (FAILED(hr))
        die("Close", hr);
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList **)&cl);

    hr = ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
                                  (void **)&fence);
    if (FAILED(hr))
        die("CreateFence", hr);
    ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12CommandQueue_Signal(q, fence, 1);
    ID3D12Fence_SetEventOnCompletion(fence, 1, ev);
    if (WaitForSingleObject(ev, 30000) != WAIT_OBJECT_0) {
        say("d3d12probe: fence wait timed out\n");
        return 1;
    }

    range.Begin = 0;
    range.End = 64 * 256;
    hr = ID3D12Resource_Map(rb, 0, &range, (void **)&p);
    if (FAILED(hr))
        die("Map", hr);
    say("d3d12probe: (32,48) %d %d %d %d, (2,2) %d %d %d %d\n",
        p[48 * 256 + 128], p[48 * 256 + 129], p[48 * 256 + 130],
        p[48 * 256 + 131], p[2 * 256 + 8], p[2 * 256 + 9], p[2 * 256 + 10],
        p[2 * 256 + 11]);
    ok = close_to(p + 48 * 256 + 128, 255, 128, 0) && close_to(p + 2 * 256 + 8, 0, 0, 128);
    range.End = 0;
    ID3D12Resource_Unmap(rb, 0, &range);
    say("d3d12probe: %s\n", ok ? "draw OK" : "draw FAIL");
    if (out)
        fclose(out);
    return ok ? 0 : 2;
}
