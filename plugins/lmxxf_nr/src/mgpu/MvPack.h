// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../../LICENSE.
// Lossless packing of R16G16 motion vectors for the cross-adapter send. Per 8x8 tile and channel: the minimum of an
// order-preserving 16-bit key of the half-float bits, then every texel's key - minimum in the fewest bits that hold
// the tile's range. Packed layout: tiles x 2 words of header (minima, bit widths, data offset), then the data, tiles
// in the order their atomic allocation ran (one pass; the size is deterministic, the layout is not).
// Only d3d12 and d3dcompiler: the offline test (tools/mvpack_test) builds this file as is.
#pragma once
#include <d3d12.h>
#include <wrl/client.h>

namespace Mgpu
{
class MvPack
{
  public:
    static constexpr UINT kTile = 8, kSlots = 4;
    static UINT Tiles(UINT w, UINT h) { return ((w + kTile - 1) / kTile) * ((h + kTile - 1) / kTile); }
    static UINT64 HeaderBytes(UINT w, UINT h) { return UINT64(Tiles(w, h)) * 8; }
    static UINT64 MaxBytes(UINT w, UINT h) { return HeaderBytes(w, h) + UINT64(Tiles(w, h)) * 256; }
    static UINT Pitch(UINT w) { return (w * 4 + 255) & ~255u; } // a copyable footprint's row pitch

    bool Init(ID3D12Device* dev);
    // src: raw buffer of w x h texels (4 bytes) at row pitch Pitch(w), NON_PIXEL_SHADER_RESOURCE. out: raw buffer of
    // MaxBytes, UNORDERED_ACCESS on entry and exit. The packed size (bytes) lands in `total` at 4 * slot.
    void Encode(ID3D12GraphicsCommandList* cl, UINT slot, ID3D12Resource* src, UINT w, UINT h, ID3D12Resource* out,
                ID3D12Resource* total);
    // packed: NON_PIXEL_SHADER_RESOURCE; dst: raw buffer at row pitch Pitch(w), UNORDERED_ACCESS
    void Decode(ID3D12GraphicsCommandList* cl, UINT slot, ID3D12Resource* packed, UINT w, UINT h, ID3D12Resource* dst);
    const char* Error() const { return _err; }

  private:
    Microsoft::WRL::ComPtr<ID3D12Device> _dev;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> _root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _reset, _pack, _decode;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _heap;
    UINT _inc = 0;
    const char* _err = "";
    void Bind(ID3D12GraphicsCommandList* cl, UINT slot, ID3D12Resource* srv, ID3D12Resource* uav0,
              ID3D12Resource* uav1, const UINT* k);
};
} // namespace Mgpu
