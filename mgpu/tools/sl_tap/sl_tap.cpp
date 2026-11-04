// Streamline tag tap: IAT-hooks the game exe's sl.interposer imports, logs SL calls and dumps one frame's tagged resources.
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>

#include <ffx_api.h>
#include <ffx_api_types.h>
#include <ffx_framegeneration.h>
#include <ffx_upscale.h>
#include <dx12/ffx_api_framegeneration_dx12.h>

using Microsoft::WRL::ComPtr;

static std::mutex g_mx, g_logmx;
static FILE* g_log;
static std::wstring g_out;
static uint32_t g_capture_after = 600;  // frames after the first HUD-less tag
static std::atomic<uint32_t> g_frames;

static void logf(const char* fmt, ...) {
  if (!g_log) return;
  std::lock_guard<std::mutex> l(g_logmx);
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  va_list a;
  va_start(a, fmt);
  vfprintf(g_log, fmt, a);
  va_end(a);
  fputc('\n', g_log);
  fflush(g_log);
}

static int pci_bus(LUID luid) {
  D3DKMT_OPENADAPTERFROMLUID open{};
  open.AdapterLuid = luid;
  if (D3DKMTOpenAdapterFromLuid(&open) != 0) return -1;
  D3DKMT_ADAPTERADDRESS addr{};
  D3DKMT_QUERYADAPTERINFO q{};
  q.hAdapter = open.hAdapter;
  q.Type = KMTQAITYPE_ADAPTERADDRESS;
  q.pPrivateDriverData = &addr;
  q.PrivateDriverDataSize = sizeof addr;
  int bus = D3DKMTQueryAdapterInfo(&q) == 0 && addr.BusNumber != 0xFFFFFFFF ? int(addr.BusNumber) : -1;
  D3DKMT_CLOSEADAPTER close{open.hAdapter};
  D3DKMTCloseAdapter(&close);
  return bus;
}

static const char* module_of(const void* p) {
  static thread_local char name[MAX_PATH];
  HMODULE m = nullptr;
  name[0] = 0;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &m)) {
    GetModuleFileNameA(m, name, MAX_PATH);
    if (const char* s = strrchr(name, '\\')) return s + 1;
  }
  return name;
}

// ---- capture -------------------------------------------------------------------------------
struct Pending {
  std::string name;
  uint32_t type;
  ComPtr<ID3D12Resource> readback;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
  UINT rows;
  DXGI_FORMAT fmt;
  sl::Extent ext;
};
enum class Cap { Waiting, Armed, Recorded, Done };
static std::atomic<Cap> g_cap{Cap::Waiting};
static uint32_t g_first_hudless_frame = UINT32_MAX, g_cap_frame = 0, g_recorded_at = 0;
static std::vector<Pending> g_pending;
static std::string g_cap_json;

static std::wstring g_cap_dir;
static int g_cap_index;
static bool g_auto_used;

// Auto-capture once at capture_after frames, then once per capture.trigger file (consumed).
static bool should_arm(uint32_t n, uint32_t first) {
  bool fire = !g_auto_used && first != UINT32_MAX && n >= first + g_capture_after;
  if (!fire && DeleteFileW((g_out + L"\\capture.trigger").c_str())) fire = true;
  if (!fire) return false;
  g_auto_used = true;
  g_cap_dir = g_out + L"\\cap" + std::to_wstring(++g_cap_index);
  CreateDirectoryW(g_cap_dir.c_str(), nullptr);
  return true;
}

// Copies subresource 0 of res into a readback buffer on cl, restoring its state afterwards.
static void record_copy(const char* name, uint32_t type, ID3D12Resource* res, D3D12_RESOURCE_STATES before, const sl::Extent& ext,
                        ID3D12GraphicsCommandList* cl) {
  if (!res) return;
  D3D12_RESOURCE_DESC d = res->GetDesc();
  if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1) {
    logf("capture: %s skipped (dim %d samples %u)", name, d.Dimension, d.SampleDesc.Count);
    return;
  }
  ComPtr<ID3D12Device> dev;
  res->GetDevice(IID_PPV_ARGS(&dev));
  Pending p{};
  p.name = name;
  p.type = type;
  p.fmt = d.Format;
  p.ext = ext;
  UINT64 row_bytes, total;
  dev->GetCopyableFootprints(&d, 0, 1, 0, &p.fp, &p.rows, &row_bytes, &total);
  D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = total;
  bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&p.readback)))) return;
  D3D12_RESOURCE_BARRIER b{};
  b.Transition.pResource = res;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  bool tr = before != D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (tr) cl->ResourceBarrier(1, &b);
  D3D12_TEXTURE_COPY_LOCATION dst{p.readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  dst.PlacedFootprint = p.fp;
  D3D12_TEXTURE_COPY_LOCATION src{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  src.SubresourceIndex = 0;
  cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  if (tr) {
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    cl->ResourceBarrier(1, &b);
  }
  logf("capture: %s %ux%u fmt %u state 0x%x queued", name, p.fp.Footprint.Width, p.fp.Footprint.Height, d.Format, before);
  g_pending.push_back(p);
}

static void write_pending() {
  std::string j = "{\n  \"frame\": " + std::to_string(g_cap_frame) + ",\n  \"tags\": [\n";
  for (size_t i = 0; i < g_pending.size(); ++i) {
    auto& p = g_pending[i];
    wchar_t name[96];
    swprintf(name, 96, L"%hs_fmt%u_%ux%u.raw", p.name.c_str(), p.fmt, p.fp.Footprint.Width, p.fp.Footprint.Height);
    void* m = nullptr;
    D3D12_RANGE r{0, (SIZE_T)p.fp.Footprint.RowPitch * p.rows};
    if (SUCCEEDED(p.readback->Map(0, &r, &m))) {
      if (FILE* f = _wfopen((g_cap_dir + L"\\" + name).c_str(), L"wb")) {
        fwrite(m, 1, r.End, f);
        fclose(f);
      }
      D3D12_RANGE none{0, 0};
      p.readback->Unmap(0, &none);
    }
    char e[512];
    snprintf(e, sizeof e,
             "    {\"name\": \"%s\", \"type\": %u, \"file\": \"%ls\", \"format\": %u, \"width\": %u, \"height\": %u, \"row_pitch\": %u, \"rows\": %u, "
             "\"extent\": [%u, %u, %u, %u]}%s\n",
             p.name.c_str(), p.type, name, p.fmt, p.fp.Footprint.Width, p.fp.Footprint.Height, p.fp.Footprint.RowPitch, p.rows, p.ext.left, p.ext.top,
             p.ext.width, p.ext.height, i + 1 < g_pending.size() ? "," : "");
    j += e;
  }
  j += "  ],\n" + g_cap_json + "}\n";
  if (FILE* f = _wfopen((g_cap_dir + L"\\capture.json").c_str(), L"wb")) {
    fwrite(j.data(), 1, j.size(), f);
    fclose(f);
  }
  logf("capture %d: wrote %zu resources for frame %u", g_cap_index, g_pending.size(), g_cap_frame);
  g_pending.clear();
}

// ---- hooks ---------------------------------------------------------------------------------
using PFun_D3D12CreateDevice = HRESULT WINAPI(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using PFun_CreateDXGIFactory1 = HRESULT WINAPI(REFIID, void**);
static PFun_slInit* r_slInit;
static PFun_slSetTagForFrame* r_slSetTagForFrame;
static PFun_slSetConstants* r_slSetConstants;
static PFun_slGetNewFrameToken* r_slGetNewFrameToken;
static PFun_slEvaluateFeature* r_slEvaluateFeature;
static PFun_slGetFeatureFunction* r_slGetFeatureFunction;
static PFun_slSetFeatureLoaded* r_slSetFeatureLoaded;
static PFun_slIsFeatureSupported* r_slIsFeatureSupported;
static PFun_slSetD3DDevice* r_slSetD3DDevice;
static PFun_D3D12CreateDevice* r_D3D12CreateDevice;
static PFun_CreateDXGIFactory1* r_CreateDXGIFactory1;
static PFun_slDLSSGSetOptions* r_slDLSSGSetOptions;

static sl::Result hk_slInit(const sl::Preferences& p, uint64_t sdk) {
  std::string feats;
  for (uint32_t i = 0; i < p.numFeaturesToLoad && p.featuresToLoad; ++i) feats += std::to_string(p.featuresToLoad[i]) + " ";
  auto r = r_slInit(p, sdk);
  logf("slInit sdk 0x%llx flags 0x%llx renderAPI %u engine %u appId %u features [%s] -> %d", (unsigned long long)sdk,
       (unsigned long long)p.flags, (unsigned)p.renderAPI, (unsigned)p.engine, p.applicationId, feats.c_str(), (int)r);
  return r;
}

static HRESULT WINAPI hk_D3D12CreateDevice(IUnknown* ad, D3D_FEATURE_LEVEL fl, REFIID iid, void** out) {
  HRESULT hr = r_D3D12CreateDevice(ad, fl, iid, out);
  if (SUCCEEDED(hr) && out && *out) {
    ComPtr<ID3D12Device> d;
    if (SUCCEEDED(((IUnknown*)*out)->QueryInterface(IID_PPV_ARGS(&d)))) {
      LUID l = d->GetAdapterLuid();
      logf("D3D12CreateDevice adapter %p (%s) -> device %p luid %08lx:%08lx bus %d", ad, ad ? "explicit" : "default", *out, l.HighPart,
           l.LowPart, pci_bus(l));
    }
  } else {
    logf("D3D12CreateDevice adapter %p -> 0x%08lx", ad, hr);
  }
  return hr;
}

static HRESULT WINAPI hk_CreateDXGIFactory1(REFIID iid, void** out) {
  HRESULT hr = r_CreateDXGIFactory1(iid, out);
  logf("CreateDXGIFactory1 -> 0x%08lx factory %p vtable in %s", hr, out ? *out : nullptr, out && *out ? module_of(**(void***)out) : "");
  return hr;
}

static sl::Result hk_slSetD3DDevice(void* d) {
  logf("slSetD3DDevice %p", d);
  return r_slSetD3DDevice(d);
}

static sl::Result hk_slGetNewFrameToken(sl::FrameToken*& tok, const uint32_t* idx) {
  auto r = r_slGetNewFrameToken(tok, idx);
  uint32_t n = ++g_frames;
  uint32_t f = tok ? (uint32_t)*tok : 0;
  if (n <= 3 || n % 1000 == 0) logf("slGetNewFrameToken #%u frame %u (idx %s)", n, f, idx ? std::to_string(*idx).c_str() : "auto");
  std::lock_guard<std::mutex> l(g_mx);
  Cap c = g_cap.load();
  if (c == Cap::Waiting && g_first_hudless_frame != UINT32_MAX && should_arm(n, g_first_hudless_frame)) {
    g_cap_frame = f;
    g_cap.store(Cap::Armed);
  } else if (c == Cap::Recorded && n >= g_recorded_at + 30) {
    write_pending();
    g_cap.store(Cap::Waiting);
  }
  return r;
}

static const char* lifecycle_name(sl::ResourceLifecycle l) {
  return l == sl::eOnlyValidNow ? "OnlyValidNow" : l == sl::eValidUntilPresent ? "ValidUntilPresent" : "ValidUntilEvaluate";
}

static sl::Result hk_slSetTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& vp, const sl::ResourceTag* tags, uint32_t n,
                                      sl::CommandBuffer* cb) {
  static uint32_t logged;
  uint32_t f = (uint32_t)frame;
  std::string types;
  for (uint32_t i = 0; i < n; ++i) types += std::to_string(tags[i].type) + (tags[i].resource ? "" : "(null)") + " ";
  bool verbose = logged < 40 || g_cap.load() == Cap::Armed;
  if (verbose) {
    ++logged;
    logf("slSetTagForFrame frame %u vp %u n %u cmd %p [%s]", f, (uint32_t)vp, n, cb, types.c_str());
    for (uint32_t i = 0; i < n; ++i) {
      auto& t = tags[i];
      if (!t.resource || !t.resource->native) continue;
      auto d = ((ID3D12Resource*)t.resource->native)->GetDesc();
      logf("  tag %u res %p %llux%u fmt %u mips %u flags 0x%x state 0x%x lifecycle %s extent (%u,%u %ux%u) next %p", t.type,
           t.resource->native, (unsigned long long)d.Width, d.Height, d.Format, d.MipLevels, d.Flags, t.resource->state,
           lifecycle_name(t.lifecycle), t.extent.left, t.extent.top, t.extent.width, t.extent.height, t.next);
    }
  }
  {
    std::lock_guard<std::mutex> l(g_mx);
    for (uint32_t i = 0; i < n && g_first_hudless_frame == UINT32_MAX; ++i)
      if (tags[i].type == sl::kBufferTypeHUDLessColor && tags[i].resource) {
        g_first_hudless_frame = g_frames.load();
        logf("first HUD-less colour tag at token #%u", g_first_hudless_frame);
      }
    if (g_cap.load() == Cap::Armed && f == g_cap_frame) {
      ComPtr<ID3D12GraphicsCommandList> cl;
      if (cb && SUCCEEDED(((IUnknown*)cb)->QueryInterface(IID_PPV_ARGS(&cl)))) {
        for (uint32_t i = 0; i < n; ++i)
          if (tags[i].resource) {
            char nm[32];
            snprintf(nm, sizeof nm, "sl_tag%02u", tags[i].type);
            record_copy(nm, tags[i].type, (ID3D12Resource*)tags[i].resource->native, (D3D12_RESOURCE_STATES)tags[i].resource->state,
                        tags[i].extent, cl.Get());
          }
      } else {
        logf("capture: tags without a command list on frame %u (not copied)", f);
      }
    } else if (g_cap.load() == Cap::Armed && f != g_cap_frame && !g_pending.empty()) {
      g_recorded_at = g_frames.load();
      g_cap.store(Cap::Recorded);
    }
  }
  return r_slSetTagForFrame(frame, vp, tags, n, cb);
}

static void m4(std::string& s, const char* n, const sl::float4x4& m) {
  char b[512];
  snprintf(b, sizeof b, "    \"%s\": [[%g,%g,%g,%g],[%g,%g,%g,%g],[%g,%g,%g,%g],[%g,%g,%g,%g]],\n", n, m[0].x, m[0].y, m[0].z, m[0].w, m[1].x,
           m[1].y, m[1].z, m[1].w, m[2].x, m[2].y, m[2].z, m[2].w, m[3].x, m[3].y, m[3].z, m[3].w);
  s += b;
}

static sl::Result hk_slSetConstants(const sl::Constants& c, const sl::FrameToken& frame, const sl::ViewportHandle& vp) {
  static uint32_t logged;
  uint32_t f = (uint32_t)frame;
  if (logged++ < 5)
    logf("slSetConstants frame %u vp %u jitter (%g,%g) mvecScale (%g,%g) near %g far %g fov %g aspect %g depthInv %u camMotion %u mv3D %u reset %u "
         "dilated %u jittered %u",
         f, (uint32_t)vp, c.jitterOffset.x, c.jitterOffset.y, c.mvecScale.x, c.mvecScale.y, c.cameraNear, c.cameraFar, c.cameraFOV,
         c.cameraAspectRatio, (unsigned)c.depthInverted, (unsigned)c.cameraMotionIncluded, (unsigned)c.motionVectors3D, (unsigned)c.reset,
         (unsigned)c.motionVectorsDilated, (unsigned)c.motionVectorsJittered);
  {
    std::lock_guard<std::mutex> l(g_mx);
    if (g_cap.load() == Cap::Armed && f == g_cap_frame) {
      std::string s = "  \"constants\": {\n";
      m4(s, "cameraViewToClip", c.cameraViewToClip);
      m4(s, "clipToCameraView", c.clipToCameraView);
      m4(s, "clipToLensClip", c.clipToLensClip);
      m4(s, "clipToPrevClip", c.clipToPrevClip);
      m4(s, "prevClipToClip", c.prevClipToClip);
      char b[1024];
      snprintf(b, sizeof b,
               "    \"viewport\": %u, \"jitterOffset\": [%g,%g], \"mvecScale\": [%g,%g], \"cameraPinholeOffset\": [%g,%g],\n"
               "    \"cameraPos\": [%g,%g,%g], \"cameraUp\": [%g,%g,%g], \"cameraRight\": [%g,%g,%g], \"cameraFwd\": [%g,%g,%g],\n"
               "    \"cameraNear\": %g, \"cameraFar\": %g, \"cameraFOV\": %g, \"cameraAspectRatio\": %g, \"motionVectorsInvalidValue\": %g,\n"
               "    \"depthInverted\": %u, \"cameraMotionIncluded\": %u, \"motionVectors3D\": %u, \"reset\": %u, \"orthographicProjection\": %u,\n"
               "    \"motionVectorsDilated\": %u, \"motionVectorsJittered\": %u, \"structVersion\": %u\n  }\n",
               (uint32_t)vp, c.jitterOffset.x, c.jitterOffset.y, c.mvecScale.x, c.mvecScale.y, c.cameraPinholeOffset.x, c.cameraPinholeOffset.y,
               c.cameraPos.x, c.cameraPos.y, c.cameraPos.z, c.cameraUp.x, c.cameraUp.y, c.cameraUp.z, c.cameraRight.x, c.cameraRight.y,
               c.cameraRight.z, c.cameraFwd.x, c.cameraFwd.y, c.cameraFwd.z, c.cameraNear, c.cameraFar, c.cameraFOV, c.cameraAspectRatio,
               c.motionVectorsInvalidValue, (unsigned)c.depthInverted, (unsigned)c.cameraMotionIncluded, (unsigned)c.motionVectors3D,
               (unsigned)c.reset, (unsigned)c.orthographicProjection, (unsigned)c.motionVectorsDilated, (unsigned)c.motionVectorsJittered,
               (unsigned)c.structVersion);
      g_cap_json = s + b;
    }
  }
  return r_slSetConstants(c, frame, vp);
}

static sl::Result hk_slEvaluateFeature(sl::Feature feat, const sl::FrameToken& frame, const sl::BaseStructure** in, uint32_t n,
                                       sl::CommandBuffer* cb) {
  static uint32_t logged;
  if (logged++ < 10) logf("slEvaluateFeature feature %u frame %u inputs %u cmd %p", feat, (uint32_t)frame, n, cb);
  return r_slEvaluateFeature(feat, frame, in, n, cb);
}

static sl::Result hk_slDLSSGSetOptions(const sl::ViewportHandle& vp, const sl::DLSSGOptions& o) {
  static uint32_t last = UINT32_MAX;
  if ((uint32_t)o.mode != last) {
    last = (uint32_t)o.mode;
    logf("slDLSSGSetOptions vp %u mode %u frames %u flags 0x%x", (uint32_t)vp, (unsigned)o.mode, o.numFramesToGenerate, (unsigned)o.flags);
  }
  return r_slDLSSGSetOptions(vp, o);
}

static sl::Result hk_slGetFeatureFunction(sl::Feature feat, const char* name, void*& fn) {
  auto r = r_slGetFeatureFunction(feat, name, fn);
  logf("slGetFeatureFunction feature %u %s -> %d", feat, name, (int)r);
  if (r == sl::Result::eOk && fn && !strcmp(name, "slDLSSGSetOptions")) {
    r_slDLSSGSetOptions = (PFun_slDLSSGSetOptions*)fn;
    fn = (void*)&hk_slDLSSGSetOptions;
  }
  return r;
}

static sl::Result hk_slSetFeatureLoaded(sl::Feature feat, bool loaded) {
  auto r = r_slSetFeatureLoaded(feat, loaded);
  logf("slSetFeatureLoaded feature %u loaded %d -> %d", feat, loaded, (int)r);
  return r;
}

static sl::Result hk_slIsFeatureSupported(sl::Feature feat, const sl::AdapterInfo& ai) {
  auto r = r_slIsFeatureSupported(feat, ai);
  LUID l{};
  if (ai.deviceLUID && ai.deviceLUIDSizeInBytes >= sizeof l) memcpy(&l, ai.deviceLUID, sizeof l);
  logf("slIsFeatureSupported feature %u luid %08lx:%08lx bus %d -> %d", feat, l.HighPart, l.LowPart, ai.deviceLUID ? pci_bus(l) : -1, (int)r);
  return r;
}

// ---- FidelityFX API (the AMD frame-generation path) ----------------------------------------
using PFun_ffxCreateContext = ffxReturnCode_t(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
using PFun_ffxConfigure = ffxReturnCode_t(ffxContext*, const ffxConfigureDescHeader*);
using PFun_ffxDispatch = ffxReturnCode_t(ffxContext*, const ffxDispatchDescHeader*);
using PFun_ffxQuery = ffxReturnCode_t(ffxContext*, ffxQueryDescHeader*);
static PFun_ffxCreateContext* r_ffxCreateContext;
static PFun_ffxConfigure* r_ffxConfigure;
static PFun_ffxDispatch* r_ffxDispatch;
static PFun_ffxQuery* r_ffxQuery;
static FfxApiResource g_hudless{}, g_ui{};
static void* g_fg_swapchain;
static UINT g_bb_index = UINT_MAX;
static uint32_t g_ffx_prepares, g_ffx_first = UINT32_MAX;

static D3D12_RESOURCE_STATES d3d_state(uint32_t s) {
  uint32_t r = 0;
  if (s & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS) r |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  if (s & FFX_API_RESOURCE_STATE_COMPUTE_READ) r |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  if (s & FFX_API_RESOURCE_STATE_PIXEL_READ) r |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  if (s & FFX_API_RESOURCE_STATE_COPY_SRC) r |= D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (s & FFX_API_RESOURCE_STATE_COPY_DEST) r |= D3D12_RESOURCE_STATE_COPY_DEST;
  if (s & FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT) r |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
  if (s & FFX_API_RESOURCE_STATE_RENDER_TARGET) r |= D3D12_RESOURCE_STATE_RENDER_TARGET;
  if (s & FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT) r |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
  return (D3D12_RESOURCE_STATES)r;
}

static std::string res_str(const FfxApiResource& r) {
  char b[192];
  if (!r.resource) return "null";
  auto d = ((ID3D12Resource*)r.resource)->GetDesc();
  snprintf(b, sizeof b, "%p %llux%u dxgi %u (ffx fmt %u %ux%u) state 0x%x", r.resource, (unsigned long long)d.Width, d.Height, d.Format,
           r.description.format, r.description.width, r.description.height, r.state);
  return b;
}

static bool first_of(uint64_t type) {
  static std::vector<std::pair<uint64_t, uint32_t>> seen;
  for (auto& s : seen)
    if (s.first == type) return ++s.second <= 3;
  seen.push_back({type, 1});
  return true;
}

static std::string chain(const ffxApiHeader* h) {
  std::string s;
  char b[24];
  for (; h; h = h->pNext) {
    snprintf(b, sizeof b, "0x%llx ", (unsigned long long)h->type);
    s += b;
  }
  return s;
}

static ffxReturnCode_t hk_ffxCreateContext(ffxContext* c, ffxCreateContextDescHeader* d, const ffxAllocationCallbacks* m) {
  auto r = r_ffxCreateContext(c, d, m);
  logf("ffxCreateContext [%s] -> %u ctx %p", chain(d).c_str(), r, c ? *c : nullptr);
  return r;
}

static ffxReturnCode_t hk_ffxQuery(ffxContext* c, ffxQueryDescHeader* d) {
  if (first_of(d->type | (1ull << 40))) logf("ffxQuery [%s]", chain(d).c_str());
  return r_ffxQuery(c, d);
}

static ffxReturnCode_t hk_ffxConfigure(ffxContext* c, const ffxConfigureDescHeader* d) {
  for (auto h = d; h; h = h->pNext) {
    bool log = first_of(h->type | (2ull << 40));
    if (h->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION) {
      auto& f = *(const ffxConfigureDescFrameGeneration*)h;
      {
        std::lock_guard<std::mutex> l(g_mx);
        if (g_cap.load() != Cap::Armed) g_hudless = f.HUDLessColor;
        g_fg_swapchain = f.swapChain;
      }
      static int last_en = -1;
      if (log || last_en != (int)f.frameGenerationEnabled)
        logf("ffxConfigure FG: enabled %d async %d flags 0x%x onlyGenerated %d frameID %llu swapchain %p rect (%d,%d %dx%d) hudless %s",
             f.frameGenerationEnabled, f.allowAsyncWorkloads, f.flags, f.onlyPresentGenerated, (unsigned long long)f.frameID, f.swapChain,
             f.generationRect.left, f.generationRect.top, f.generationRect.width, f.generationRect.height, res_str(f.HUDLessColor).c_str());
      last_en = f.frameGenerationEnabled;
    } else if (h->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12) {
      auto& u = *(const ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12*)h;
      {
        std::lock_guard<std::mutex> l(g_mx);
        if (g_cap.load() != Cap::Armed) g_ui = u.uiResource;
      }
      if (log) logf("ffxConfigure register UI: flags 0x%x ui %s", u.flags, res_str(u.uiResource).c_str());
    } else if (log) {
      logf("ffxConfigure type 0x%llx", (unsigned long long)h->type);
    }
  }
  return r_ffxConfigure(c, d);
}

static void prepare_json(const ffxDispatchDescFrameGenerationPrepareV2& p, bool v2, const float* cam) {
  char b[1024];
  snprintf(b, sizeof b,
           "  \"ffx_prepare\": {\"v2\": %d, \"frameID\": %llu, \"flags\": %u, \"renderSize\": [%u,%u], \"jitterOffset\": [%g,%g], "
           "\"motionVectorScale\": [%g,%g], \"frameTimeDelta\": %g, \"cameraNear\": %g, \"cameraFar\": %g, \"cameraFovAngleVertical\": %g, "
           "\"viewSpaceToMetersFactor\": %g, \"cameraPosition\": [%g,%g,%g], \"cameraUp\": [%g,%g,%g], \"cameraRight\": [%g,%g,%g], "
           "\"cameraForward\": [%g,%g,%g]}\n",
           v2, (unsigned long long)p.frameID, p.flags, p.renderSize.width, p.renderSize.height, p.jitterOffset.x, p.jitterOffset.y,
           p.motionVectorScale.x, p.motionVectorScale.y, p.frameTimeDelta, p.cameraNear, p.cameraFar, p.cameraFovAngleVertical,
           p.viewSpaceToMetersFactor, cam[0], cam[1], cam[2], cam[3], cam[4], cam[5], cam[6], cam[7], cam[8], cam[9], cam[10], cam[11]);
  g_cap_json = b;
}

static void on_prepare(const ffxDispatchDescFrameGenerationPrepareV2& p, bool v2, const float* cam) {
  std::lock_guard<std::mutex> l(g_mx);
  uint32_t n = ++g_ffx_prepares;
  if (g_ffx_first == UINT32_MAX) {
    g_ffx_first = n;
    logf("first FG prepare: frameID %llu", (unsigned long long)p.frameID);
  }
  auto* cl = (ID3D12GraphicsCommandList*)p.commandList;
  Cap c = g_cap.load();
  if (c == Cap::Waiting && should_arm(n, g_ffx_first)) {
    g_cap_frame = (uint32_t)p.frameID;
    prepare_json(p, v2, cam);
    record_copy("ffx_depth", 0, (ID3D12Resource*)p.depth.resource, d3d_state(p.depth.state), {}, cl);
    record_copy("ffx_motionvectors", 1, (ID3D12Resource*)p.motionVectors.resource, d3d_state(p.motionVectors.state), {}, cl);
    ComPtr<IDXGISwapChain3> sc;
    if (g_fg_swapchain && SUCCEEDED(((IUnknown*)g_fg_swapchain)->QueryInterface(IID_PPV_ARGS(&sc)))) g_bb_index = sc->GetCurrentBackBufferIndex();
    g_cap.store(Cap::Armed);
  } else if (c == Cap::Armed) {
    // HUD-less colour and UI of the captured frame are complete by the next frame's prepare.
    record_copy("ffx_hudless", 2, (ID3D12Resource*)g_hudless.resource, d3d_state(g_hudless.state), {}, cl);
    record_copy("ffx_ui", 23, (ID3D12Resource*)g_ui.resource, d3d_state(g_ui.state), {}, cl);
    ComPtr<IDXGISwapChain3> sc;
    ComPtr<ID3D12Resource> bb;
    if (g_bb_index != UINT_MAX && SUCCEEDED(((IUnknown*)g_fg_swapchain)->QueryInterface(IID_PPV_ARGS(&sc))) &&
        SUCCEEDED(sc->GetBuffer(g_bb_index, IID_PPV_ARGS(&bb)))) {
      logf("capture: back buffer %u of FG swapchain %p (current %u)", g_bb_index, g_fg_swapchain, sc->GetCurrentBackBufferIndex());
      record_copy("backbuffer", 53, bb.Get(), D3D12_RESOURCE_STATE_PRESENT, {}, cl);
    }
    g_recorded_at = n;
    g_cap.store(Cap::Recorded);
  } else if (c == Cap::Recorded && n >= g_recorded_at + 30) {
    write_pending();
    g_cap.store(Cap::Waiting);
  }
}

static ffxReturnCode_t hk_ffxDispatch(ffxContext* c, const ffxDispatchDescHeader* d) {
  bool log = first_of(d->type | (3ull << 40));
  static const float zero[12]{};
  if (d->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2) {
    auto& p = *(const ffxDispatchDescFrameGenerationPrepareV2*)d;
    if (log)
      logf("ffxDispatch FG prepare v2: frameID %llu flags 0x%x cmd %p render %ux%u jitter (%g,%g) mvScale (%g,%g) near %g far %g fov %g depth %s mv %s",
           (unsigned long long)p.frameID, p.flags, p.commandList, p.renderSize.width, p.renderSize.height, p.jitterOffset.x, p.jitterOffset.y,
           p.motionVectorScale.x, p.motionVectorScale.y, p.cameraNear, p.cameraFar, p.cameraFovAngleVertical, res_str(p.depth).c_str(),
           res_str(p.motionVectors).c_str());
    on_prepare(p, true, p.cameraPosition);
  } else if (d->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE) {
    ffxDispatchDescFrameGenerationPrepareV2 p{};
    memcpy(&p, d, sizeof(ffxDispatchDescFrameGenerationPrepare));
    const float* cam = zero;
    for (auto h = d->pNext; h; h = h->pNext)
      if (h->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO) cam = ((const ffxDispatchDescFrameGenerationPrepareCameraInfo*)h)->cameraPosition;
    if (log)
      logf("ffxDispatch FG prepare: frameID %llu flags 0x%x cmd %p render %ux%u jitter (%g,%g) mvScale (%g,%g) depth %s mv %s chain [%s]",
           (unsigned long long)p.frameID, p.flags, p.commandList, p.renderSize.width, p.renderSize.height, p.jitterOffset.x, p.jitterOffset.y,
           p.motionVectorScale.x, p.motionVectorScale.y, res_str(p.depth).c_str(), res_str(p.motionVectors).c_str(), chain(d).c_str());
    on_prepare(p, false, cam);
  } else if (d->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
    auto& u = *(const ffxDispatchDescUpscale*)d;
    if (log)
      logf("ffxDispatch upscale: render %ux%u -> %ux%u flags 0x%x color %s depth %s mv %s output %s", u.renderSize.width, u.renderSize.height,
           u.upscaleSize.width, u.upscaleSize.height, u.flags, res_str(u.color).c_str(), res_str(u.depth).c_str(),
           res_str(u.motionVectors).c_str(), res_str(u.output).c_str());
  } else if (log) {
    logf("ffxDispatch [%s]", chain(d).c_str());
  }
  return r_ffxDispatch(c, d);
}

// ---- IAT patch -----------------------------------------------------------------------------
struct Hook {
  const char* name;
  void* fn;
  void** real;
};

static int patch_iat(HMODULE mod, const char* dll, const Hook* hooks, size_t nh) {
  auto base = (BYTE*)mod;
  auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
  auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  int patched = 0;
  for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
    if (_stricmp((char*)(base + imp->Name), dll)) continue;
    auto names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
    auto iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++iat) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto name = ((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
      for (size_t h = 0; h < nh; ++h) {
        if (strcmp(name, hooks[h].name)) continue;
        DWORD old;
        VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
        *hooks[h].real = (void*)iat->u1.Function;
        iat->u1.Function = (ULONG_PTR)hooks[h].fn;
        VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
        ++patched;
      }
    }
  }
  return patched;
}

static void init(HMODULE self) {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(self, path, MAX_PATH);
  std::wstring cfg = path;
  cfg = cfg.substr(0, cfg.find_last_of(L'\\')) + L"\\sl_tap.ini";
  g_out = cfg.substr(0, cfg.find_last_of(L'\\'));
  if (FILE* f = _wfopen(cfg.c_str(), L"r")) {
    char line[1024];
    while (fgets(line, sizeof line, f)) {
      std::string s = line;
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
      if (!s.compare(0, 4, "out=")) {
        g_out.assign(s.begin() + 4, s.end());
      } else if (!s.compare(0, 14, "capture_after=")) {
        g_capture_after = (uint32_t)atoi(s.c_str() + 14);
      }
    }
    fclose(f);
  }
  CreateDirectoryW(g_out.c_str(), nullptr);
  g_log = _wfopen((g_out + L"\\sl_tap.log").c_str(), L"a");
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  logf("sl_tap loaded into %ls (pid %lu), sl.interposer %s, capture_after %u", path, GetCurrentProcessId(),
       GetModuleHandleW(L"sl.interposer.dll") ? "loaded" : "not loaded", g_capture_after);
  const Hook hooks[] = {
      {"slInit", (void*)&hk_slInit, (void**)&r_slInit},
      {"slSetTagForFrame", (void*)&hk_slSetTagForFrame, (void**)&r_slSetTagForFrame},
      {"slSetConstants", (void*)&hk_slSetConstants, (void**)&r_slSetConstants},
      {"slGetNewFrameToken", (void*)&hk_slGetNewFrameToken, (void**)&r_slGetNewFrameToken},
      {"slEvaluateFeature", (void*)&hk_slEvaluateFeature, (void**)&r_slEvaluateFeature},
      {"slGetFeatureFunction", (void*)&hk_slGetFeatureFunction, (void**)&r_slGetFeatureFunction},
      {"slSetFeatureLoaded", (void*)&hk_slSetFeatureLoaded, (void**)&r_slSetFeatureLoaded},
      {"slIsFeatureSupported", (void*)&hk_slIsFeatureSupported, (void**)&r_slIsFeatureSupported},
      {"slSetD3DDevice", (void*)&hk_slSetD3DDevice, (void**)&r_slSetD3DDevice},
      {"D3D12CreateDevice", (void*)&hk_D3D12CreateDevice, (void**)&r_D3D12CreateDevice},
      {"CreateDXGIFactory1", (void*)&hk_CreateDXGIFactory1, (void**)&r_CreateDXGIFactory1},
  };
  int n = patch_iat(GetModuleHandleW(nullptr), "sl.interposer.dll", hooks, std::size(hooks));
  const Hook ffx[] = {
      {"ffxCreateContext", (void*)&hk_ffxCreateContext, (void**)&r_ffxCreateContext},
      {"ffxConfigure", (void*)&hk_ffxConfigure, (void**)&r_ffxConfigure},
      {"ffxDispatch", (void*)&hk_ffxDispatch, (void**)&r_ffxDispatch},
      {"ffxQuery", (void*)&hk_ffxQuery, (void**)&r_ffxQuery},
  };
  int nf = patch_iat(GetModuleHandleW(nullptr), "amd_fidelityfx_loader_dx12.dll", ffx, std::size(ffx));
  logf("patched %d sl.interposer and %d FidelityFX loader imports in the exe", n, nf);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(h);
    init(h);
  }
  return TRUE;
}
