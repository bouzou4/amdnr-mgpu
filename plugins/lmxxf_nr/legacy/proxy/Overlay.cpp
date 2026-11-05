// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../../LICENSE.
// Legacy mode, game side: menu and toasts. Dear ImGui (MIT, vendored by OptiScaler under src/OptiScaler/include/imgui)
// with its DX12 backend; input comes from the proxy's window subclass, not the Win32 backend (the game owns the
// cursor: raw deltas drive a drawn one).
#include "Overlay.h"

#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>


namespace Overlay
{
namespace
{
std::mutex g_mutex; // ImGui context: input events (window thread) and frames (present thread)
ImGuiContext* g_ctx = nullptr;
ID3D12DescriptorHeap* g_srvHeap = nullptr;
uint32_t g_srvStride = 0, g_srvUsed = 0;
constexpr uint32_t kSrvCount = 64;
bool g_menu = false;
std::string g_toast;
ULONGLONG g_toastEnd = 0;
LARGE_INTEGER g_freq {}, g_last {};
ImVec2 g_mouse { -1, -1 };
ULONGLONG g_rawTick = 0; // last raw mouse delta: client positions are ignored for a while
uint32_t g_dispW = 1, g_dispH = 1;
DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;

void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    const uint32_t i = std::min(g_srvUsed++, kSrvCount - 1); // a handful of textures: no free list
    cpu->ptr = g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * g_srvStride;
    gpu->ptr = g_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(i) * g_srvStride;
}
void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {}

ImGuiKey KeyOf(WPARAM vk)
{
    if (vk >= '0' && vk <= '9')
        return ImGuiKey(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z')
        return ImGuiKey(ImGuiKey_A + (vk - 'A'));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return ImGuiKey(ImGuiKey_Keypad0 + (vk - VK_NUMPAD0));
    switch (vk)
    {
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_OEM_PERIOD: return ImGuiKey_Period;
    case VK_OEM_MINUS: return ImGuiKey_Minus;
    case VK_DECIMAL: return ImGuiKey_KeypadDecimal;
    case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
    case VK_SHIFT: return ImGuiKey_LeftShift;
    case VK_CONTROL: return ImGuiKey_LeftCtrl;
    case VK_MENU: return ImGuiKey_LeftAlt;
    default: return ImGuiKey_None;
    }
}

void Mods(ImGuiIO& io)
{
    io.AddKeyEvent(ImGuiMod_Ctrl, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (GetKeyState(VK_MENU) & 0x8000) != 0);
}

void MoveTo(ImGuiIO& io, float x, float y)
{
    g_mouse.x = std::clamp(x, 0.0f, float(g_dispW - 1));
    g_mouse.y = std::clamp(y, 0.0f, float(g_dispH - 1));
    io.AddMousePosEvent(g_mouse.x, g_mouse.y);
}

// Combo whose disabled items are greyed out
bool Combo(const char* label, int* v, const char* const* items, int n, const bool* off = nullptr)
{
    bool changed = false;
    if (ImGui::BeginCombo(label, items[std::clamp(*v, 0, n - 1)]))
    {
        for (int i = 0; i < n; ++i)
        {
            const bool dis = off && off[i];
            if (ImGui::Selectable(items[i], i == *v, dis ? ImGuiSelectableFlags_Disabled : 0))
                *v = i, changed = true;
            if (i == *v)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void Tip(const char* t)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", t);
}

// src/Presets.h rows (that header needs the x64 runtime ABI): model, size, passes, history
struct Preset
{
    const char* name;
    int model, size, passes, temporal;
    const char* cost;
};
const Preset kPresets[] = {
    { "Performance", 1, 0, 1, 1, "about 8 ms of network at 4K" },
    { "Balanced", 0, 0, 1, 1, "about 18 ms" },
    { "Quality", 0, 1, 1, 1, "about 25 ms" },
    { "Ultra", 0, 2, 1, 1, "about 55 ms (estimated)" },
};
constexpr int kPresetCount = 4;

const char* const kSizes[] = { "1080 tier", "1440 (2560x1440)", "Native" };
const char* const kModels[] = { "Full (71 blocks)", "Small (31 blocks)" };
const char* const kPasses[] = { "1", "2 (about 2x the network)", "3 (2 run + 1 predicted)" };
const char* const kStyles[] = { "Auto (0 here)", "0 (NVIDIA look)", "1", "2" };
const char* const kFlows[] = { "None (history off)", "Zero vectors (static history)", "Optical flow" };

void Menu()
{
    Settings s = GetSettings();
    const Readouts r = GetReadouts();
    Legacy::Control& c = s.c;
    bool changed = false;
    const bool freeRes = r.caps & Legacy::kCapFreeRes, models = r.caps & Legacy::kCapModels,
               styles = r.caps & Legacy::kCapStyles, known = r.hostUp;

    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
    ImGui::Begin("lmxxf legacy NR", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::TextDisabled("Insert / Esc close   End NR   PgUp size   PgDn history   Home optical flow");
    ImGui::Separator();

    changed |= ImGui::Checkbox("Neural rendering (End)", &s.nr);
    Tip("Off: frames go straight to the screen (the host idles)");

    // Presets set model, size, passes and history only
    const char* presetNames[kPresetCount + 1];
    bool presetOff[kPresetCount + 1] = {};
    int presetIdx = kPresetCount;
    std::string tip;
    for (int i = 0; i < kPresetCount; ++i)
    {
        const Preset& p = kPresets[i];
        presetNames[i] = p.name;
        presetOff[i] = known && ((p.size != 0 && !freeRes) || (p.model == 1 && !models));
        if (c.model == p.model && c.size == p.size && c.passes == p.passes && (c.temporal != 0) == (p.temporal != 0))
            presetIdx = i;
        tip += std::string(p.name) + ": " + (p.model == 1 ? "Small" : "Full") + ", " + kSizes[p.size] +
               ". One GPU: " + p.cost + ".\n";
    }
    presetNames[kPresetCount] = "Custom";
    if (Combo("Preset", &presetIdx, presetNames, kPresetCount + 1, presetOff) && presetIdx < kPresetCount)
    {
        const Preset& p = kPresets[presetIdx];
        c.model = p.model, c.size = p.size, c.passes = p.passes, c.temporal = p.temporal;
        changed = true;
    }
    Tip(tip.c_str());

    const bool sizeOff[] = { false, known && !freeRes, known && !freeRes };
    changed |= Combo("Network size (PgUp)", &c.size, kSizes, 3, sizeOff);
    Tip(freeRes || !known ? "1080 tier: the picture fitted into 1920x1080. 1440 / Native: free network size"
                          : "This runtime has fixed tiers only (no DLSS5_NETWORK_FREE_RES)");
    ImGui::BeginDisabled(known && !models);
    changed |= Combo("Model", &c.model, kModels, 2);
    int pass = c.passes - 1;
    if (Combo("Passes", &pass, kPasses, 3))
        c.passes = pass + 1, changed = true;
    ImGui::EndDisabled();
    Tip("The network runs N times per frame for a stronger style (DLSS5_MULTI_PASS). 3 runs without history");
    ImGui::BeginDisabled(known && !styles);
    int style = c.style + 1;
    if (Combo("Style", &style, kStyles, 4))
        c.style = style - 1, changed = true;
    ImGui::EndDisabled();
    changed |= ImGui::SliderFloat("Strength", &c.strength, 0.0f, 2.0f, "%.2f");
    changed |= ImGui::SliderFloat("Colour", &c.colorStrength, -1.0f, 1.0f, c.colorStrength < 0 ? "auto" : "%.2f");
    Tip("Share of the network's colour change: 0 its luma on the game's chroma, 1 its colour as it is (default)");
    bool guided = c.upsample == 1, stab = c.stabilise != 0, hist = c.temporal != 0;
    if (ImGui::Checkbox("Guided edit upsample", &guided))
        c.upsample = guided ? 1 : 0, changed = true;
    Tip("1080 / 1440 sizes: the edit reaches the output guided by its full-size colour. Off: bilinear");
    ImGui::BeginDisabled(!guided);
    if (ImGui::Checkbox("Stabilise the edit", &stab))
        c.stabilise = stab ? 1 : 0, changed = true;
    ImGui::EndDisabled();
    Tip("Guided upsample: the edit accumulated over frames at the output size, following the motion");
    if (ImGui::Checkbox("Network history (PgDn)", &hist))
        c.temporal = hist ? 1 : 0, changed = true;
    changed |= Combo("Motion (Home)", &c.flow, kFlows, 3);
    Tip("What the network history and the stabiliser follow: our colour-only optical flow, or no motion");
    if (changed)
        SetSettings(s);
    ImGui::TextDisabled("Size, model, passes and style rebuild the network (a few seconds without NR).");

    ImGui::Separator();
    ImGui::Text("%.1f fps (%.2f ms)   %ux%u", r.fps, r.frameMs, r.outW, r.outH);
    if (!r.on12)
        ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "D3D9On12 is off: no NR");
    else if (!r.hostUp)
        ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "NR host not running");
    else
    {
        ImGui::Text("Host: %s%s", r.status, r.applied ? "" : "   (applying settings)");
        ImGui::Text("Network %ux%u   NR %.2f ms (network %.2f)   optical flow %.2f ms", r.feedW, r.feedH, r.hostMs,
                    r.netMs, r.ofMs);
        ImGui::Text("Wait for the host %.2f ms/frame", r.proxyWaitMs);
    }
    ImGui::Text("Frames: %llu with NR, %llu without (rebuild / failure), %llu bypassed", r.nrFrames, r.passFrames,
                r.bypassed);
    ImGui::Text("Stalls: %llu host timeouts, %llu frames skipped, %llu failed presents", r.timeouts, r.skipped,
                r.failedPresents);
    if (r.flkStatic == r.flkStatic && r.flkMc == r.flkMc)
        ImGui::Text("Flicker (answer minus input): still %.5f, moving %.5f", r.flkStatic, r.flkMc);
    if (r.error[0])
        ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", r.error);
    ImGui::End();
}
} // namespace

bool Init(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT format, uint32_t height)
{
    std::lock_guard lock(g_mutex);
    if (g_ctx)
        return true;
    D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvCount,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvHeap))))
        return false;
    g_srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_srvUsed = 0;
    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendPlatformName = "lmxxf_legacy";
    ImGui::StyleColorsDark();
    const float scale = std::max(1.0f, float(height) / 1080.0f);
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6, st.FrameRounding = 3;
    st.Colors[ImGuiCol_WindowBg].w = 0.92f;
    st.ScaleAllSizes(scale);
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    char font[MAX_PATH * 2];
    snprintf(font, sizeof(font), "%ls\\Fonts\\segoeui.ttf", win);
    if (GetFileAttributesA(font) == INVALID_FILE_ATTRIBUTES || !io.Fonts->AddFontFromFileTTF(font, 17.0f * scale))
    {
        ImFontConfig fc;
        fc.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault(&fc);
    }
    ImGui_ImplDX12_InitInfo ii;
    ii.Device = device;
    ii.CommandQueue = queue;
    ii.NumFramesInFlight = 3;
    ii.RTVFormat = format;
    ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ii.SrvDescriptorHeap = g_srvHeap;
    ii.SrvDescriptorAllocFn = SrvAlloc;
    ii.SrvDescriptorFreeFn = SrvFree;
    if (!ImGui_ImplDX12_Init(&ii))
    {
        ImGui::DestroyContext(g_ctx);
        g_ctx = nullptr;
        g_srvHeap->Release(), g_srvHeap = nullptr;
        return false;
    }
    g_format = format;
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_last);
    return true;
}

void Shutdown()
{
    std::lock_guard lock(g_mutex);
    if (!g_ctx)
        return;
    ImGui_ImplDX12_Shutdown();
    ImGui::DestroyContext(g_ctx);
    g_ctx = nullptr;
    if (g_srvHeap)
        g_srvHeap->Release(), g_srvHeap = nullptr;
}

void Toast(const char* fmt, ...)
{
    char buf[256];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    std::lock_guard lock(g_mutex);
    g_toast = buf;
    g_toastEnd = GetTickCount64() + 2000;
}

bool MenuOpen() { return g_menu; }

void SetMenu(bool open)
{
    std::lock_guard lock(g_mutex);
    if (open && !g_menu && g_ctx)
    {
        ImGuiIO& io = ImGui::GetIO();
        MoveTo(io, g_dispW * 0.5f, g_dispH * 0.5f);
        for (int b = 0; b < 3; ++b)
            io.AddMouseButtonEvent(b, false);
    }
    g_menu = open;
}

bool Visible()
{
    std::lock_guard lock(g_mutex);
    return g_menu || GetTickCount64() < g_toastEnd;
}

void Draw(ID3D12GraphicsCommandList* list, ID3D12Resource*, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h)
{
    std::lock_guard lock(g_mutex);
    if (!g_ctx)
        return;
    ImGuiIO& io = ImGui::GetIO();
    g_dispW = w, g_dispH = h;
    io.DisplaySize = ImVec2(float(w), float(h));
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    io.DeltaTime = std::clamp(float(double(now.QuadPart - g_last.QuadPart) / double(g_freq.QuadPart)), 1e-4f, 0.1f);
    g_last = now;
    io.MouseDrawCursor = g_menu;
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    if (g_menu)
        Menu(); // takes the proxy's settings lock inside ours: the proxy never calls in here holding it
    if (GetTickCount64() < g_toastEnd && !g_toast.empty())
    {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.08f), ImGuiCond_Always,
                                ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.75f);
        ImGui::Begin("##toast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted(g_toast.c_str());
        ImGui::End();
    }
    ImGui::Render();
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->SetDescriptorHeaps(1, &g_srvHeap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
}

bool OnMessage(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    std::lock_guard lock(g_mutex);
    if (!g_ctx || !g_menu)
        return false;
    ImGuiIO& io = ImGui::GetIO();
    switch (msg)
    {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    {
        const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        Mods(io);
        if (const ImGuiKey k = KeyOf(wp); k != ImGuiKey_None)
            io.AddKeyEvent(k, down);
        return down; // releases go on: a key held when the menu opened must not stick in the game
    }
    case WM_CHAR:
    case WM_SYSCHAR:
        if (wp > 0 && wp < 0x10000 && wp != 27)
            io.AddInputCharacterUTF16((unsigned short) wp);
        return true;
    case WM_MOUSEMOVE:
        if (GetTickCount64() - g_rawTick > 250)
        {
            RECT rc;
            GetClientRect(wnd, &rc);
            const float sx = rc.right > 0 ? float(g_dispW) / float(rc.right) : 1.0f;
            const float sy = rc.bottom > 0 ? float(g_dispH) / float(rc.bottom) : 1.0f;
            MoveTo(io, float((short) LOWORD(lp)) * sx, float((short) HIWORD(lp)) * sy);
        }
        return true;
    case WM_INPUT:
    {
        RAWINPUT ri;
        UINT size = sizeof(ri);
        if (GetRawInputData((HRAWINPUT) lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            ri.header.dwType == RIM_TYPEMOUSE)
        {
            const RAWMOUSE& m = ri.data.mouse;
            if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE) && (m.lLastX || m.lLastY))
            {
                g_rawTick = GetTickCount64();
                MoveTo(io, g_mouse.x + float(m.lLastX), g_mouse.y + float(m.lLastY));
            }
            const USHORT f = m.usButtonFlags;
            if (f & RI_MOUSE_LEFT_BUTTON_DOWN) io.AddMouseButtonEvent(0, true);
            if (f & RI_MOUSE_LEFT_BUTTON_UP) io.AddMouseButtonEvent(0, false);
            if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) io.AddMouseButtonEvent(1, true);
            if (f & RI_MOUSE_RIGHT_BUTTON_UP) io.AddMouseButtonEvent(1, false);
            if (f & RI_MOUSE_WHEEL) io.AddMouseWheelEvent(0, float((SHORT) m.usButtonData) / WHEEL_DELTA);
        }
        return true;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: io.AddMouseButtonEvent(0, true); return true;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK: io.AddMouseButtonEvent(1, true); return true;
    case WM_MBUTTONDOWN: io.AddMouseButtonEvent(2, true); return true;
    case WM_LBUTTONUP: io.AddMouseButtonEvent(0, false); return false;
    case WM_RBUTTONUP: io.AddMouseButtonEvent(1, false); return false;
    case WM_MBUTTONUP: io.AddMouseButtonEvent(2, false); return false;
    case WM_MOUSEWHEEL: io.AddMouseWheelEvent(0, float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA); return true;
    default: return false;
    }
}
} // namespace Overlay
