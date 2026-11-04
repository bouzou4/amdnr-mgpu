// PCI location per LUID (D3DKMT) and PCIe link state per devnode chain (SetupAPI/CfgMgr).
#include "diag.h"

#include <winternl.h>
#include <d3dkmthk.h>
#include <initguid.h>
#include <devguid.h>
#include <devpkey.h>
#include <pciprop.h>
#include <setupapi.h>
#include <cfgmgr32.h>

PciAddr pci_addr_for_luid(LUID luid, uint32_t* kmt_type) {
  PciAddr r;
  *kmt_type = 0;
  D3DKMT_OPENADAPTERFROMLUID open{};
  open.AdapterLuid = luid;
  if (D3DKMTOpenAdapterFromLuid(&open) != 0) return r;
  D3DKMT_ADAPTERADDRESS addr{};
  D3DKMT_QUERYADAPTERINFO q{};
  q.hAdapter = open.hAdapter;
  q.Type = KMTQAITYPE_ADAPTERADDRESS;
  q.pPrivateDriverData = &addr;
  q.PrivateDriverDataSize = sizeof addr;
  if (D3DKMTQueryAdapterInfo(&q) == 0 && addr.BusNumber != 0xFFFFFFFF)
    r = {int(addr.BusNumber), int(addr.DeviceNumber), int(addr.FunctionNumber)};
  D3DKMT_ADAPTERTYPE t{};
  q.Type = KMTQAITYPE_ADAPTERTYPE;
  q.pPrivateDriverData = &t;
  q.PrivateDriverDataSize = sizeof t;
  if (D3DKMTQueryAdapterInfo(&q) == 0) *kmt_type = t.Value;
  D3DKMT_CLOSEADAPTER close{open.hAdapter};
  D3DKMTCloseAdapter(&close);
  return r;
}

static bool prop_u32(DEVINST dn, const DEVPROPKEY& k, uint32_t& out) {
  DEVPROPTYPE t;
  ULONG sz = sizeof out;
  return CM_Get_DevNode_PropertyW(dn, &k, &t, (PBYTE)&out, &sz, 0) == CR_SUCCESS && t == DEVPROP_TYPE_UINT32;
}

static std::string prop_str(DEVINST dn, const DEVPROPKEY& k) {
  DEVPROPTYPE t;
  wchar_t buf[512];
  ULONG sz = sizeof buf;
  if (CM_Get_DevNode_PropertyW(dn, &k, &t, (PBYTE)buf, &sz, 0) != CR_SUCCESS || t != DEVPROP_TYPE_STRING) return {};
  return narrow(buf);
}

static double gts(uint32_t code) {
  static const double t[] = {0, 2.5, 5, 8, 16, 32, 64};
  return code < 7 ? t[code] : 0;
}

static DEVINST find_display_devnode(const PciAddr& a) {
  HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
  if (set == INVALID_HANDLE_VALUE) return 0;
  DEVINST found = 0;
  SP_DEVINFO_DATA d{sizeof d};
  for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
    DWORD bus = 0, addr = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_BUSNUMBER, nullptr, (PBYTE)&bus, sizeof bus, nullptr)) continue;
    if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_ADDRESS, nullptr, (PBYTE)&addr, sizeof addr, nullptr)) continue;
    if (int(bus) == a.bus && int(addr >> 16) == a.dev && int(addr & 0xFFFF) == a.fn) { found = d.DevInst; break; }
  }
  SetupDiDestroyDeviceInfoList(set);
  return found;
}

// GPU endpoint first, then each upstream bridge until the root port. Windows only fills the
// link keys on the endpoint, which on R9700 boards is the GPU-to-onboard-switch link.
Json pcie_links_for(const PciAddr& a) {
  Json arr = Json::arr();
  if (a.bus < 0) return arr;
  DEVINST dn = find_display_devnode(a);
  for (int depth = 0; dn && depth < 8; ++depth) {
    uint32_t cs = 0, cw = 0, ms = 0, mw = 0;
    bool has = prop_u32(dn, DEVPKEY_PciDevice_CurrentLinkSpeed, cs);
    prop_u32(dn, DEVPKEY_PciDevice_CurrentLinkWidth, cw);
    prop_u32(dn, DEVPKEY_PciDevice_MaxLinkSpeed, ms);
    prop_u32(dn, DEVPKEY_PciDevice_MaxLinkWidth, mw);
    std::string loc = prop_str(dn, DEVPKEY_Device_LocationInfo), name = prop_str(dn, DEVPKEY_Device_DeviceDesc);
    if (loc.rfind("PCI bus", 0) != 0) break;
    Json j = Json::obj();
    j["role"] = depth == 0 ? "gpu" : ("upstream" + std::to_string(depth));
    j["name"] = name;
    j["location"] = loc;
    if (has) {
      j["current_gts"] = gts(cs);
      j["current_width"] = cw;
      j["max_gts"] = gts(ms);
      j["max_width"] = mw;
    }
    arr.push(j);
    if (name.find("Root Port") != std::string::npos) break;
    DEVINST parent = 0;
    if (CM_Get_Parent(&parent, dn, 0) != CR_SUCCESS) break;
    dn = parent;
  }
  return arr;
}

// ADL (MIT SDK, GPUOpen display-library) structures and PMLOG sensor ids.
namespace {
struct AdlAdapterInfo {
  int iSize, iAdapterIndex;
  char strUDID[256];
  int iBusNumber, iDeviceNumber, iFunctionNumber, iVendorID;
  char strAdapterName[256], strDisplayName[256];
  int iPresent, iExist;
  char strDriverPath[256], strDriverPathExt[256], strPNPString[256];
  int iOSDisplayIndex;
};
struct AdlSensor { int supported, value; };
struct AdlPmLog { int size; AdlSensor sensors[256]; };
constexpr int PMLOG_BUS_SPEED = 40, PMLOG_BUS_LANES = 41, PMLOG_BUS_CURR_MAX_SPEED = 58;
void* __stdcall adl_alloc(int n) { return malloc(n); }
}  // namespace

Json adl_link_for_bus(int bus) {
  static HMODULE m = LoadLibraryExW(L"atiadlxx.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  static void* ctx = nullptr;
  using Create = int(__cdecl*)(void*(__stdcall*)(int), int, void**);
  using Num = int(__cdecl*)(void*, int*);
  using Info = int(__cdecl*)(void*, AdlAdapterInfo*, int);
  using Pm = int(__cdecl*)(void*, int, AdlPmLog*);
  Json j;
  if (!m) return j;
  auto create = (Create)GetProcAddress(m, "ADL2_Main_Control_Create");
  auto num = (Num)GetProcAddress(m, "ADL2_Adapter_NumberOfAdapters_Get");
  auto info = (Info)GetProcAddress(m, "ADL2_Adapter_AdapterInfo_Get");
  auto pm = (Pm)GetProcAddress(m, "ADL2_New_QueryPMLogData_Get");
  if (!create || !num || !info || !pm) return j;
  if (!ctx && create(adl_alloc, 1, &ctx) != 0) return j;
  int n = 0;
  if (num(ctx, &n) != 0 || n <= 0) return j;
  std::vector<AdlAdapterInfo> ai(n);
  for (auto& a : ai) a.iSize = sizeof a;
  if (info(ctx, ai.data(), int(ai.size() * sizeof(AdlAdapterInfo))) != 0) return j;
  for (auto& a : ai) {
    if (a.iBusNumber != bus) continue;
    AdlPmLog log{};
    log.size = sizeof log;
    if (pm(ctx, a.iAdapterIndex, &log) != 0) continue;
    auto& sp = log.sensors[PMLOG_BUS_SPEED];
    auto& ln = log.sensors[PMLOG_BUS_LANES];
    auto& mx = log.sensors[PMLOG_BUS_CURR_MAX_SPEED];
    if (!sp.supported && !ln.supported) continue;
    j = Json::obj();
    j["source"] = "ADL PMLOG_BUS_SPEED/LANES/CURR_MAX_SPEED";
    j["bus_speed_raw"] = sp.value;
    j["bus_lanes"] = ln.value;
    j["bus_curr_max_speed_raw"] = mx.value;
    // Encoding inferred on this rig: BUS_SPEED is a 0-based gen index, CURR_MAX_SPEED 1-based; matches measured GB/s.
    j["pcie_gen"] = mx.supported ? mx.value : sp.value + 1;
    return j;
  }
  return j;
}
