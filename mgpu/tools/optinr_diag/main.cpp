// optinr_diag: adapter/HIP discovery, D3D12<->HIP interop and cross-adapter copy benchmarks.
#include "diag.h"

#include <filesystem>
#include <cmath>

namespace fs = std::filesystem;

static void dump_str(std::string& o, const std::string& s) {
  o += '"';
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
  }
  o += '"';
}

std::string Json::dump(int ind) const {
  std::string o, pad(ind + 2, ' '), end(ind, ' ');
  if (std::holds_alternative<std::nullptr_t>(v)) return "null";
  if (auto b = std::get_if<bool>(&v)) return *b ? "true" : "false";
  if (auto d = std::get_if<double>(&v)) {
    if (!std::isfinite(*d)) return "null";
    char buf[32];
    if (*d == std::floor(*d) && std::fabs(*d) < 1e15) snprintf(buf, sizeof buf, "%.0f", *d);
    else snprintf(buf, sizeof buf, "%.6g", *d);
    return buf;
  }
  if (auto s = std::get_if<std::string>(&v)) { dump_str(o, *s); return o; }
  if (auto a = std::get_if<Arr>(&v)) {
    if (a->empty()) return "[]";
    o = "[\n";
    for (size_t i = 0; i < a->size(); ++i) o += pad + (*a)[i].dump(ind + 2) + (i + 1 < a->size() ? ",\n" : "\n");
    return o + end + "]";
  }
  auto& ob = std::get<Obj>(v);
  if (ob.empty()) return "{}";
  o = "{\n";
  for (size_t i = 0; i < ob.size(); ++i) {
    o += pad;
    dump_str(o, ob[i].first);
    o += ": " + ob[i].second.dump(ind + 2) + (i + 1 < ob.size() ? ",\n" : "\n");
  }
  return o + end + "}";
}

Stats Stats::of(std::vector<double> s) {
  Stats r;
  if (s.empty()) return r;
  std::sort(s.begin(), s.end());
  double sum = 0;
  for (double x : s) sum += x;
  r.n = s.size();
  r.mean = sum / s.size();
  auto pct = [&](double p) { return s[std::min(s.size() - 1, size_t(p * (s.size() - 1) + 0.5))]; };
  r.p50 = pct(0.5);
  r.p95 = pct(0.95);
  r.min = s.front();
  r.max = s.back();
  return r;
}

Json Stats::json() const {
  Json j = Json::obj();
  j["n"] = (unsigned long long)n;
  j["mean"] = mean;
  j["p50"] = p50;
  j["p95"] = p95;
  j["min"] = min;
  j["max"] = max;
  return j;
}

double now_ms() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string hr_str(HRESULT hr) {
  char b[16];
  snprintf(b, sizeof b, "0x%08lX", (unsigned long)hr);
  return b;
}

std::string narrow(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::string luid_str(LUID l) {
  char b[32];
  snprintf(b, sizeof b, "%08lX:%08lX", (unsigned long)l.HighPart, (unsigned long)l.LowPart);
  return b;
}

uint64_t luid_u64(LUID l) { return (uint64_t(uint32_t(l.HighPart)) << 32) | l.LowPart; }

static fs::path default_out_dir() {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  for (fs::path p = fs::path(exe).parent_path(); p.has_parent_path() && p != p.parent_path(); p = p.parent_path())
    if (fs::exists(p / ".git") && fs::exists(p / "CMakeLists.txt")) return p / "data" / "diag";
  return fs::current_path() / "data" / "diag";
}

static bool supported_amd_arch(const std::string& arch) {
  // RDNA2 (gfx103x), RDNA3 (gfx11xx), RDNA4 (gfx12xx).
  return arch.rfind("gfx103", 0) == 0 || arch.rfind("gfx11", 0) == 0 || arch.rfind("gfx12", 0) == 0;
}

static const char* rdna_gen(const std::string& arch) {
  if (arch.rfind("gfx12", 0) == 0) return "RDNA4";
  if (arch.rfind("gfx11", 0) == 0) return "RDNA3";
  if (arch.rfind("gfx103", 0) == 0) return "RDNA2";
  return "unknown";
}

static bool has_output(IDXGIAdapter4* a) {
  ComPtr<IDXGIOutput> o;
  return SUCCEEDED(a->EnumOutputs(0, &o));
}

int wmain(int argc, wchar_t** argv) {
  int iters = 60;
  bool skip_hip = false, skip_xa = false;
  fs::path out_dir;
  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"--iters" && i + 1 < argc) iters = std::max(5, _wtoi(argv[++i]));
    else if (a == L"--out" && i + 1 < argc) out_dir = argv[++i];
    else if (a == L"--no-hip") skip_hip = true;
    else if (a == L"--no-xadapter") skip_xa = true;
    else if (a == L"--debug") {
      ComPtr<ID3D12Debug> dbg;
      if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    } else {
      printf("usage: optinr_diag [--iters N] [--out DIR] [--no-hip] [--no-xadapter] [--debug]\n");
      return 2;
    }
  }
  if (out_dir.empty()) out_dir = default_out_dir();

  Json report = Json::obj();
  {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[64];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    report["timestamp"] = b;
    report["iterations"] = iters;
  }

  // DXGI adapters
  ComPtr<IDXGIFactory6> factory;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) { printf("CreateDXGIFactory2 failed\n"); return 1; }
  std::vector<AdapterInfo> adapters;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter4> ad;
    if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_UNSPECIFIED, IID_PPV_ARGS(&ad)) == DXGI_ERROR_NOT_FOUND) break;
    if (!ad) continue;
    AdapterInfo info;
    ad->GetDesc3(&info.desc);
    if (info.desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) continue;
    info.adapter = ad;
    info.pci = pci_addr_for_luid(info.desc.AdapterLuid, &info.kmt_type);
    adapters.push_back(std::move(info));
  }

  printf("== DXGI adapters ==\n");
  Json jad = Json::arr();
  for (size_t i = 0; i < adapters.size(); ++i) {
    auto& a = adapters[i];
    auto& d = a.desc;
    bool out = has_output(a.adapter.Get());
    printf("[%zu] %s  vendor=%04X device=%04X rev=%02X  LUID=%s  VRAM=%.1f GiB  PCI %d:%d.%d  %s\n", i,
           narrow(d.Description).c_str(), d.VendorId, d.DeviceId, d.Revision, luid_str(d.AdapterLuid).c_str(),
           d.DedicatedVideoMemory / 1073741824.0, a.pci.bus, a.pci.dev, a.pci.fn, out ? "[display]" : "");
    Json j = Json::obj();
    j["index"] = (unsigned)i;
    j["name"] = narrow(d.Description);
    char b[16];
    snprintf(b, sizeof b, "0x%04X", d.VendorId); j["vendor_id"] = b;
    snprintf(b, sizeof b, "0x%04X", d.DeviceId); j["device_id"] = b;
    snprintf(b, sizeof b, "0x%08X", d.SubSysId); j["subsys_id"] = b;
    j["revision"] = (unsigned)d.Revision;
    j["luid"] = luid_str(d.AdapterLuid);
    j["dedicated_vram_bytes"] = (unsigned long long)d.DedicatedVideoMemory;
    j["shared_system_memory_bytes"] = (unsigned long long)d.SharedSystemMemory;
    j["has_display_output"] = out;
    bool indirect = (a.kmt_type >> 6) & 1;
    j["kmt_adapter_type"] = a.kmt_type;
    j["indirect_display"] = indirect;
    j["physical"] = a.pci.bus >= 0 && !indirect;
    if (a.pci.bus < 0) printf("      no PCI location: %s adapter, excluded from benchmarks\n", indirect ? "indirect-display" : "virtual");
    Json p = Json::obj();
    p["bus"] = a.pci.bus; p["device"] = a.pci.dev; p["function"] = a.pci.fn;
    j["pci"] = p;
    Json links = pcie_links_for(a.pci);
    if (auto arr = std::get_if<Json::Arr>(&links.v)) {
      for (auto& l : *arr) {
        auto& o = std::get<Json::Obj>(l.v);
        auto get = [&](const char* k) -> std::string {
          for (auto& [kk, vv] : o)
            if (kk == k) return std::holds_alternative<std::string>(vv.v) ? std::get<std::string>(vv.v) : vv.dump();
          return "?";
        };
        if (get("current_gts") == "?") printf("      %-11s %s  %s\n", get("role").c_str(), get("location").c_str(), get("name").c_str());
        else printf("      %-11s %s  %s  cur %s GT/s x%s  max %s GT/s x%s\n", get("role").c_str(), get("location").c_str(),
                    get("name").c_str(), get("current_gts").c_str(), get("current_width").c_str(), get("max_gts").c_str(),
                    get("max_width").c_str());
      }
    }
    j["pcie_links"] = links;
    Json adl = adl_link_for_bus(a.pci.bus);
    if (std::holds_alternative<Json::Obj>(adl.v)) {
      int gen = int(std::get<double>(adl["pcie_gen"].v)), lanes = int(std::get<double>(adl["bus_lanes"].v));
      printf("      slot link (ADL): PCIe Gen%d x%d\n", gen, lanes);
    }
    j["slot_link_adl"] = adl;
    jad.push(j);
  }
  report["adapters"] = jad;

  // HIP
  HipApi hip;
  bool hip_ok = false;
  if (!skip_hip) {
    char why[256] = {};
    printf("\n== HIP (amdhip64_7.dll, dynamic) ==\n");
    hip_ok = load_hip(hip, L"amdhip64_7.dll", why, sizeof why);
    if (!hip_ok) {
      printf("HIP unavailable: %s\n", why);
      report["hip"] = Json::obj();
      report["hip"]["available"] = false;
      report["hip"]["error"] = why;
    } else {
      report["hip"] = hip_enumerate(hip, adapters);
    }
  }

  // (a) interop per matched adapter
  if (hip_ok) {
    Json ja = Json::arr();
    for (auto& a : adapters) {
      if (a.hip_index < 0) continue;
      printf("\n== (a) D3D12 -> HIP interop on %s (HIP %d) ==\n", narrow(a.desc.Description).c_str(), a.hip_index);
      ja.push(bench_hip_interop(hip, a, iters));
    }
    report["hip_interop"] = ja;
  }

  // (b) cross-adapter
  std::vector<AdapterInfo*> amd;
  for (auto& a : adapters)
    if (a.desc.VendorId == 0x1002 && a.pci.bus >= 0 && !((a.kmt_type >> 6) & 1)) amd.push_back(&a);
  if (!skip_xa && amd.size() >= 2) {
    printf("\n== (b) cross-adapter copy ==\n");
    report["cross_adapter"] = bench_cross_adapter(*amd[0], *amd[1], iters);
  } else if (amd.size() < 2) {
    report["cross_adapter"] = "skipped: fewer than 2 AMD adapters";
  }

  // (c) present path
  report["present_path"] = "TODO: own swapchain on the second adapter vs DComp overlay, measured with PresentMon";

  // Topology
  printf("\n== Topology recommendation ==\n");
  std::vector<AdapterInfo*> sup;
  for (auto* a : amd) if (a->gcn_arch.empty() || supported_amd_arch(a->gcn_arch)) sup.push_back(a);
  Json topo = Json::obj();
  topo["supported_amd_gpus"] = (unsigned)sup.size();
  if (sup.empty()) {
    printf("No supported AMD GPU (RDNA2+). DLSS-NR on AMD is unavailable.\n");
    topo["mode"] = "none";
  } else if (sup.size() == 1) {
    std::string gen = rdna_gen(sup[0]->gcn_arch);
    double scale = (gen == "RDNA4") ? 1.0 : 0.75;
    printf("1 supported AMD GPU (%s, %s): Single-GPU Inline, NRScale %.2f\n", narrow(sup[0]->desc.Description).c_str(),
           gen.c_str(), scale);
    topo["mode"] = "single_gpu_inline";
    topo["nr_scale_default"] = scale;
  } else {
    AdapterInfo* render = sup[0];
    for (auto* a : sup) if (has_output(a->adapter.Get())) { render = a; break; }
    AdapterInfo* neural = (render == sup[0]) ? sup[1] : sup[0];
    printf("%zu supported AMD GPUs: MGPU Coprocessor. Render = %s (PCI bus %d, display), Neural = PCI bus %d. Roles swappable.\n",
           sup.size(), narrow(render->desc.Description).c_str(), render->pci.bus, neural->pci.bus);
    topo["mode"] = "mgpu_coprocessor";
    topo["render_luid"] = luid_str(render->desc.AdapterLuid);
    topo["neural_luid"] = luid_str(neural->desc.AdapterLuid);
    topo["roles_swappable"] = true;
    topo["nr_scale_default"] = (std::string(rdna_gen(neural->gcn_arch)) == "RDNA4") ? 1.0 : 0.75;
  }
  // Step 2 gate: 4K package (pipelined) within a third of a frame at 60 and 120 fps.
  if (auto* xa = std::get_if<Json::Obj>(&report["cross_adapter"].v)) {
    double best = 1e9, fast_half = 1e9;
    for (auto& [k, val] : *xa) {
      if (k != "runs") continue;
      for (auto& r : std::get<Json::Arr>(val.v)) {
        auto& o = std::get<Json::Obj>(r.v);
        auto field = [&](const char* name) -> const Json* {
          for (auto& [kk, vv] : o) if (kk == name) return &vv;
          return nullptr;
        };
        const Json* var = field("variant");
        if (!var || std::get<std::string>(var->v) != "4k_rgba16f" || field("background_load") || field("error")) continue;
        if (auto* p = field("pipelined_ms_per_package")) best = std::min(best, std::get<double>(p->v));
        for (const char* h : {"producer_gpu_ms", "consumer_gpu_ms"})
          if (auto* s = field(h))
            for (auto& [sk, sv] : std::get<Json::Obj>(s->v))
              if (sk == "p50") fast_half = std::min(fast_half, std::get<double>(sv.v));
      }
    }
    if (best < 1e9) {
      Json g = Json::obj();
      g["pipelined_4k_rgba16f_ms"] = best;
      g["budget_60fps_ms"] = 1000.0 / 60 / 3;
      g["budget_120fps_ms"] = 1000.0 / 120 / 3;
      g["go_60fps"] = best <= 1000.0 / 60 / 3;
      g["go_120fps"] = best <= 1000.0 / 120 / 3;
      g["projected_ms_if_both_links_match_faster"] = fast_half;
      printf("Step 2 gate (4K RGBA16F package, pipelined): %.2f ms vs budget %.2f ms @60 / %.2f ms @120 -> %s\n", best,
             1000.0 / 60 / 3, 1000.0 / 120 / 3, best <= 1000.0 / 60 / 3 ? "GO @60" : "NO-GO");
      printf("  projected with both links as fast as the faster card: %.2f ms per package\n", fast_half);
      topo["gate"] = g;
    }
  }
  report["topology"] = topo;

  std::error_code ec;
  fs::create_directories(out_dir, ec);
  std::string ts = std::get<std::string>(report["timestamp"].v);
  std::replace(ts.begin(), ts.end(), ':', '-');
  fs::path out = out_dir / (ts + ".json");
  if (FILE* f = _wfopen(out.c_str(), L"wb")) {
    std::string s = report.dump() + "\n";
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    printf("\nJSON: %s\n", out.string().c_str());
  } else {
    printf("\nfailed to write %s\n", out.string().c_str());
  }
  return 0;
}
