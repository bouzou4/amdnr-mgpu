// Reports how DXGI orders adapters for this exe (UserGpuPreferences test) and where default devices land.
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

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

static void print_adapter(const char* tag, UINT i, IDXGIAdapter1* a) {
  DXGI_ADAPTER_DESC1 d;
  a->GetDesc1(&d);
  ComPtr<IDXGIOutput> o;
  UINT outputs = 0;
  while (a->EnumOutputs(outputs, &o) != DXGI_ERROR_NOT_FOUND) { ++outputs; o.Reset(); }
  ComPtr<ID3D12Device> dev;
  bool d12 = SUCCEEDED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)));
  printf("%s[%u] bus=%d luid=%08lx:%08lx ven=%04x dev=%04x subsys=%08x rev=%x outputs=%u sw=%d d3d12=%d %ls\n", tag, i,
         pci_bus(d.AdapterLuid), d.AdapterLuid.HighPart, d.AdapterLuid.LowPart, d.VendorId, d.DeviceId, d.SubSysId, d.Revision,
         outputs, (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0, d12, d.Description);
}

int main(int argc, char** argv) {
  int present_ms = argc > 1 ? atoi(argv[1]) : 0;
  ComPtr<IDXGIFactory6> f;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) return 1;
  ComPtr<IDXGIAdapter1> a;
  for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset()) print_adapter("EnumAdapters1", i, a.Get());
  const struct { const char* n; DXGI_GPU_PREFERENCE p; } prefs[] = {
      {"ByPref.UNSPECIFIED", DXGI_GPU_PREFERENCE_UNSPECIFIED},
      {"ByPref.MIN_POWER", DXGI_GPU_PREFERENCE_MINIMUM_POWER},
      {"ByPref.HIGH_PERF", DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE}};
  for (auto& p : prefs)
    for (UINT i = 0; f->EnumAdapterByGpuPreference(i, p.p, IID_PPV_ARGS(&a)) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset()) print_adapter(p.n, i, a.Get());

  ComPtr<ID3D12Device> d12;
  if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d12)))) {
    LUID l = d12->GetAdapterLuid();
    printf("D3D12CreateDevice(nullptr) bus=%d luid=%08lx:%08lx\n", pci_bus(l), l.HighPart, l.LowPart);
  }
  ComPtr<ID3D11Device> d11;
  if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d11, nullptr, nullptr))) {
    ComPtr<IDXGIDevice> dx;
    ComPtr<IDXGIAdapter> ad;
    d11.As(&dx);
    dx->GetAdapter(&ad);
    DXGI_ADAPTER_DESC d;
    ad->GetDesc(&d);
    printf("D3D11CreateDevice(nullptr) bus=%d luid=%08lx:%08lx\n", pci_bus(d.AdapterLuid), d.AdapterLuid.HighPart, d.AdapterLuid.LowPart);
  }
  if (present_ms <= 0 || !d12) return 0;

  // Optional: present from the default D3D12 device for present_ms, so PresentMon can classify it.
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"gpu_pref_probe";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"gpu_pref_probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 360, nullptr, nullptr, wc.hInstance, nullptr);
  D3D12_COMMAND_QUEUE_DESC qd{};
  ComPtr<ID3D12CommandQueue> q;
  d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = 640; sd.Height = 360; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> sc;
  HRESULT hr = f->CreateSwapChainForHwnd(q.Get(), hwnd, &sd, nullptr, nullptr, &sc);
  printf("CreateSwapChainForHwnd hr=%08lx\n", hr);
  if (FAILED(hr)) return 2;
  ULONGLONG end = GetTickCount64() + present_ms;
  UINT frames = 0;
  while (GetTickCount64() < end) {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    sc->Present(1, 0);
    ++frames;
  }
  printf("presented %u frames\n", frames);
  return 0;
}
