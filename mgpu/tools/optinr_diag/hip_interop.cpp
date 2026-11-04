// HIP loading, device enumeration/matching, and (a) D3D12 -> HIP external memory + semaphore round trip.
#include "diag.h"

#include <cstring>

bool load_hip(HipApi& api, const wchar_t* dll, char* why, size_t why_len) {
  HMODULE m = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!m) { snprintf(why, why_len, "LoadLibrary failed (%lu)", GetLastError()); return false; }
  api.module = m;
#define OPTINR_LOAD(name, ret, args)                                              \
  api.name = reinterpret_cast<ret(__cdecl*) args>(GetProcAddress(m, #name));      \
  if (!api.name) { snprintf(why, why_len, "missing export %s", #name); return false; }
  OPTINR_HIP_FUNCS(OPTINR_LOAD)
#undef OPTINR_LOAD
  hipError_t e = api.hipInit(0);
  if (e != hipSuccess) { snprintf(why, why_len, "hipInit: %d %s", e, api.err(e)); return false; }
  return true;
}

static bool parse_bus_id(const char* s, PciAddr& a) {
  unsigned dom, bus, dev, fn;
  if (sscanf(s, "%x:%x:%x.%x", &dom, &bus, &dev, &fn) != 4) return false;
  a = {int(bus), int(dev), int(fn)};
  return true;
}

Json hip_enumerate(HipApi& hip, std::vector<AdapterInfo>& adapters) {
  Json j = Json::obj();
  j["available"] = true;
  int drv = 0, rt = 0, n = 0;
  hip.hipDriverGetVersion(&drv);
  hip.hipRuntimeGetVersion(&rt);
  hip.hipGetDeviceCount(&n);
  j["driver_version"] = drv;
  j["runtime_version"] = rt;
  j["device_count"] = n;
  printf("HIP driver %d runtime %d, %d device(s)\n", drv, rt, n);
  Json devs = Json::arr();
  for (int i = 0; i < n; ++i) {
    // Oversized buffer guards against a newer runtime writing a larger struct.
    auto buf = std::make_unique<unsigned char[]>(16384);
    memset(buf.get(), 0, 16384);
    auto* p = reinterpret_cast<HipDevicePropR0600*>(buf.get());
    hipError_t e = hip.hipGetDevicePropertiesR0600(p, i);
    Json d = Json::obj();
    d["index"] = i;
    if (e != hipSuccess) { d["error"] = hip.err(e); devs.push(d); continue; }
    p->name[255] = 0;
    p->gcnArchName[255] = 0;
    // Cross-check the struct layout by locating the arch string in the raw bytes.
    size_t found = 0;
    for (size_t off = 256; off + 4 < 16384; ++off)
      if (!memcmp(buf.get() + off, "gfx", 3)) { found = off; break; }
    bool layout_ok = found == offsetof(HipDevicePropR0600, gcnArchName);
    char bus[64] = {};
    hip.hipDeviceGetPCIBusId(bus, sizeof bus, i);
    PciAddr pa;
    parse_bus_id(bus, pa);
    LUID luid{};
    memcpy(&luid.LowPart, p->luid, 4);
    memcpy(&luid.HighPart, p->luid + 4, 4);
    std::string arch = layout_ok ? std::string(p->gcnArchName) : std::string((const char*)buf.get() + found);
    int match = -1;
    const char* how = "none";
    for (size_t k = 0; k < adapters.size(); ++k)
      if (luid_u64(adapters[k].desc.AdapterLuid) == luid_u64(luid) && luid_u64(luid) != 0) { match = int(k); how = "luid"; break; }
    if (match < 0)
      for (size_t k = 0; k < adapters.size(); ++k)
        if (adapters[k].pci.bus == pa.bus && adapters[k].pci.dev == pa.dev && adapters[k].pci.fn == pa.fn) { match = int(k); how = "pci"; break; }
    if (match >= 0) { adapters[match].hip_index = i; adapters[match].gcn_arch = arch; }
    printf("HIP[%d] %s  arch=%s  LUID=%s  PCI %s  CUs=%d  VRAM=%.1f GiB  largeBar=%d  -> DXGI[%d] by %s%s\n", i, p->name,
           arch.c_str(), luid_str(luid).c_str(), bus, p->multiProcessorCount, p->totalGlobalMem / 1073741824.0,
           layout_ok ? p->isLargeBar : -1, match, how, layout_ok ? "" : "  (struct layout mismatch, arch from scan)");
    d["name"] = p->name;
    d["gcn_arch_name"] = arch;
    d["layout_verified"] = layout_ok;
    d["luid"] = luid_str(luid);
    d["pci_bus_id"] = bus;
    d["multiprocessor_count"] = p->multiProcessorCount;
    d["total_global_mem_bytes"] = (unsigned long long)p->totalGlobalMem;
    d["compute_capability"] = std::to_string(p->major) + "." + std::to_string(p->minor);
    if (layout_ok) {
      d["is_large_bar"] = p->isLargeBar;
      d["pci_bus"] = p->pciBusID;
      d["async_engine_count"] = p->asyncEngineCount;
    }
    d["dxgi_match_index"] = match;
    d["dxgi_match_by"] = how;
    devs.push(d);
  }
  j["devices"] = devs;
  return j;
}

namespace {

struct Q {
  ComPtr<ID3D12Device> dev;
  ComPtr<ID3D12CommandQueue> q;
  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  uint64_t v = 0;
  ~Q() { CloseHandle(ev); }
  void wait_cpu(uint64_t val) {
    if (fence->GetCompletedValue() < val) { fence->SetEventOnCompletion(val, ev); WaitForSingleObject(ev, 10000); }
  }
  void submit() {
    list->Close();
    ID3D12CommandList* l[] = {list.Get()};
    q->ExecuteCommandLists(1, l);
  }
  void reset() { alloc->Reset(); list->Reset(alloc.Get(), nullptr); }
};

D3D12_RESOURCE_DESC buffer_desc(uint64_t size) {
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = size;
  rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return rd;
}

ComPtr<ID3D12Resource> make_buffer(ID3D12Device* d, D3D12_HEAP_TYPE t, uint64_t size, D3D12_HEAP_FLAGS hf = D3D12_HEAP_FLAG_NONE,
                                   D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COMMON) {
  D3D12_HEAP_PROPERTIES hp{t};
  D3D12_RESOURCE_DESC rd = buffer_desc(size);
  if (t == D3D12_HEAP_TYPE_UPLOAD) st = D3D12_RESOURCE_STATE_GENERIC_READ;
  if (t == D3D12_HEAP_TYPE_READBACK) st = D3D12_RESOURCE_STATE_COPY_DEST;
  ComPtr<ID3D12Resource> r;
  d->CreateCommittedResource(&hp, hf, &rd, st, nullptr, IID_PPV_ARGS(&r));
  return r;
}

uint32_t pattern(size_t i) { return uint32_t(i * 2654435761u) ^ 0x5EEDF00Du; }

}  // namespace

Json bench_hip_interop(HipApi& hip, AdapterInfo& a, int iters) {
  Json out = Json::obj();
  out["adapter_luid"] = luid_str(a.desc.AdapterLuid);
  out["hip_device"] = a.hip_index;
  out["kernels"] = "not compiled: no hipcc/hiprtc on this machine; validation uses hipMemsetD32Async and hipMemcpy DtoD";
  Q d;
  HRESULT hr = D3D12CreateDevice(a.adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d.dev));
  if (FAILED(hr)) { out["error"] = "D3D12CreateDevice " + hr_str(hr); return out; }
  D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
  d.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&d.q));
  d.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d.alloc));
  d.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.alloc.Get(), nullptr, IID_PPV_ARGS(&d.list));
  d.list->Close();
  d.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d.fence));

  hip.hipSetDevice(a.hip_index);
  hipStream_t s = nullptr;
  hip.hipStreamCreate(&s);
  hipEvent_t e0, e1;
  hip.hipEventCreate(&e0);
  hip.hipEventCreate(&e1);

  // Fence as external semaphore.
  hipExternalSemaphore_t sem = nullptr;
  HANDLE fh = nullptr;
  hr = d.dev->CreateSharedHandle(d.fence.Get(), nullptr, GENERIC_ALL, nullptr, &fh);
  if (SUCCEEDED(hr)) {
    hipExternalSemaphoreHandleDesc sd{};
    sd.type = hipExternalSemaphoreHandleTypeD3D12Fence;
    sd.handle.win32.handle = fh;
    hipError_t e = hip.hipImportExternalSemaphore(&sem, &sd);
    out["semaphore_import"] = e == hipSuccess ? std::string("ok") : std::string("failed: ") + hip.err(e);
    if (e != hipSuccess) sem = nullptr;
  } else {
    out["semaphore_import"] = "CreateSharedHandle(fence) " + hr_str(hr);
  }
  printf("  D3D12 fence -> hipExternalSemaphore: %s\n", std::get<std::string>(out["semaphore_import"].v).c_str());

  auto hip_wait = [&](uint64_t v) {
    if (sem) {
      hipExternalSemaphoreWaitParams w{};
      w.params.fence.value = v;
      hip.hipWaitExternalSemaphoresAsync(&sem, &w, 1, s);
    } else {
      d.wait_cpu(v);
    }
  };
  auto hip_signal = [&](uint64_t v) {
    if (sem) {
      hipExternalSemaphoreSignalParams sp{};
      sp.params.fence.value = v;
      hip.hipSignalExternalSemaphoresAsync(&sem, &sp, 1, s);
    } else {
      hip.hipStreamSynchronize(s);
      d.q->Signal(d.fence.Get(), v);  // CPU-ordered fallback
    }
  };

  const uint64_t size = 3840ull * 2160 * 8;  // one 4K RGBA16F surface
  const size_t words = size / 4;
  auto upload = make_buffer(d.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, size);
  auto readback = make_buffer(d.dev.Get(), D3D12_HEAP_TYPE_READBACK, size);
  {
    uint32_t* m = nullptr;
    upload->Map(0, nullptr, (void**)&m);
    for (size_t i = 0; i < words; ++i) m[i] = pattern(i);
    upload->Unmap(0, nullptr);
  }

  // Native baseline for DtoD bandwidth.
  void* native = nullptr;
  double native_gbps = 0;
  if (hip.hipMalloc(&native, size) == hipSuccess) {
    std::vector<double> t;
    for (int i = 0; i < iters; ++i) {
      hip.hipEventRecord(e0, s);
      hip.hipMemcpyAsync((char*)native + size / 2, native, size / 2, hipMemcpyDeviceToDevice, s);
      hip.hipEventRecord(e1, s);
      hip.hipEventSynchronize(e1);
      float ms = 0;
      hip.hipEventElapsedTime(&ms, e0, e1);
      if (i) t.push_back(ms);
    }
    Stats st = Stats::of(t);
    native_gbps = (size / 2) / (st.p50 * 1e6);
    out["native_dtod_half_ms"] = st.json();
    out["native_dtod_gbps"] = native_gbps;
    hip.hipFree(native);
  }

  Json modes = Json::arr();
  for (int mode = 0; mode < 2; ++mode) {
    Json m = Json::obj();
    const char* mname = mode == 0 ? "d3d12_resource" : "d3d12_heap";
    m["handle_type"] = mname;
    ComPtr<ID3D12Heap> heap;
    ComPtr<ID3D12Resource> shared;
    HANDLE h = nullptr;
    uint64_t import_size = 0;
    if (mode == 0) {
      shared = make_buffer(d.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, size, D3D12_HEAP_FLAG_SHARED);
      if (!shared) { m["error"] = "CreateCommittedResource(SHARED) failed"; modes.push(m); continue; }
      hr = d.dev->CreateSharedHandle(shared.Get(), nullptr, GENERIC_ALL, nullptr, &h);
      D3D12_RESOURCE_DESC sdsc = shared->GetDesc();
      import_size = d.dev->GetResourceAllocationInfo(0, 1, &sdsc).SizeInBytes;
    } else {
      D3D12_HEAP_DESC hd{};
      hd.SizeInBytes = (size + 65535) & ~65535ull;
      hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
      hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
      hr = d.dev->CreateHeap(&hd, IID_PPV_ARGS(&heap));
      if (FAILED(hr)) { m["error"] = "CreateHeap(SHARED) " + hr_str(hr); modes.push(m); continue; }
      D3D12_RESOURCE_DESC rd = buffer_desc(size);
      d.dev->CreatePlacedResource(heap.Get(), 0, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&shared));
      hr = d.dev->CreateSharedHandle(heap.Get(), nullptr, GENERIC_ALL, nullptr, &h);
      import_size = hd.SizeInBytes;
    }
    if (FAILED(hr)) { m["error"] = "CreateSharedHandle " + hr_str(hr); modes.push(m); continue; }

    hipExternalMemoryHandleDesc md{};
    md.type = mode == 0 ? hipExternalMemoryHandleTypeD3D12Resource : hipExternalMemoryHandleTypeD3D12Heap;
    md.handle.win32.handle = h;
    md.size = import_size;
    md.flags = mode == 0 ? hipExternalMemoryDedicated : 0;
    hipExternalMemory_t ext = nullptr;
    hipError_t e = hip.hipImportExternalMemory(&ext, &md);
    m["import"] = e == hipSuccess ? std::string("ok") : std::string("failed: ") + hip.err(e);
    void* ptr = nullptr;
    if (e == hipSuccess) {
      hipExternalMemoryBufferDesc bd{};
      bd.size = size;
      e = hip.hipExternalMemoryGetMappedBuffer(&ptr, ext, &bd);
      m["map"] = e == hipSuccess ? std::string("ok") : std::string("failed: ") + hip.err(e);
    }
    printf("  [%s] import: %s", mname, std::get<std::string>(m["import"].v).c_str());
    if (!ptr) { printf("\n"); CloseHandle(h); if (ext) hip.hipDestroyExternalMemory(ext); modes.push(m); continue; }
    printf(", mapped\n");

    // D3D12 writes, HIP reads.
    d.reset();
    d.list->CopyBufferRegion(shared.Get(), 0, upload.Get(), 0, size);
    d.submit();
    d.q->Signal(d.fence.Get(), ++d.v);
    hip_wait(d.v);
    std::vector<uint32_t> host(words);
    hip.hipMemcpyAsync(host.data(), ptr, size, hipMemcpyDeviceToHost, s);
    hip.hipStreamSynchronize(s);
    size_t bad = 0;
    for (size_t i = 0; i < words; ++i) bad += host[i] != pattern(i);
    m["d3d12_to_hip_ok"] = bad == 0;
    m["d3d12_to_hip_mismatched_words"] = (unsigned long long)bad;

    // HIP writes, D3D12 reads.
    const size_t q4 = size / 4;
    hip.hipMemsetD32Async(ptr, int(0xA5A5A5A5), q4 / 4, s);
    hip.hipMemcpyAsync((char*)ptr + 3 * q4, ptr, q4, hipMemcpyDeviceToDevice, s);
    hip_signal(++d.v);
    d.reset();
    d.q->Wait(d.fence.Get(), d.v);
    d.list->CopyBufferRegion(readback.Get(), 0, shared.Get(), 0, size);
    d.submit();
    d.q->Signal(d.fence.Get(), ++d.v);
    d.wait_cpu(d.v);
    {
      uint32_t* r = nullptr;
      readback->Map(0, nullptr, (void**)&r);
      size_t bad2 = 0;
      for (size_t i = 0; i < words; ++i) {
        bool edge = i < q4 / 4 || i >= 3 * (q4 / 4);
        bad2 += r[i] != (edge ? 0xA5A5A5A5u : pattern(i));
      }
      readback->Unmap(0, nullptr);
      m["hip_to_d3d12_ok"] = bad2 == 0;
      m["hip_to_d3d12_mismatched_words"] = (unsigned long long)bad2;
    }
    printf("    validate D3D12->HIP %s, HIP->D3D12 %s (sync: %s)\n", bad == 0 ? "ok" : "FAIL",
           std::get<bool>(m["hip_to_d3d12_ok"].v) ? "ok" : "FAIL", sem ? "external semaphore" : "CPU");

    // Round trip: D3D12 signal -> HIP wait + tiny op + signal -> D3D12 wait -> CPU.
    std::vector<double> rt_sync, rt_copy, hip_copy;
    for (int pass = 0; pass < 2; ++pass) {
      for (int i = 0; i <= iters; ++i) {
        double t0 = now_ms();
        d.q->Signal(d.fence.Get(), ++d.v);
        hip_wait(d.v);
        hip.hipEventRecord(e0, s);
        if (pass == 0) hip.hipMemsetD32Async(ptr, i, 1, s);
        else hip.hipMemcpyAsync((char*)ptr + size / 2, ptr, size / 2, hipMemcpyDeviceToDevice, s);
        hip.hipEventRecord(e1, s);
        hip_signal(++d.v);
        d.q->Wait(d.fence.Get(), d.v);
        d.q->Signal(d.fence.Get(), ++d.v);
        d.wait_cpu(d.v);
        double t1 = now_ms();
        if (i == 0) continue;  // warm-up
        (pass == 0 ? rt_sync : rt_copy).push_back(t1 - t0);
        if (pass == 1) {
          hip.hipEventSynchronize(e1);
          float ms = 0;
          hip.hipEventElapsedTime(&ms, e0, e1);
          hip_copy.push_back(ms);
        }
      }
    }
    Stats ss = Stats::of(rt_sync), sc = Stats::of(rt_copy), hc = Stats::of(hip_copy);
    m["roundtrip_sync_only_ms"] = ss.json();
    m["roundtrip_with_33mb_dtod_ms"] = sc.json();
    // HIP events around the imported copy read near zero here; the wall-clock delta is the copy cost.
    m["hip_event_dtod_33mb_ms_unreliable"] = hc.json();
    double copy_ms = sc.p50 - ss.p50;
    m["dtod_33mb_cost_ms"] = copy_ms;
    printf("    round trip sync-only: mean %.3f p50 %.3f p95 %.3f ms\n", ss.mean, ss.p50, ss.p95);
    printf("    round trip + 33 MB DtoD: mean %.3f p50 %.3f p95 %.3f ms (copy adds %.3f ms; native DtoD %.0f GB/s, fits Infinity Cache)\n",
           sc.mean, sc.p50, sc.p95, copy_ms, native_gbps);

    hip.hipStreamSynchronize(s);
    hip.hipFree(ptr);
    hip.hipDestroyExternalMemory(ext);
    CloseHandle(h);
    modes.push(m);
  }
  out["modes"] = modes;
  if (sem) hip.hipDestroyExternalSemaphore(sem);
  if (fh) CloseHandle(fh);
  hip.hipEventDestroy(e0);
  hip.hipEventDestroy(e1);
  hip.hipStreamDestroy(s);
  return out;
}
