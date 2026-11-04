// UI composite on the display card: out = lerp(nr, bb, smoothstep(t0, t1, max3|bb - hudless|)) (the FFX / HUDfix
// idea), and a count of the pixels the mask takes from the back buffer, for the sanity gate.
#pragma once
#include <d3d12.h>
#include <wrl/client.h>

namespace Mgpu
{
class UiComposite
{
  public:
    static constexpr UINT kSlots = 3;
    static constexpr float kT0 = 2.f / 1023.f, kT1 = 16.f / 1023.f;

    bool Init(ID3D12Device* dev, DXGI_FORMAT outFormat);
    // count only (nr = nullptr) or composite into `out` (UAV state on entry and exit). Inputs in NON_PIXEL_SHADER_RESOURCE.
    void Record(ID3D12GraphicsCommandList* cl, UINT slot, ID3D12Resource* bb, ID3D12Resource* hud, ID3D12Resource* nr,
                ID3D12Resource* out, UINT w, UINT h);
    // Pixels counted by the last Record on `slot`; valid once that work completed.
    UINT Count(UINT slot);
    DXGI_FORMAT Format() const { return _fmt; }

  private:
    Microsoft::WRL::ComPtr<ID3D12Device> _dev;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> _root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _pso;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> _counter, _zero, _readback;
    DXGI_FORMAT _fmt = DXGI_FORMAT_UNKNOWN;
    UINT _inc = 0;
};

// Render card: one flag per kTile square tile, set where any texel of the HUD-less colour differs from the back
// buffer (bit compare). Only those tiles of the HUD-less colour are sent; the rest equals the back buffer.
class TileDiff
{
  public:
    static constexpr UINT kTile = 64;
    bool Init(ID3D12Device* dev, DXGI_FORMAT fmt, UINT w, UINT h, UINT buffers);
    // bb and hud in NON_PIXEL_SHADER_RESOURCE; the flags land in readback slice `i` once the list completed
    void Record(ID3D12GraphicsCommandList* cl, UINT i, ID3D12Resource* bb, ID3D12Resource* hud);
    const UINT* Map(UINT i);
    void Unmap();
    UINT TilesX() const { return _tx; }
    UINT TilesY() const { return _ty; }

  private:
    Microsoft::WRL::ComPtr<ID3D12Device> _dev;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> _root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _pso;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> _flags, _readback;
    DXGI_FORMAT _fmt = DXGI_FORMAT_UNKNOWN;
    UINT _inc = 0, _w = 0, _h = 0, _tx = 0, _ty = 0, _n = 0;
};
} // namespace Mgpu
