// (b) Cross-adapter frame package copies through a SHARED_CROSS_ADAPTER heap, both directions.
#include "diag.h"

#include <cstring>
#include <atomic>
#include <random>
#include <thread>

namespace {

constexpr int kSlots = 3;

struct Dev {
  AdapterInfo* info = nullptr;
  ComPtr<ID3D12Device> dev;
  ComPtr<ID3D12CommandQueue> copyq, directq;
  ComPtr<ID3D12CommandAllocator> dalloc;
  ComPtr<ID3D12GraphicsCommandList> dlist;
  ComPtr<ID3D12Fence> local;
  HANDLE ev = nullptr;
  uint64_t lv = 0;
  bool copy_ts = false, rowmajor = false;
  uint64_t ts_freq = 0;
  std::string label;

  bool wait(ID3D12Fence* f, uint64_t v, DWORD ms = 10000) {
    if (f->GetCompletedValue() >= v) return true;
    f->SetEventOnCompletion(v, ev);
    return WaitForSingleObject(ev, ms) == WAIT_OBJECT_0;
  }
  bool flush_direct() {
    directq->Signal(local.Get(), ++lv);
    return wait(local.Get(), lv);
  }
  void dbegin() { dalloc->Reset(); dlist->Reset(dalloc.Get(), nullptr); }
  void dsubmit() {
    dlist->Close();
    ID3D12CommandList* l[] = {dlist.Get()};
    directq->ExecuteCommandLists(1, l);
  }
};

struct Surface { const char* name; DXGI_FORMAT fmt; UINT w, h, bpp; bool depth; };
struct Variant { const char* name; std::vector<Surface> s; };

D3D12_RESOURCE_DESC tex_desc(const Surface& s, D3D12_TEXTURE_LAYOUT layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
                             D3D12_RESOURCE_FLAGS extra = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = s.w;
  d.Height = s.h;
  d.DepthOrArraySize = d.MipLevels = 1;
  d.Format = s.fmt;
  d.SampleDesc.Count = 1;
  d.Layout = layout;
  d.Flags = extra;
  if (layout == D3D12_TEXTURE_LAYOUT_UNKNOWN)
    d.Flags |= s.depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  return d;
}

D3D12_RESOURCE_DESC buf_desc(uint64_t size, D3D12_RESOURCE_FLAGS f = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size;
  d.Height = d.DepthOrArraySize = d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  d.Flags = f;
  return d;
}

ComPtr<ID3D12Resource> committed(ID3D12Device* dev, D3D12_HEAP_TYPE t, const D3D12_RESOURCE_DESC& d, D3D12_RESOURCE_STATES st) {
  D3D12_HEAP_PROPERTIES hp{t};
  ComPtr<ID3D12Resource> r;
  dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r));
  return r;
}

uint16_t rand_half(std::mt19937& g) { return uint16_t(((g() % 15) << 10) | (g() & 0x3FF)); }

// Finite, non-NaN noise for each format so copies can be compared bitwise.
std::vector<uint8_t> noise(const Surface& s, uint32_t seed) {
  std::mt19937 g(seed);
  std::vector<uint8_t> v(size_t(s.w) * s.h * s.bpp);
  std::uniform_real_distribution<float> u(0.f, 1.f);
  for (size_t px = 0; px < size_t(s.w) * s.h; ++px) {
    uint8_t* p = v.data() + px * s.bpp;
    if (s.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT || s.fmt == DXGI_FORMAT_R16G16_FLOAT) {
      for (UINT c = 0; c < s.bpp / 2; ++c) { uint16_t h = rand_half(g); memcpy(p + 2 * c, &h, 2); }
    } else if (s.fmt == DXGI_FORMAT_R11G11B10_FLOAT) {
      uint32_t r = ((g() % 31) << 6) | (g() & 63), gg = ((g() % 31) << 6) | (g() & 63), b = ((g() % 31) << 5) | (g() & 31);
      uint32_t x = r | (gg << 11) | (b << 22);
      memcpy(p, &x, 4);
    } else {
      float f = u(g);
      memcpy(p, &f, 4);
    }
  }
  return v;
}

struct Footprints {
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp;
  std::vector<UINT> rows;
  std::vector<UINT64> row_bytes;
  uint64_t total = 0;
};

Footprints layout(ID3D12Device* dev, const Variant& v, uint64_t base = 0) {
  Footprints f;
  uint64_t off = base;
  for (auto& s : v.s) {
    D3D12_RESOURCE_DESC d = tex_desc(s);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 rb, total;
    dev->GetCopyableFootprints(&d, 0, 1, off, &fp, &rows, &rb, &total);
    f.fp.push_back(fp);
    f.rows.push_back(rows);
    f.row_bytes.push_back(rb);
    off = (off + total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
  }
  f.total = off - base;
  return f;
}

uint64_t package_bytes(const Variant& v) {
  uint64_t b = 0;
  for (auto& s : v.s) b += uint64_t(s.w) * s.h * s.bpp;
  return b;
}

bool init_dev(Dev& d, AdapterInfo* a, Json& j) {
  d.info = a;
  d.label = "PCI bus " + std::to_string(a->pci.bus);
  HRESULT hr = D3D12CreateDevice(a->adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d.dev));
  if (FAILED(hr)) { j["error"] = "D3D12CreateDevice " + hr_str(hr); return false; }
  D3D12_COMMAND_QUEUE_DESC cq{D3D12_COMMAND_LIST_TYPE_COPY};
  d.dev->CreateCommandQueue(&cq, IID_PPV_ARGS(&d.copyq));
  D3D12_COMMAND_QUEUE_DESC dq{D3D12_COMMAND_LIST_TYPE_DIRECT};
  d.dev->CreateCommandQueue(&dq, IID_PPV_ARGS(&d.directq));
  d.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d.dalloc));
  d.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.dalloc.Get(), nullptr, IID_PPV_ARGS(&d.dlist));
  d.dlist->Close();
  d.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.local));
  d.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
  d.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o);
  D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};
  d.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof o3);
  d.rowmajor = o.CrossAdapterRowMajorTextureSupported;
  d.copy_ts = o3.CopyQueueTimestampQueriesSupported;
  d.copyq->GetTimestampFrequency(&d.ts_freq);
  j["luid"] = luid_str(a->desc.AdapterLuid);
  j["pci_bus"] = a->pci.bus;
  j["cross_adapter_row_major_texture_supported"] = bool(o.CrossAdapterRowMajorTextureSupported);
  j["cross_node_sharing_tier"] = int(o.CrossNodeSharingTier);
  j["resource_heap_tier"] = int(o.ResourceHeapTier);
  j["copy_queue_timestamps"] = bool(o3.CopyQueueTimestampQueriesSupported);
  j["copy_queue_timestamp_hz"] = (unsigned long long)d.ts_freq;
  printf("  %s LUID %s: CrossAdapterRowMajorTextureSupported=%d CrossNodeSharingTier=%d copy-queue timestamps=%d (%llu Hz)\n",
         d.label.c_str(), luid_str(a->desc.AdapterLuid).c_str(), o.CrossAdapterRowMajorTextureSupported,
         o.CrossNodeSharingTier, o3.CopyQueueTimestampQueriesSupported, (unsigned long long)d.ts_freq);
  return true;
}

struct Lists {
  ComPtr<ID3D12CommandAllocator> alloc[kSlots];
  ComPtr<ID3D12GraphicsCommandList> list[kSlots];
  ComPtr<ID3D12QueryHeap> qh;
  ComPtr<ID3D12Resource> ts_rb;
};

bool make_lists(Dev& d, Lists& l) {
  for (int i = 0; i < kSlots; ++i) {
    if (FAILED(d.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&l.alloc[i])))) return false;
    if (FAILED(d.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, l.alloc[i].Get(), nullptr, IID_PPV_ARGS(&l.list[i]))))
      return false;
  }
  if (d.copy_ts) {
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP, 2 * kSlots};
    d.dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&l.qh));
    l.ts_rb = committed(d.dev.Get(), D3D12_HEAP_TYPE_READBACK, buf_desc(16 * kSlots), D3D12_RESOURCE_STATE_COPY_DEST);
  }
  return true;
}

double ts_ms(Dev& d, Lists& l, int slot) {
  if (!l.qh) return -1;
  uint64_t* t = nullptr;
  D3D12_RANGE r{size_t(16 * slot), size_t(16 * slot + 16)};
  l.ts_rb->Map(0, &r, (void**)&t);
  double ms = double(t[2 * slot + 1] - t[2 * slot]) * 1000.0 / double(d.ts_freq);
  D3D12_RANGE w{0, 0};
  l.ts_rb->Unmap(0, &w);
  return ms;
}

struct Config { const Variant* v; Dev* P; Dev* C; bool owner_is_producer; bool texture_mode; };

Json run_config(const Config& cfg, int iters) {
  Dev& P = *cfg.P;
  Dev& C = *cfg.C;
  Dev& O = cfg.owner_is_producer ? P : C;
  Dev& X = cfg.owner_is_producer ? C : P;
  const Variant& v = *cfg.v;
  Json j = Json::obj();
  j["variant"] = v.name;
  j["direction"] = P.label + " -> " + C.label;
  j["heap_owner"] = cfg.owner_is_producer ? "producer" : "consumer";
  j["layout"] = cfg.texture_mode ? "row_major_texture" : "buffer";
  const uint64_t pkg = package_bytes(v);
  j["package_bytes"] = (unsigned long long)pkg;
  auto fail = [&](const std::string& why) { j["error"] = why; printf("    %-16s %s: %s\n", v.name, std::get<std::string>(j["direction"].v).c_str(), why.c_str()); return j; };

  // Local surfaces: producer filled with noise, consumer as copy destination.
  std::vector<ComPtr<ID3D12Resource>> srcs, dsts;
  std::vector<std::vector<uint8_t>> ref;
  Footprints tight = layout(P.dev.Get(), v);
  auto upload = committed(P.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, buf_desc(tight.total), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!upload) return fail("upload buffer");
  uint8_t* up = nullptr;
  upload->Map(0, nullptr, (void**)&up);
  P.dbegin();
  for (size_t i = 0; i < v.s.size(); ++i) {
    auto& s = v.s[i];
    ref.push_back(noise(s, uint32_t(1234 + i)));
    for (UINT y = 0; y < s.h; ++y)
      memcpy(up + tight.fp[i].Offset + uint64_t(y) * tight.fp[i].Footprint.RowPitch, ref[i].data() + size_t(y) * s.w * s.bpp, size_t(s.w) * s.bpp);
    auto src = committed(P.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, tex_desc(s), D3D12_RESOURCE_STATE_COPY_DEST);
    auto dst = committed(C.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, tex_desc(s), D3D12_RESOURCE_STATE_COPY_DEST);
    if (!src || !dst) return fail(std::string("texture ") + s.name);
    D3D12_TEXTURE_COPY_LOCATION dl{src.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dl.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION sl{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint = tight.fp[i];
    P.dlist->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    b.Transition = {src.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE};
    P.dlist->ResourceBarrier(1, &b);
    srcs.push_back(src);
    dsts.push_back(dst);
  }
  upload->Unmap(0, nullptr);
  P.dsubmit();
  if (!P.flush_direct()) return fail("producer upload timed out");

  // Shared heap: kSlots package slots, created on the owner and opened on the other device.
  const uint64_t align64k = 65536;
  std::vector<uint64_t> tex_off;  // texture mode: per slot/surface placement
  uint64_t slot_bytes = 0;
  if (cfg.texture_mode) {
    for (auto& s : v.s) {
      D3D12_RESOURCE_DESC d = tex_desc(s, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
      if (s.depth) d.Format = DXGI_FORMAT_R32_FLOAT;
      auto ai = O.dev->GetResourceAllocationInfo(0, 1, &d);
      if (ai.SizeInBytes == UINT64_MAX) return fail(std::string("row-major texture not allocatable: ") + s.name);
      tex_off.push_back(slot_bytes);
      slot_bytes += (ai.SizeInBytes + align64k - 1) & ~(align64k - 1);
    }
  } else {
    slot_bytes = (tight.total + align64k - 1) & ~(align64k - 1);
  }
  D3D12_HEAP_DESC hd{};
  hd.SizeInBytes = slot_bytes * kSlots;
  hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
  ComPtr<ID3D12Heap> heapO, heapX;
  HRESULT hr = O.dev->CreateHeap(&hd, IID_PPV_ARGS(&heapO));
  if (FAILED(hr)) return fail("CreateHeap(SHARED|SHARED_CROSS_ADAPTER) " + hr_str(hr));
  HANDLE hh = nullptr;
  hr = O.dev->CreateSharedHandle(heapO.Get(), nullptr, GENERIC_ALL, nullptr, &hh);
  if (FAILED(hr)) return fail("CreateSharedHandle(heap) " + hr_str(hr));
  hr = X.dev->OpenSharedHandle(hh, IID_PPV_ARGS(&heapX));
  CloseHandle(hh);
  if (FAILED(hr)) return fail("OpenSharedHandle(heap) " + hr_str(hr));
  ID3D12Heap* heapP = cfg.owner_is_producer ? heapO.Get() : heapX.Get();
  ID3D12Heap* heapC = cfg.owner_is_producer ? heapX.Get() : heapO.Get();

  // Placed views of the shared memory on both devices.
  std::vector<ComPtr<ID3D12Resource>> shP, shC;  // buffer: one per slot; texture: slot*surfaces
  auto place = [&](Dev& d, ID3D12Heap* h, std::vector<ComPtr<ID3D12Resource>>& out) -> HRESULT {
    for (int sl = 0; sl < kSlots; ++sl) {
      if (!cfg.texture_mode) {
        ComPtr<ID3D12Resource> r;
        auto bd = buf_desc(slot_bytes, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
        HRESULT e = d.dev->CreatePlacedResource(h, sl * slot_bytes, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
        if (FAILED(e)) return e;
        out.push_back(r);
      } else {
        for (size_t i = 0; i < v.s.size(); ++i) {
          D3D12_RESOURCE_DESC td = tex_desc(v.s[i], D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
          if (v.s[i].depth) td.Format = DXGI_FORMAT_R32_FLOAT;
          ComPtr<ID3D12Resource> r;
          HRESULT e = d.dev->CreatePlacedResource(h, sl * slot_bytes + tex_off[i], &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                  IID_PPV_ARGS(&r));
          if (FAILED(e)) return e;
          out.push_back(r);
        }
      }
    }
    return S_OK;
  };
  if (FAILED(hr = place(P, heapP, shP))) return fail("CreatePlacedResource(producer) " + hr_str(hr));
  if (FAILED(hr = place(C, heapC, shC))) return fail("CreatePlacedResource(consumer) " + hr_str(hr));

  // Cross-adapter fences: producer-done owned by P, consumer-done owned by C.
  auto share_fence = [&](Dev& owner, Dev& other, ComPtr<ID3D12Fence>& fo, ComPtr<ID3D12Fence>& fx) -> HRESULT {
    HRESULT e = owner.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&fo));
    if (FAILED(e)) return e;
    HANDLE fh = nullptr;
    if (FAILED(e = owner.dev->CreateSharedHandle(fo.Get(), nullptr, GENERIC_ALL, nullptr, &fh))) return e;
    e = other.dev->OpenSharedHandle(fh, IID_PPV_ARGS(&fx));
    CloseHandle(fh);
    return e;
  };
  ComPtr<ID3D12Fence> pDoneP, pDoneC, cDoneC, cDoneP;
  if (FAILED(hr = share_fence(P, C, pDoneP, pDoneC))) return fail("shared fence (producer) " + hr_str(hr));
  if (FAILED(hr = share_fence(C, P, cDoneC, cDoneP))) return fail("shared fence (consumer) " + hr_str(hr));

  // Prerecord one list per slot on each side.
  Lists lp, lc;
  if (!make_lists(P, lp) || !make_lists(C, lc)) return fail("copy command lists");
  for (int sl = 0; sl < kSlots; ++sl) {
    Footprints f = layout(P.dev.Get(), v, 0);
    for (int side = 0; side < 2; ++side) {
      Lists& L = side == 0 ? lp : lc;
      auto* cl = L.list[sl].Get();
      if (L.qh) cl->EndQuery(L.qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * sl);
      for (size_t i = 0; i < v.s.size(); ++i) {
        D3D12_TEXTURE_COPY_LOCATION local{(side == 0 ? srcs[i] : dsts[i]).Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        local.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION shared{};
        if (cfg.texture_mode) {
          shared.pResource = (side == 0 ? shP : shC)[sl * v.s.size() + i].Get();
          shared.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          shared.SubresourceIndex = 0;
        } else {
          shared.pResource = (side == 0 ? shP : shC)[sl].Get();
          shared.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          shared.PlacedFootprint = f.fp[i];
        }
        if (side == 0) cl->CopyTextureRegion(&shared, 0, 0, 0, &local, nullptr);
        else cl->CopyTextureRegion(&local, 0, 0, 0, &shared, nullptr);
      }
      if (L.qh) {
        cl->EndQuery(L.qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * sl + 1);
        cl->ResolveQueryData(L.qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * sl, 2, L.ts_rb.Get(), 16 * sl);
      }
      if (FAILED(hr = cl->Close())) return fail("Close copy list " + hr_str(hr));
    }
  }

  uint64_t pv = 0, cv = 0;
  auto exec = [](Dev& d, Lists& l, int sl) {
    ID3D12CommandList* x[] = {l.list[sl].Get()};
    d.copyq->ExecuteCommandLists(1, x);
  };

  // Serial: one package at a time, full latency.
  std::vector<double> wall, wallP, gpuP, gpuC;
  for (int i = 0; i <= iters; ++i) {
    double t0 = now_ms();
    exec(P, lp, 0);
    P.copyq->Signal(pDoneP.Get(), ++pv);
    C.copyq->Wait(pDoneC.Get(), pv);
    exec(C, lc, 0);
    C.copyq->Signal(cDoneC.Get(), ++cv);
    if (!P.wait(pDoneP.Get(), pv)) return fail("producer copy timed out");
    double t1 = now_ms();
    if (!C.wait(cDoneC.Get(), cv)) return fail("consumer copy timed out");
    double t2 = now_ms();
    if (i == 0) continue;
    wall.push_back(t2 - t0);
    wallP.push_back(t1 - t0);
    if (lp.qh) gpuP.push_back(ts_ms(P, lp, 0));
    if (lc.qh) gpuC.push_back(ts_ms(C, lc, 0));
  }

  // Validate consumer contents against the producer's noise.
  bool valid = true;
  {
    Footprints rf = layout(C.dev.Get(), v);
    auto rb = committed(C.dev.Get(), D3D12_HEAP_TYPE_READBACK, buf_desc(rf.total), D3D12_RESOURCE_STATE_COPY_DEST);
    C.dbegin();
    for (size_t i = 0; i < v.s.size(); ++i) {
      D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
      b.Transition = {dsts[i].Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE};
      C.dlist->ResourceBarrier(1, &b);
      D3D12_TEXTURE_COPY_LOCATION dl{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dl.PlacedFootprint = rf.fp[i];
      D3D12_TEXTURE_COPY_LOCATION sl{dsts[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      sl.SubresourceIndex = 0;
      C.dlist->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
      std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
      C.dlist->ResourceBarrier(1, &b);
    }
    C.dsubmit();
    if (!C.flush_direct()) return fail("validation readback timed out");
    uint8_t* r = nullptr;
    rb->Map(0, nullptr, (void**)&r);
    Json per = Json::obj();
    for (size_t i = 0; i < v.s.size(); ++i) {
      auto& s = v.s[i];
      size_t bad_rows = 0;
      for (UINT y = 0; y < s.h; ++y)
        bad_rows += memcmp(r + rf.fp[i].Offset + uint64_t(y) * rf.fp[i].Footprint.RowPitch, ref[i].data() + size_t(y) * s.w * s.bpp,
                           size_t(s.w) * s.bpp) != 0;
      per[s.name] = bad_rows == 0;
      valid = valid && bad_rows == 0;
    }
    rb->Unmap(0, nullptr);
    j["validated"] = per;
  }

  // Pipelined: kSlots-deep ring, producer waits only for slot reuse.
  const int frames = iters * 2;
  double tp0 = now_ms();
  for (int i = 0; i < frames; ++i) {
    int sl = i % kSlots;
    if (i >= kSlots) P.copyq->Wait(cDoneP.Get(), cv - kSlots + 1);
    exec(P, lp, sl);
    P.copyq->Signal(pDoneP.Get(), ++pv);
    C.copyq->Wait(pDoneC.Get(), pv);
    exec(C, lc, sl);
    C.copyq->Signal(cDoneC.Get(), ++cv);
    if (i >= 2 * kSlots && !C.wait(cDoneC.Get(), cv - 2 * kSlots)) return fail("pipelined timed out");
  }
  if (!C.wait(cDoneC.Get(), cv, 30000)) return fail("pipelined drain timed out");
  double pipe_ms = (now_ms() - tp0) / frames;

  Stats sw = Stats::of(wall), swp = Stats::of(wallP), sgp = Stats::of(gpuP), sgc = Stats::of(gpuC);
  auto gbps = [&](double ms) { return ms > 0 ? pkg / (ms * 1e6) : 0.0; };
  j["serial_wall_ms"] = sw.json();
  j["producer_wall_ms"] = swp.json();
  if (!gpuP.empty()) j["producer_gpu_ms"] = sgp.json();
  if (!gpuC.empty()) j["consumer_gpu_ms"] = sgc.json();
  j["producer_gpu_gbps"] = gbps(sgp.p50);
  j["consumer_gpu_gbps"] = gbps(sgc.p50);
  j["serial_wall_gbps"] = gbps(sw.p50);
  j["pipelined_ms_per_package"] = pipe_ms;
  j["pipelined_gbps"] = gbps(pipe_ms);
  j["valid"] = valid;
  printf("    %-17s %-22s owner=%-8s %-6s %6.1f MB | wall p50 %6.2f p95 %6.2f ms (%5.1f GB/s) | GPU P %5.2f ms (%5.1f) C %5.2f ms (%5.1f) | pipelined %5.2f ms/pkg (%5.1f GB/s) | %s\n",
         v.name, std::get<std::string>(j["direction"].v).c_str(), cfg.owner_is_producer ? "producer" : "consumer",
         cfg.texture_mode ? "tex" : "buf", pkg / 1e6, sw.p50, sw.p95, gbps(sw.p50), sgp.p50, gbps(sgp.p50), sgc.p50, gbps(sgc.p50),
         pipe_ms, gbps(pipe_ms), valid ? "valid" : "MISMATCH");
  return j;
}

// Host link bandwidth: copy-queue upload->VRAM and VRAM->readback, GPU-timed.
Json host_link_bw(Dev& d, int iters) {
  Json j = Json::obj();
  const uint64_t sz = 128ull << 20;
  auto up = committed(d.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, buf_desc(sz), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto vram = committed(d.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, buf_desc(sz), D3D12_RESOURCE_STATE_COMMON);
  auto rb = committed(d.dev.Get(), D3D12_HEAP_TYPE_READBACK, buf_desc(sz), D3D12_RESOURCE_STATE_COPY_DEST);
  Lists l;
  if (!up || !vram || !rb || !make_lists(d, l) || !l.qh) { j["error"] = "setup failed"; return j; }
  for (int k = 0; k < 2; ++k) {
    auto* cl = l.list[k].Get();
    cl->EndQuery(l.qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * k);
    if (k == 0) cl->CopyBufferRegion(vram.Get(), 0, up.Get(), 0, sz);
    else cl->CopyBufferRegion(rb.Get(), 0, vram.Get(), 0, sz);
    cl->EndQuery(l.qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * k + 1);
    cl->ResolveQueryData(l.qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * k, 2, l.ts_rb.Get(), 16 * k);
    cl->Close();
  }
  ComPtr<ID3D12Fence> f;
  d.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f));
  uint64_t v = 0;
  for (int k = 0; k < 2; ++k) {
    std::vector<double> t;
    for (int i = 0; i <= iters; ++i) {
      ID3D12CommandList* x[] = {l.list[k].Get()};
      d.copyq->ExecuteCommandLists(1, x);
      d.copyq->Signal(f.Get(), ++v);
      d.wait(f.Get(), v);
      if (i) t.push_back(ts_ms(d, l, k));
    }
    Stats st = Stats::of(t);
    j[k == 0 ? "host_to_vram_ms" : "vram_to_host_ms"] = st.json();
    j[k == 0 ? "host_to_vram_gbps" : "vram_to_host_gbps"] = sz / (st.p50 * 1e6);
  }
  printf("  %s host link (copy queue, 128 MB): host->VRAM %.1f GB/s, VRAM->host %.1f GB/s\n", d.label.c_str(),
         std::get<double>(j["host_to_vram_gbps"].v), std::get<double>(j["vram_to_host_gbps"].v));
  return j;
}

// Keeps a device's direct queue busy with VRAM-to-VRAM copies, as a stand-in for render/NR load.
struct Busy {
  std::atomic<bool> stop{false};
  std::thread th;
  void start(Dev& d) {
    th = std::thread([this, &d] {
      ComPtr<ID3D12CommandQueue> q;
      D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
      d.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
      const uint64_t sz = 256ull << 20;
      auto a = committed(d.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, buf_desc(sz), D3D12_RESOURCE_STATE_COMMON);
      auto b = committed(d.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, buf_desc(sz), D3D12_RESOURCE_STATE_COMMON);
      ComPtr<ID3D12CommandAllocator> al;
      ComPtr<ID3D12GraphicsCommandList> l;
      ComPtr<ID3D12Fence> f;
      d.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al));
      d.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&l));
      for (int i = 0; i < 4; ++i) { l->CopyBufferRegion(b.Get(), 0, a.Get(), 0, sz); l->CopyBufferRegion(a.Get(), 0, b.Get(), 0, sz); }
      l->Close();
      d.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f));
      HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      uint64_t v = 0;
      while (!stop) {
        ID3D12CommandList* x[] = {l.Get()};
        q->ExecuteCommandLists(1, x);
        q->Signal(f.Get(), ++v);
        f->SetEventOnCompletion(v, ev);
        WaitForSingleObject(ev, 5000);
      }
      CloseHandle(ev);
    });
  }
  void end() { stop = true; if (th.joinable()) th.join(); }
};

}  // namespace

Json bench_cross_adapter(AdapterInfo& a, AdapterInfo& b, int iters) {
  Json out = Json::obj();
  Dev A, B;
  Json ja = Json::obj(), jb = Json::obj();
  bool ok = init_dev(A, &a, ja) && init_dev(B, &b, jb);
  out["device_a"] = ja;
  out["device_b"] = jb;
  if (!ok) return out;

  ja["host_link"] = host_link_bw(A, iters);
  jb["host_link"] = host_link_bw(B, iters);
  out["device_a"] = ja;
  out["device_b"] = jb;

  const UINT W = 3840, H = 2160, w = 2880, h = 1620;
  std::vector<Variant> variants = {
      {"4k_rgba16f", {{"color", DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, 8, false}, {"depth", DXGI_FORMAT_D32_FLOAT, W, H, 4, true},
                      {"mv", DXGI_FORMAT_R16G16_FLOAT, W, H, 4, false}}},
      {"4k_r11g11b10f", {{"color", DXGI_FORMAT_R11G11B10_FLOAT, W, H, 4, false}, {"depth", DXGI_FORMAT_D32_FLOAT, W, H, 4, true},
                         {"mv", DXGI_FORMAT_R16G16_FLOAT, W, H, 4, false}}},
      {"nrscale0.75_rgba16f", {{"color", DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, 8, false}, {"depth", DXGI_FORMAT_D32_FLOAT, w, h, 4, true},
                               {"mv", DXGI_FORMAT_R16G16_FLOAT, w, h, 4, false}}},
      {"4k_output_rgba16f", {{"color", DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, 8, false}}},
  };
  out["notes"] = "nrscale0.75 = inputs at 2880x1620 (0.75 of 4K per axis); 4k_output = NR result returned to the display GPU";
  Json runs = Json::arr();
  for (auto& v : variants)
    for (int dir = 0; dir < 2; ++dir)
      for (int owner = 1; owner >= 0; --owner) {
        Config c{&v, dir == 0 ? &A : &B, dir == 0 ? &B : &A, owner == 1, false};
        runs.push(run_config(c, iters));
        for (Dev* d : {&A, &B})
          if (HRESULT r = d->dev->GetDeviceRemovedReason(); FAILED(r)) {
            out["runs"] = runs;
            out["error"] = "device removed " + hr_str(r);
            return out;
          }
      }
  // Same package with both GPUs kept busy, to expose idle link/clock power states.
  {
    Busy ba, bb;
    ba.start(A);
    bb.start(B);
    Sleep(300);
    for (int dir = 0; dir < 2; ++dir) {
      Config c{&variants[0], dir == 0 ? &A : &B, dir == 0 ? &B : &A, true, false};
      Json r = run_config(c, iters);
      r["background_load"] = "both GPUs: direct-queue 256 MB VRAM copies";
      runs.push(r);
    }
    ba.end();
    bb.end();
  }
  // Row-major textures in the shared heap, only when both drivers allow it.
  if (A.rowmajor && B.rowmajor) {
    for (int dir = 0; dir < 2; ++dir) {
      Config c{&variants[0], dir == 0 ? &A : &B, dir == 0 ? &B : &A, true, true};
      runs.push(run_config(c, iters));
    }
  } else {
    out["row_major_texture_runs"] = "skipped: CrossAdapterRowMajorTextureSupported is false, buffer + placed footprints only";
  }
  out["runs"] = runs;
  CloseHandle(A.ev);
  CloseHandle(B.ev);
  return out;
}
