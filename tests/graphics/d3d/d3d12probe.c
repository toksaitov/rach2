/* d3d12probe: name the adapter, draw one triangle off screen, read it back. */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
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
        printf("d3d12probe: compile %s: hr=0x%08lx %s\n", entry, hr,
               err ? (char *)ID3D10Blob_GetBufferPointer(err) : "");
        return NULL;
    }
    return code;
}

static const float navy[4] = {0, 0, 0.5f, 1};

/** Create adapter 0's device and print its limits; the caller owns it. */
static ID3D12Device *make_device(void)
{
    static const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1,
                                             D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1,
                                             (D3D_FEATURE_LEVEL)0xc200};
    IDXGIFactory1 *f;
    IDXGIAdapter1 *a;
    DXGI_ADAPTER_DESC1 d;
    ID3D12Device *dev;
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl;
    D3D12_FEATURE_DATA_SHADER_MODEL sm;
    D3D12_FEATURE_DATA_D3D12_OPTIONS o;
    HRESULT hr;
    char name[128];
    int i;

    check(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f), "factory");
    if (FAILED(IDXGIFactory1_EnumAdapters1(f, 0, &a))) {
        printf("d3d12probe: no DXGI adapter\n");
        exit(1);
    }
    check(IDXGIAdapter1_GetDesc1(a, &d), "IDXGIAdapter1_GetDesc1");
    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, NULL, NULL);
    printf("adapter: %s vendor 0x%04x device 0x%04x dedicated %llu MiB "
           "shared %llu MiB\n",
           name, d.VendorId, d.DeviceId, (unsigned long long)d.DedicatedVideoMemory >> 20,
           (unsigned long long)d.SharedSystemMemory >> 20);
    hr = D3D12CreateDevice((IUnknown *)a, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
    printf("D3D12CreateDevice: hr=0x%08lx\n", hr);
    if (FAILED(hr))
        exit(1);
    fl.NumFeatureLevels = sizeof want / sizeof want[0];
    fl.pFeatureLevelsRequested = want;
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl);
    printf("max feature level: hr=0x%08lx 0x%x\n", hr,
           SUCCEEDED(hr) ? fl.MaxSupportedFeatureLevel : 0);
    for (i = 0x68; i >= 0x60; i--) {
        sm.HighestShaderModel = (D3D_SHADER_MODEL)i;
        hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm);
        if (SUCCEEDED(hr))
            break;
    }
    printf("highest shader model: hr=0x%08lx asked 0x%x got 0x%x\n", hr, i, sm.HighestShaderModel);
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o);
    printf("options: hr=0x%08lx binding tier %d tiled tier %d "
           "conservative raster tier %d heap tier %d typed uav loads %d\n",
           hr, SUCCEEDED(hr) ? o.ResourceBindingTier : 0, SUCCEEDED(hr) ? o.TiledResourcesTier : 0,
           SUCCEEDED(hr) ? o.ConservativeRasterizationTier : 0,
           SUCCEEDED(hr) ? o.ResourceHeapTier : 0,
           SUCCEEDED(hr) ? o.TypedUAVLoadAdditionalFormats : 0);

    return dev;
}

/** Create a pipeline and root signature, both owned by the caller. */
static ID3D12PipelineState *make_pipeline(ID3D12Device *dev, ID3D12RootSignature **root)
{
    ID3DBlob *sig, *err = NULL, *vsb, *psb;
    ID3D12RootSignature *rs;
    ID3D12PipelineState *pso;
    D3D12_ROOT_SIGNATURE_DESC rsd = {0, NULL, 0, NULL,
                                     D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    HRESULT hr;

    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    check(hr, "serialize root signature");
    check(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(sig),
                                           ID3D10Blob_GetBufferSize(sig), &IID_ID3D12RootSignature,
                                           (void **)&rs),
          "root signature");
    vsb = compile("vs", "vs_5_0");
    psb = compile("ps", "ps_5_0");
    if (!vsb || !psb)
        exit(1);
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = rs;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vsb);
    pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vsb);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(psb);
    pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(psb);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    pd.SampleMask = 0xffffffff;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    check(
        ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso),
        "pipeline state");

    *root = rs;
    return pso;
}

/** Create two 64x64 targets and their RTV, owned by the caller. */
static D3D12_CPU_DESCRIPTOR_HANDLE make_targets(ID3D12Device *dev, ID3D12Resource **target,
                                                ID3D12Resource **readback)
{
    ID3D12Resource *rt, *rb;
    ID3D12DescriptorHeap *heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC td, bd;
    D3D12_CLEAR_VALUE cv;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;

    memset(&td, 0, sizeof td);
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = td.Height = 64;
    td.DepthOrArraySize = td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    cv.Format = td.Format;
    memcpy(cv.Color, navy, sizeof navy);
    check(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                               &IID_ID3D12Resource, (void **)&rt),
          "render target");
    memset(&bd, 0, sizeof bd);
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256 * 64;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    check(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                                               D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                               &IID_ID3D12Resource, (void **)&rb),
          "readback buffer");
    check(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap),
          "rtv heap");
    rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);

    *target = rt;
    *readback = rb;
    return rtv;
}

/** Draw and copy the target, then wait for the queue's fence. */
static void draw(ID3D12Device *dev, ID3D12PipelineState *pso, ID3D12RootSignature *rs,
                 ID3D12Resource *rt, ID3D12Resource *rb, D3D12_CPU_DESCRIPTOR_HANDLE rtv)
{
    ID3D12CommandQueue *q;
    ID3D12CommandAllocator *ca;
    ID3D12GraphicsCommandList *cl;
    ID3D12Fence *fence;
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE,
                                   0};
    D3D12_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
    D3D12_RECT sc = {0, 0, 64, 64};
    D3D12_RESOURCE_BARRIER b;
    D3D12_TEXTURE_COPY_LOCATION dst, srcl;
    HANDLE ev;

    check(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q), "queue");
    check(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              &IID_ID3D12CommandAllocator, (void **)&ca),
          "allocator");
    check(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, pso,
                                         &IID_ID3D12GraphicsCommandList, (void **)&cl),
          "command list");
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv, navy, 0, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rs);
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
    memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = rt;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
    memset(&dst, 0, sizeof dst);
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = 64;
    dst.PlacedFootprint.Footprint.Height = 64;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = 256;
    memset(&srcl, 0, sizeof srcl);
    srcl.pResource = rt;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcl.SubresourceIndex = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &srcl, NULL);
    check(ID3D12GraphicsCommandList_Close(cl), "close");
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList **)&cl);
    check(
        ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence),
        "fence");
    check(ID3D12CommandQueue_Signal(q, fence, 1), "signal");
    ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (ev == NULL)
        exit(1);
    check(ID3D12Fence_SetEventOnCompletion(fence, 1, ev), "event");
    if (WaitForSingleObject(ev, INFINITE) != WAIT_OBJECT_0) {
        printf("d3d12probe: fence wait failed, value %llu\n",
               (unsigned long long)ID3D12Fence_GetCompletedValue(fence));
        exit(1);
    }
    CloseHandle(ev);
}

/** Return 0 for the drawn pixels, 1 for an API error, 2 for wrong pixels. */
int main(void)
{
    ID3D12Device *dev;
    ID3D12RootSignature *rs;
    ID3D12PipelineState *pso;
    ID3D12Resource *rt, *rb;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    D3D12_RANGE r = {0, 256 * 64};
    unsigned char *px, *c, *k;
    int ok;

    probe_output();
    dev = make_device();
    pso = make_pipeline(dev, &rs);
    rtv = make_targets(dev, &rt, &rb);
    draw(dev, pso, rs, rt, rb, rtv);
    check(ID3D12Resource_Map(rb, 0, &r, (void **)&px), "map");
    /* Row 48 is inside the triangle; row 2, column 2 is the clear colour. */
    c = px + 48 * 256 + 32 * 4;
    k = px + 2 * 256 + 2 * 4;
    ok = c[0] == 255 && (c[1] == 127 || c[1] == 128) && c[2] == 0 && c[3] == 255 && k[0] == 0 &&
         k[1] == 0 && (k[2] == 127 || k[2] == 128) && k[3] == 255;
    printf("center %d %d %d %d corner %d %d %d %d draw %s\n", c[0], c[1], c[2], c[3], k[0], k[1],
           k[2], k[3], ok ? "OK" : "WRONG");
    if (ok)
        puts("D3D12 OK");
    ID3D12Resource_Unmap(rb, 0, NULL);
    return ok ? 0 : 2;
}
