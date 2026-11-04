#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "hip_min.h"

using Microsoft::WRL::ComPtr;

// Ordered JSON value, enough for the diag report.
struct Json {
  using Arr = std::vector<Json>;
  using Obj = std::vector<std::pair<std::string, Json>>;
  std::variant<std::nullptr_t, bool, double, std::string, Arr, Obj> v;
  Json() : v(nullptr) {}
  Json(bool b) : v(b) {}
  Json(int i) : v(double(i)) {}
  Json(unsigned i) : v(double(i)) {}
  Json(long long i) : v(double(i)) {}
  Json(unsigned long long i) : v(double(i)) {}
  Json(unsigned long i) : v(double(i)) {}
  Json(double d) : v(d) {}
  Json(const char* s) : v(std::string(s)) {}
  Json(std::string s) : v(std::move(s)) {}
  static Json arr() { Json j; j.v = Arr{}; return j; }
  static Json obj() { Json j; j.v = Obj{}; return j; }
  Json& operator[](const std::string& k) {
    if (!std::holds_alternative<Obj>(v)) v = Obj{};
    auto& o = std::get<Obj>(v);
    for (auto& [key, val] : o) if (key == k) return val;
    o.emplace_back(k, Json());
    return o.back().second;
  }
  void push(Json j) {
    if (!std::holds_alternative<Arr>(v)) v = Arr{};
    std::get<Arr>(v).push_back(std::move(j));
  }
  std::string dump(int ind = 0) const;
};

struct Stats {
  double mean = 0, p50 = 0, p95 = 0, min = 0, max = 0;
  size_t n = 0;
  static Stats of(std::vector<double> s);
  Json json() const;
};

double now_ms();
std::string hr_str(HRESULT hr);
std::string narrow(const wchar_t* w);
std::string luid_str(LUID l);
uint64_t luid_u64(LUID l);

struct PciAddr { int bus = -1, dev = -1, fn = -1; };

struct AdapterInfo {
  ComPtr<IDXGIAdapter4> adapter;
  DXGI_ADAPTER_DESC3 desc{};
  PciAddr pci;
  uint32_t kmt_type = 0;  // D3DKMT_ADAPTERTYPE bits
  int hip_index = -1;
  std::string gcn_arch;
};

// pcie.cpp
PciAddr pci_addr_for_luid(LUID luid, uint32_t* kmt_type);
Json pcie_links_for(const PciAddr& a);
Json adl_link_for_bus(int bus);  // driver-reported slot link via ADL PMLOG

// hip_interop.cpp
Json hip_enumerate(HipApi& hip, std::vector<AdapterInfo>& adapters);
Json bench_hip_interop(HipApi& hip, AdapterInfo& a, int iters);

// xadapter.cpp
Json bench_cross_adapter(AdapterInfo& a, AdapterInfo& b, int iters);
