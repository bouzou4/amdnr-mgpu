// Frame dumps for the image checks: a texture to .raw (tight rows) and an 8-bit .bmp.
#include "MgpuCommon.h"
#include "MgpuCore.h"
#include "../GpuWait.h"

#include <wrl/client.h>
#include <fstream>
#include <vector>
#include <format>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace Mgpu
{
static float HalfToFloat(uint16_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? std::ldexp((float) m, -24) : e == 31 ? 65504.f : std::ldexp((float) (m | 1024), (int) e - 25);
    return s ? -v : v;
}

bool DumpTexture(ID3D12Device* dev, ID3D12CommandQueue* q, ID3D12Resource* tex, D3D12_RESOURCE_STATES state,
                 const std::wstring& base)
{
    auto d = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC b {};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = total;
    b.Height = b.DepthOrArraySize = b.MipLevels = 1;
    b.SampleDesc.Count = 1;
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rb;
    ComPtr<ID3D12CommandAllocator> a;
    ComPtr<ID3D12GraphicsCommandList> l;
    ComPtr<ID3D12Fence> f;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&rb))) ||
        FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&l))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f))))
        return false;
    D3D12_RESOURCE_BARRIER br {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition = { tex, 0, state, D3D12_RESOURCE_STATE_COPY_SOURCE };
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
        l->ResourceBarrier(1, &br);
    D3D12_TEXTURE_COPY_LOCATION dst { rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    src.SubresourceIndex = 0;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(br.Transition.StateBefore, br.Transition.StateAfter);
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
        l->ResourceBarrier(1, &br);
    l->Close();
    ID3D12CommandList* ls[] = { l.Get() };
    q->ExecuteCommandLists(1, ls);
    q->Signal(f.Get(), 1);
    if (!GpuWaitDone(f.Get(), 1))
    {
        // still running: the list, allocator and readback must outlive it
        static std::vector<ComPtr<IUnknown>> s_kept;
        s_kept.push_back(rb), s_kept.push_back(a), s_kept.push_back(l), s_kept.push_back(f);
        return false;
    }
    uint8_t* p = nullptr;
    if (FAILED(rb->Map(0, nullptr, (void**) &p)))
        return false;
    const UINT w = (UINT) d.Width, h = d.Height;
    {
        std::ofstream raw(base + L".raw", std::ios::binary);
        for (UINT y = 0; y < h; ++y)
            raw.write((const char*) p + fp.Offset + UINT64(y) * fp.Footprint.RowPitch, rowBytes);
    }
    std::vector<uint8_t> bgr(size_t(w) * 3 * h);
    for (UINT y = 0; y < h; ++y)
    {
        const uint8_t* row = p + fp.Offset + UINT64(y) * fp.Footprint.RowPitch;
        uint8_t* o = bgr.data() + size_t(h - 1 - y) * w * 3;
        for (UINT x = 0; x < w; ++x)
        {
            float r = 0, g = 0, bl = 0;
            switch (d.Format)
            {
            case DXGI_FORMAT_R10G10B10A2_UNORM:
            {
                uint32_t v = ((const uint32_t*) row)[x];
                r = (v & 1023) / 1023.f;
                g = ((v >> 10) & 1023) / 1023.f;
                bl = ((v >> 20) & 1023) / 1023.f;
                break;
            }
            case DXGI_FORMAT_R8G8B8A8_UNORM:
                r = row[x * 4] / 255.f, g = row[x * 4 + 1] / 255.f, bl = row[x * 4 + 2] / 255.f;
                break;
            case DXGI_FORMAT_B8G8R8A8_UNORM:
                bl = row[x * 4] / 255.f, g = row[x * 4 + 1] / 255.f, r = row[x * 4 + 2] / 255.f;
                break;
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            {
                auto* hv = (const uint16_t*) row + x * 4;
                auto enc = [](float c) { return c <= 0 ? 0.f : std::pow((std::min)(c, 1.f), 1 / 2.2f); };
                r = enc(HalfToFloat(hv[0])), g = enc(HalfToFloat(hv[1])), bl = enc(HalfToFloat(hv[2]));
                break;
            }
            default:
                break;
            }
            o[x * 3] = uint8_t(bl * 255.f + 0.5f);
            o[x * 3 + 1] = uint8_t(g * 255.f + 0.5f);
            o[x * 3 + 2] = uint8_t(r * 255.f + 0.5f);
        }
    }
    rb->Unmap(0, nullptr);
    const uint32_t stride = (w * 3 + 3) & ~3u, img = stride * h;
    std::ofstream bmp(base + L".bmp", std::ios::binary);
    uint8_t hdr[54] = { 'B', 'M' };
    *(uint32_t*) (hdr + 2) = 54 + img;
    *(uint32_t*) (hdr + 10) = 54;
    *(uint32_t*) (hdr + 14) = 40;
    *(int32_t*) (hdr + 18) = (int32_t) w;
    *(int32_t*) (hdr + 22) = (int32_t) h;
    *(uint16_t*) (hdr + 26) = 1;
    *(uint16_t*) (hdr + 28) = 24;
    *(uint32_t*) (hdr + 34) = img;
    bmp.write((const char*) hdr, 54);
    std::vector<uint8_t> pad(stride - w * 3, 0);
    for (UINT y = 0; y < h; ++y)
    {
        bmp.write((const char*) bgr.data() + size_t(y) * w * 3, w * 3);
        if (!pad.empty())
            bmp.write((const char*) pad.data(), pad.size());
    }
    Log(0, std::format("dumped {}x{} fmt {}", w, h, (int) d.Format));
    return true;
}
} // namespace Mgpu
