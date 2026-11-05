// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../../LICENSE.
// Colour-only motion estimation for the network history: hierarchical block matching on luma, then a per-pixel pick
// among the neighbouring block vectors. Vectors point current -> previous (previous = current + mv), in units of the
// output texture's pixels.
#pragma once
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

class OpticalFlow
{
  public:
    static constexpr uint32_t kLevels = 4;
    bool Init(ID3D12Device* device, ID3D12CommandQueue* queue);
    // Records on list: src (an SRV-readable colour texture in srcState) becomes this frame's pyramid, and the vectors
    // against the last frame's go to Vectors(). False on the first frame of a size (no previous frame: no vectors).
    bool Run(ID3D12GraphicsCommandList* list, ID3D12Resource* src, D3D12_RESOURCE_STATES srcState, bool zero,
             bool linear = false);
    ID3D12Resource* Vectors() const { return _mv.Get(); } // R16G16_FLOAT, NON_PIXEL_SHADER_RESOURCE after Run
    uint32_t Width() const { return _w[0]; }
    uint32_t Height() const { return _h[0]; }
    float GpuMs(); // median of recent frames
    void Invalidate() { _valid = false; }

  private:
    bool Ensure(uint32_t srcW, uint32_t srcH);
    void Dispatch(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, const uint32_t* consts,
                  ID3D12Resource* const srv[3], const DXGI_FORMAT srvFmt[3], ID3D12Resource* uav, DXGI_FORMAT uavFmt,
                  uint32_t groupsX, uint32_t groupsY);

    Microsoft::WRL::ComPtr<ID3D12Device> _dev;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> _root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _psoLuma, _psoDown, _psoSearch, _psoPixel, _psoZero;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _heap;
    uint32_t _stride = 0, _cursor = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> _luma[2][kLevels], _vec[kLevels], _mv;
    uint32_t _w[kLevels] = {}, _h[kLevels] = {}, _bw[kLevels] = {}, _bh[kLevels] = {};
    uint32_t _srcW = 0, _srcH = 0, _cur = 0;
    bool _valid = false; // the other pyramid holds the previous frame
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> _qh;
    Microsoft::WRL::ComPtr<ID3D12Resource> _qrb;
    uint64_t _qFrame = 0;
    double _freq = 0;
    float _ms[31] = {};
    uint32_t _msN = 0;
};
