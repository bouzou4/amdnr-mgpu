#include "pch.h"

#include "PluginApi.h"

#include "Config.h"
#include "menu/menu_common.h"
#include "hooks/D3D12_Hooks.h"
#include "hooks/DxgiFactory_Hooks.h"
#include "proxies/D3D12_Proxy.h"
#include "proxies/Dxgi_Proxy.h"
#include "upscalers/IFeature.h"

#include <imgui/imgui_internal.h>

#include <charconv>

struct OptiPlugin
{
    enum class InitState
    {
        Running,
        Accepted,
        Declined,
    };

    std::string name;
    std::string section; // Plugin.<name> in OptiScaler.ini
    InitState initState = InitState::Running;
};

namespace
{
struct PluginTab
{
    OptiPlugin* plugin;
    std::string name;
    std::string description;
    OptiDrawTabFn draw;
    void* userData;
    bool faulted = false;
};

// Plugins live as long as the process, the API keeps pointers to them
std::vector<std::unique_ptr<OptiPlugin>> _plugins;
std::vector<HMODULE> _initedModules;
std::vector<std::shared_ptr<PluginTab>> _tabs;
std::vector<std::shared_ptr<PluginTab>> _pendingTabs;
std::mutex _mutex;

// State of the draw callback that is running, widgets do nothing without it
struct TabDrawState
{
    std::optional<ScopedCard> card;
    bool disabled = false;
    ImVec4 (*colorMap)(const ImVec4&) = nullptr;
};

thread_local TabDrawState* _draw = nullptr;

// Plugin code is called through these, a crash in it disables the plugin instead of the game
bool CallDraw(OptiDrawTabFn draw, void* userData)
{
    __try
    {
        draw(userData);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool CallInit(PFN_OptiPluginInit init, const OptiPluginApi* api, OptiPlugin* plugin, bool* result)
{
    __try
    {
        *result = init(api, plugin);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void ShowItemTooltip(const char* text)
{
    if (text == nullptr || !ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        return;

    ImGui::BeginTooltip();
    ImGui::TextUnformatted(text);
    ImGui::EndTooltip();
}

void WrappedText(const char* text, const ImVec4* color)
{
    if (color != nullptr)
        ImGui::PushStyleColor(ImGuiCol_Text, _draw->colorMap != nullptr ? _draw->colorMap(*color) : *color);

    ImGui::TextWrapped("%s", text);

    if (color != nullptr)
        ImGui::PopStyleColor();
}

// Copies at most bufferSize - 1 bytes without cutting a UTF-8 character in half
void CopyUtf8(const std::string& value, char* buffer, size_t bufferSize)
{
    if (buffer == nullptr || bufferSize == 0)
        return;

    size_t length = std::min(value.size(), bufferSize - 1);

    // Continuation bytes are 10xxxxxx
    if (length < value.size())
    {
        while (length > 0 && (static_cast<unsigned char>(value[length]) & 0xC0) == 0x80)
            length--;
    }

    memcpy(buffer, value.data(), length);
    buffer[length] = '\0';
}

// "auto" is treated as a missing value, like for OptiScaler options
std::optional<std::string> ReadValue(OptiPlugin* plugin, const char* key)
{
    if (plugin == nullptr || key == nullptr)
        return std::nullopt;

    auto value = Config::Instance()->GetRawValue(plugin->section.c_str(), key);
    if (!value.has_value())
        return std::nullopt;

    std::string lower = *value;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return (char) std::tolower(c); });

    if (lower == "auto")
        return std::nullopt;

    return value;
}

template <typename T> std::optional<T> ParseNumber(const std::string& text)
{
    T value {};
    const char* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(text.data(), end, value);

    if (ec != std::errc() || ptr != end)
        return std::nullopt;

    return value;
}

void WriteValue(OptiPlugin* plugin, const char* key, const std::string& value)
{
    if (plugin == nullptr || key == nullptr)
        return;

    Config::Instance()->SetRawValue(plugin->section.c_str(), key, value.c_str());
}

// API functions -----------------------------

bool Api_RegisterTab(OptiPlugin* plugin, const char* name, const char* description, OptiDrawTabFn draw, void* userData)
{
    if (plugin == nullptr || name == nullptr || name[0] == '\0' || draw == nullptr)
        return false;

    auto tab = std::make_shared<PluginTab>();
    tab->plugin = plugin;
    tab->name = name;
    tab->description = description != nullptr ? description : "";
    tab->draw = draw;
    tab->userData = userData;

    std::lock_guard<std::mutex> lock(_mutex);

    switch (plugin->initState)
    {
    case OptiPlugin::InitState::Running:
        _pendingTabs.push_back(std::move(tab));
        break;
    case OptiPlugin::InitState::Accepted:
        _tabs.push_back(std::move(tab));
        break;
    default:
        return false;
    }

    LOG_INFO("[{}] Registered tab: {}", plugin->name, name);

    return true;
}

void Api_Section(const char* title)
{
    if (_draw == nullptr)
        return;

    // Card border is drawn when the card closes, it shouldn't take the alpha of disabled widgets
    if (_draw->disabled)
        ImGui::EndDisabled();

    _draw->card.reset();

    if (_draw->disabled)
        ImGui::BeginDisabled();

    _draw->card.emplace(title);
}

void Api_Separator()
{
    if (_draw != nullptr)
        ImGui::Separator();
}

void Api_SameLine()
{
    if (_draw != nullptr)
        ImGui::SameLine();
}

void Api_Spacing()
{
    if (_draw != nullptr)
        ImGui::Spacing();
}

void Api_Disabled(bool disabled)
{
    if (_draw == nullptr || _draw->disabled == disabled)
        return;

    if (disabled)
        ImGui::BeginDisabled();
    else
        ImGui::EndDisabled();

    _draw->disabled = disabled;
}

void Api_ItemTooltip(const char* text)
{
    if (_draw != nullptr)
        ShowItemTooltip(text);
}

void Api_Text(const char* text)
{
    if (_draw != nullptr && text != nullptr)
        WrappedText(text, nullptr);
}

void Api_TextDisabled(const char* text)
{
    if (_draw == nullptr || text == nullptr)
        return;

    // Disabled color isn't tone mapped by the menu either
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void Api_TextColored(float r, float g, float b, float a, const char* text)
{
    if (_draw == nullptr || text == nullptr)
        return;

    const ImVec4 color(r, g, b, a);
    WrappedText(text, &color);
}

bool Api_Button(const char* label) { return _draw != nullptr && label != nullptr && ImGui::Button(label); }

bool Api_Checkbox(const char* label, bool* value)
{
    return _draw != nullptr && label != nullptr && value != nullptr && ImGui::Checkbox(label, value);
}

bool Api_SliderFloat(const char* label, float* value, float min, float max, const char* format)
{
    if (_draw == nullptr || label == nullptr || value == nullptr)
        return false;

    return ImGui::SliderFloat(label, value, min, max, format != nullptr ? format : "%.3f",
                              ImGuiSliderFlags_AlwaysClamp);
}

bool Api_SliderInt(const char* label, int32_t* value, int32_t min, int32_t max, const char* format)
{
    if (_draw == nullptr || label == nullptr || value == nullptr)
        return false;

    return ImGui::SliderInt(label, value, min, max, format != nullptr ? format : "%d", ImGuiSliderFlags_AlwaysClamp);
}

bool Api_ItemDeactivatedAfterEdit() { return _draw != nullptr && ImGui::IsItemDeactivatedAfterEdit(); }

bool Api_Combo(const char* label, int32_t* current, const char* const* items, int32_t itemCount,
               const bool* itemDisabled)
{
    if (_draw == nullptr || label == nullptr || current == nullptr || (items == nullptr && itemCount > 0))
        return false;

    const bool validIndex = *current >= 0 && *current < itemCount && items[*current] != nullptr;
    const char* preview = validIndex ? items[*current] : "";
    bool changed = false;

    if (ImGui::BeginCombo(label, preview))
    {
        for (int32_t i = 0; i < itemCount; i++)
        {
            ImGui::PushID(i);

            ImGui::BeginDisabled(itemDisabled != nullptr && itemDisabled[i]);

            const bool selected = *current == i;
            if (ImGui::Selectable(items[i] != nullptr ? items[i] : "", selected) && !selected)
            {
                *current = i;
                changed = true;
            }

            if (selected)
                ImGui::SetItemDefaultFocus();

            ImGui::EndDisabled();
            ImGui::PopID();
        }

        ImGui::EndCombo();
    }

    return changed;
}

bool Api_InputText(const char* label, char* buffer, size_t bufferSize)
{
    if (_draw == nullptr || label == nullptr || buffer == nullptr || bufferSize == 0)
        return false;

    return ImGui::InputText(label, buffer, bufferSize);
}

bool Api_ColorEdit(const char* label, float* rgba, bool withAlpha)
{
    if (_draw == nullptr || label == nullptr || rgba == nullptr)
        return false;

    return withAlpha ? ImGui::ColorEdit4(label, rgba) : ImGui::ColorEdit3(label, rgba);
}

bool Api_IsMenuOpen() { return MenuCommon::IsVisible(); }

bool Api_GetBool(OptiPlugin* plugin, const char* key, bool defaultValue)
{
    auto value = ReadValue(plugin, key);
    if (!value.has_value())
        return defaultValue;

    std::string lower = *value;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return (char) std::tolower(c); });

    if (lower == "true" || lower == "1")
        return true;

    if (lower == "false" || lower == "0")
        return false;

    return defaultValue;
}

int32_t Api_GetInt(OptiPlugin* plugin, const char* key, int32_t defaultValue)
{
    auto value = ReadValue(plugin, key);
    if (!value.has_value())
        return defaultValue;

    return ParseNumber<int32_t>(*value).value_or(defaultValue);
}

float Api_GetFloat(OptiPlugin* plugin, const char* key, float defaultValue)
{
    auto value = ReadValue(plugin, key);
    if (!value.has_value())
        return defaultValue;

    return ParseNumber<float>(*value).value_or(defaultValue);
}

bool Api_GetString(OptiPlugin* plugin, const char* key, char* buffer, size_t bufferSize, const char* defaultValue)
{
    auto value = ReadValue(plugin, key);

    if (!value.has_value())
    {
        CopyUtf8(defaultValue != nullptr ? defaultValue : "", buffer, bufferSize);
        return false;
    }

    CopyUtf8(*value, buffer, bufferSize);
    return true;
}

void Api_SetBool(OptiPlugin* plugin, const char* key, bool value) { WriteValue(plugin, key, value ? "true" : "false"); }

void Api_SetInt(OptiPlugin* plugin, const char* key, int32_t value) { WriteValue(plugin, key, std::to_string(value)); }

// Shortest text that reads back as the same float
void Api_SetFloat(OptiPlugin* plugin, const char* key, float value)
{
    WriteValue(plugin, key, std::format("{}", value));
}

void Api_SetString(OptiPlugin* plugin, const char* key, const char* value)
{
    WriteValue(plugin, key, value != nullptr ? value : "");
}

void Api_ResetValue(OptiPlugin* plugin, const char* key)
{
    if (plugin != nullptr && key != nullptr)
        Config::Instance()->DeleteRawValue(plugin->section.c_str(), key);
}

bool Api_SaveSettings(OptiPlugin* plugin)
{
    if (plugin == nullptr)
        return false;

    return Config::Instance()->SaveRawValues();
}

void Api_Log(OptiPlugin* plugin, OptiLogLevel level, const char* message)
{
    if (plugin == nullptr || message == nullptr)
        return;

    switch (level)
    {
    case OptiLog_Debug:
        LOG_DEBUG("[{}] {}", plugin->name, message);
        break;
    case OptiLog_Warn:
        LOG_WARN("[{}] {}", plugin->name, message);
        break;
    case OptiLog_Error:
        LOG_ERROR("[{}] {}", plugin->name, message);
        break;
    default:
        LOG_INFO("[{}] {}", plugin->name, message);
        break;
    }
}

struct UpscaleCallbacks
{
    OptiPlugin* plugin;
    OptiUpscaleFn before;
    OptiUpscaleFn after;
    void* userData;
    bool faulted = false;
};

std::vector<std::shared_ptr<UpscaleCallbacks>> _upscaleCallbacks;
std::atomic<bool> _hasUpscaleCallbacks { false };
thread_local bool _inUpscaleCallback = false;

bool CallUpscale(OptiUpscaleFn fn, OptiUpscaleFrame* frame, void* userData)
{
    __try
    {
        fn(frame, userData);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool Api_RegisterUpscaleCallbacks(OptiPlugin* plugin, OptiUpscaleFn before, OptiUpscaleFn after, void* userData)
{
    if (plugin == nullptr || (before == nullptr && after == nullptr))
        return false;

    std::lock_guard<std::mutex> lock(_mutex);

    if (plugin->initState == OptiPlugin::InitState::Declined)
        return false;

    std::erase_if(_upscaleCallbacks, [plugin](const auto& c) { return c->plugin == plugin; });
    auto entry = std::make_shared<UpscaleCallbacks>();
    *entry = { plugin, before, after, userData };
    _upscaleCallbacks.push_back(std::move(entry));
    _hasUpscaleCallbacks = true;

    LOG_INFO("[{}] Registered upscale callbacks (before: {}, after: {})", plugin->name, before != nullptr,
             after != nullptr);

    return true;
}

void Api_RestoreCommandListState(void* commandList)
{
    if (commandList != nullptr && _inUpscaleCallback)
        D3D12Hooks::RestoreRoot(static_cast<ID3D12GraphicsCommandList*>(commandList));
}

struct OverlayText
{
    OptiPlugin* plugin;
    std::string label;
    OptiOverlayTextFn fn;
    void* userData;
    bool faulted = false;
};

std::vector<std::shared_ptr<OverlayText>> _overlayTexts;

bool CallOverlayText(OptiOverlayTextFn fn, char* buffer, size_t bufferSize, void* userData, bool* shown)
{
    __try
    {
        *shown = fn(buffer, bufferSize, userData);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool Api_RegisterOverlayText(OptiPlugin* plugin, const char* label, OptiOverlayTextFn fn, void* userData)
{
    if (plugin == nullptr || label == nullptr || fn == nullptr)
        return false;

    std::lock_guard<std::mutex> lock(_mutex);

    if (plugin->initState == OptiPlugin::InitState::Declined)
        return false;

    auto entry = std::make_shared<OverlayText>();
    *entry = { plugin, label, fn, userData };
    _overlayTexts.push_back(std::move(entry));

    LOG_INFO("[{}] Registered overlay text: {}", plugin->name, label);

    return true;
}

struct PresentCallback
{
    OptiPlugin* plugin;
    OptiPresentFn fn;
    uint32_t requests;
    void* userData;
    bool faulted = false;
};

std::vector<std::shared_ptr<PresentCallback>> _presentCallbacks;
std::atomic<uint32_t> _presentRequests { 0 };
std::atomic<bool> _hasPresentCallbacks { false };
std::atomic<uint64_t> _lastUpscaleFrameId { 0 };

bool CallPresent(OptiPresentFn fn, OptiPresentFrame* frame, void* userData)
{
    __try
    {
        fn(frame, userData);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void UpdatePresentRequests()
{
    uint32_t requests = 0;
    for (const auto& c : _presentCallbacks)
        requests |= c->requests;
    _presentRequests = requests;
    _hasPresentCallbacks = !_presentCallbacks.empty();
}

bool Api_RegisterPresentCallback(OptiPlugin* plugin, OptiPresentFn fn, uint32_t requests, void* userData)
{
    if (plugin == nullptr || fn == nullptr)
        return false;

    std::lock_guard<std::mutex> lock(_mutex);

    if (plugin->initState == OptiPlugin::InitState::Declined)
        return false;

    std::erase_if(_presentCallbacks, [plugin](const auto& c) { return c->plugin == plugin; });
    auto entry = std::make_shared<PresentCallback>();
    *entry = { plugin, fn, requests, userData };
    _presentCallbacks.push_back(std::move(entry));
    UpdatePresentRequests();

    LOG_INFO("[{}] Registered present callback (requests: {:X})", plugin->name, requests);

    return true;
}

struct AdapterOverride
{
    OptiPlugin* plugin = nullptr;
    OptiAdapterOverrideFn fn = nullptr;
    void* userData = nullptr;
};

struct SwapchainProvider
{
    OptiPlugin* plugin = nullptr;
    OptiSwapchainProviderFn fn = nullptr;
    void* userData = nullptr;
};

AdapterOverride _adapterOverride;
SwapchainProvider _swapchainProvider;
thread_local bool _unhookedDeviceCreate = false;

bool Api_RegisterAdapterOverride(OptiPlugin* plugin, OptiAdapterOverrideFn fn, void* userData)
{
    if (plugin == nullptr || fn == nullptr)
        return false;

    std::lock_guard<std::mutex> lock(_mutex);
    if (plugin->initState == OptiPlugin::InitState::Declined ||
        (_adapterOverride.plugin != nullptr && _adapterOverride.plugin != plugin))
        return false;

    _adapterOverride = { plugin, fn, userData };
    LOG_INFO("[{}] Registered the adapter override", plugin->name);
    return true;
}

int32_t Api_CreateD3D12DeviceUnhooked(void* adapter, int32_t minimumFeatureLevel, const void* riid, void** device)
{
    auto create = D3d12Proxy::D3D12CreateDevice_Hooked();
    if (create == nullptr || riid == nullptr || device == nullptr)
        return E_INVALIDARG;

    // OptiScaler's D3D12CreateDevice hooks pass the call straight through
    _unhookedDeviceCreate = true;
    const HRESULT result =
        create((IUnknown*) adapter, (D3D_FEATURE_LEVEL) minimumFeatureLevel, *static_cast<const IID*>(riid), device);
    _unhookedDeviceCreate = false;
    return result;
}

int32_t Api_CreateDxgiFactoryUnhooked(const void* riid, void** factory)
{
    if (riid == nullptr || factory == nullptr)
        return E_INVALIDARG;

    DxgiProxy::Init();
    if (auto create = DxgiProxy::CreateDxgiFactory1_())
        return create(*static_cast<const IID*>(riid), (IDXGIFactory1**) factory);

    return E_NOINTERFACE;
}

void Api_RunUnspoofed(void (*fn)(void* userData), void* userData)
{
    if (fn == nullptr)
        return;

    ScopedSkipSpoofingThread skipSpoofing {};
    fn(userData);
}

bool Api_RegisterSwapchainProvider(OptiPlugin* plugin, OptiSwapchainProviderFn fn, void* userData)
{
    if (plugin == nullptr || fn == nullptr)
        return false;

    std::lock_guard<std::mutex> lock(_mutex);
    if (plugin->initState == OptiPlugin::InitState::Declined ||
        (_swapchainProvider.plugin != nullptr && _swapchainProvider.plugin != plugin))
        return false;

    _swapchainProvider = { plugin, fn, userData };
    LOG_INFO("[{}] Registered the swapchain provider", plugin->name);
    return true;
}

int32_t Api_CreateSwapChainForHwndUnhooked(void* factory, void* device, void* hwnd, const void* desc1,
                                           const void* fullscreenDesc, void* restrictToOutput, void** swapChain)
{
    if (factory == nullptr || device == nullptr || desc1 == nullptr || swapChain == nullptr)
        return E_INVALIDARG;

    return DxgiFactoryHooks::CreateSwapChainForHwndOriginal(
        (IDXGIFactory2*) factory, (IUnknown*) device, (HWND) hwnd, (const DXGI_SWAP_CHAIN_DESC1*) desc1,
        (const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*) fullscreenDesc, (IDXGIOutput*) restrictToOutput,
        (IDXGISwapChain1**) swapChain);
}

bool CallAdapterOverride(OptiAdapterOverrideFn fn, void* requested, void* caller, void* userData, void** result)
{
    __try
    {
        *result = fn(requested, caller, userData);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool CallSwapchainProvider(OptiSwapchainProviderFn fn, void* factory, void* queue, void* hwnd, const void* desc,
                           const void* fs, void* restrict, void** swapChain, void* userData, int32_t* result)
{
    __try
    {
        *result = fn(factory, queue, hwnd, desc, fs, restrict, swapChain, userData);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

const OptiPluginApi _api = {
    .size = sizeof(OptiPluginApi),
    .version = OPTI_PLUGIN_API_VERSION,

    .RegisterTab = Api_RegisterTab,

    .Section = Api_Section,
    .Separator = Api_Separator,
    .SameLine = Api_SameLine,
    .Spacing = Api_Spacing,
    .Disabled = Api_Disabled,
    .ItemTooltip = Api_ItemTooltip,
    .ItemDeactivatedAfterEdit = Api_ItemDeactivatedAfterEdit,

    .Text = Api_Text,
    .TextDisabled = Api_TextDisabled,
    .TextColored = Api_TextColored,

    .Button = Api_Button,
    .Checkbox = Api_Checkbox,
    .SliderFloat = Api_SliderFloat,
    .SliderInt = Api_SliderInt,
    .Combo = Api_Combo,
    .InputText = Api_InputText,
    .ColorEdit = Api_ColorEdit,

    .IsMenuOpen = Api_IsMenuOpen,

    .GetBool = Api_GetBool,
    .GetInt = Api_GetInt,
    .GetFloat = Api_GetFloat,
    .GetString = Api_GetString,
    .SetBool = Api_SetBool,
    .SetInt = Api_SetInt,
    .SetFloat = Api_SetFloat,
    .SetString = Api_SetString,
    .ResetValue = Api_ResetValue,
    .SaveSettings = Api_SaveSettings,

    .Log = Api_Log,

    .RegisterUpscaleCallbacks = Api_RegisterUpscaleCallbacks,
    .RestoreCommandListState = Api_RestoreCommandListState,

    .RegisterOverlayText = Api_RegisterOverlayText,

    .RegisterPresentCallback = Api_RegisterPresentCallback,

    .RegisterAdapterOverride = Api_RegisterAdapterOverride,
    .CreateD3D12DeviceUnhooked = Api_CreateD3D12DeviceUnhooked,
    .CreateDxgiFactoryUnhooked = Api_CreateDxgiFactoryUnhooked,
    .RunUnspoofed = Api_RunUnspoofed,

    .RegisterSwapchainProvider = Api_RegisterSwapchainProvider,
    .CreateSwapChainForHwndUnhooked = Api_CreateSwapChainForHwndUnhooked,
};
} // namespace

void PluginApi::InitPlugin(HMODULE module, PFN_OptiPluginInit init, const std::filesystem::path& path)
{
    // Name shouldn't change when the plugin is switched to late loading
    std::string name = wstring_to_string(path.stem().wstring());
    constexpr std::string_view lateSuffix = "-loadlate";

    if (name.size() > lateSuffix.size())
    {
        std::string tail = name.substr(name.size() - lateSuffix.size());
        std::ranges::transform(tail, tail.begin(), [](unsigned char c) { return (char) std::tolower(c); });

        if (tail == lateSuffix)
            name.resize(name.size() - lateSuffix.size());
    }

    InitPlugin(module, init, name);
}

void PluginApi::InitPlugin(HMODULE module, PFN_OptiPluginInit init, const std::string& name)
{
    if (module == nullptr || init == nullptr)
        return;

    OptiPlugin* plugin = nullptr;

    {
        std::lock_guard<std::mutex> lock(_mutex);

        // Late loaded plugins and custom dlls can be reported more than once
        if (std::ranges::find(_initedModules, module) != _initedModules.end())
            return;

        _initedModules.push_back(module);

        auto& created = _plugins.emplace_back(std::make_unique<OptiPlugin>());
        created->name = name;
        created->section = "Plugin." + name;
        plugin = created.get();
    }

    LOG_INFO("Initializing plugin API for {}", plugin->name);

    bool accepted = false;
    const bool finished = CallInit(init, &_api, plugin, &accepted);

    if (!finished)
        LOG_ERROR("[{}] Crashed in OptiPluginInit, its tabs are removed", plugin->name);
    else if (!accepted)
        LOG_INFO("[{}] Declined the plugin API", plugin->name);

    std::lock_guard<std::mutex> lock(_mutex);

    // Declined plugins can't register tabs later either
    plugin->initState = finished && accepted ? OptiPlugin::InitState::Accepted : OptiPlugin::InitState::Declined;

    if (plugin->initState == OptiPlugin::InitState::Accepted)
    {
        for (const auto& tab : _pendingTabs)
        {
            if (tab->plugin == plugin)
                _tabs.push_back(tab);
        }
    }

    std::erase_if(_pendingTabs, [plugin](const auto& tab) { return tab->plugin == plugin; });

    if (plugin->initState != OptiPlugin::InitState::Accepted)
    {
        std::erase_if(_upscaleCallbacks, [plugin](const auto& c) { return c->plugin == plugin; });
        _hasUpscaleCallbacks = !_upscaleCallbacks.empty();
        std::erase_if(_overlayTexts, [plugin](const auto& t) { return t->plugin == plugin; });
        std::erase_if(_presentCallbacks, [plugin](const auto& c) { return c->plugin == plugin; });
        UpdatePresentRequests();
        if (_adapterOverride.plugin == plugin)
            _adapterOverride = {};
        if (_swapchainProvider.plugin == plugin)
            _swapchainProvider = {};
    }
}

std::vector<PluginApi::TabInfo> PluginApi::GetTabs()
{
    std::lock_guard<std::mutex> lock(_mutex);

    std::vector<TabInfo> tabs;
    tabs.reserve(_tabs.size());

    for (const auto& tab : _tabs)
        tabs.push_back({ tab->name, tab->description });

    return tabs;
}

void PluginApi::DrawTab(size_t index, float menuScale, ImVec4 (*colorMap)(const ImVec4&))
{
    std::shared_ptr<PluginTab> tab;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (index >= _tabs.size())
            return;

        tab = _tabs[index];
    }

    if (tab->faulted)
    {
        ImGui::TextColored(colorMap(ImVec4(1.0f, 0.4f, 0.4f, 1.0f)), "%s",
                           "This plugin crashed while drawing its tab and was disabled.\nCheck the log for details.");
        return;
    }

    // Lock isn't held while the plugin runs, it can register more tabs from here
    TabDrawState state;
    state.colorMap = colorMap;

    ImGuiErrorRecoveryState recovery;
    ImGui::ErrorRecoveryStoreState(&recovery);

    ImGui::PushID(tab->plugin->name.c_str());
    ImGui::PushID(tab->name.c_str());
    ImGui::PushItemWidth(220.0f * menuScale);

    _draw = &state;
    const bool finished = CallDraw(tab->draw, tab->userData);
    _draw = nullptr;

    if (!finished)
    {
        LOG_ERROR("[{}] Crashed while drawing tab {}, disabling it", tab->plugin->name, tab->name);
        tab->faulted = true;

        // Closes whatever was left open when the plugin crashed, popups of a combo for example.
        // Also pops the IDs and the disabled state from above, but not the item width.
        auto& io = ImGui::GetIO();
        const bool enableAssert = io.ConfigErrorRecoveryEnableAssert;
        const bool enableTooltip = io.ConfigErrorRecoveryEnableTooltip;
        io.ConfigErrorRecoveryEnableAssert = false;
        io.ConfigErrorRecoveryEnableTooltip = false;

        ImGui::ErrorRecoveryTryToRecoverState(&recovery);

        io.ConfigErrorRecoveryEnableAssert = enableAssert;
        io.ConfigErrorRecoveryEnableTooltip = enableTooltip;

        ImGui::PopItemWidth();
        state.card.reset();
        return;
    }

    if (state.disabled)
        ImGui::EndDisabled();

    state.card.reset();

    ImGui::PopItemWidth();
    ImGui::PopID();
    ImGui::PopID();
}

namespace
{
ID3D12Resource* GetNgxResource(NVSDK_NGX_Parameter* params, const char* name)
{
    ID3D12Resource* res = nullptr;
    if (params->Get(name, &res) != NVSDK_NGX_Result_Success)
        params->Get(name, (void**) &res);
    return res;
}
} // namespace

void PluginApi::FillUpscaleFrame(OptiUpscaleFrame& f, IFeature* feature, NVSDK_NGX_Parameter* params,
                                 ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12CommandQueue* queue)
{
    static std::atomic<uint64_t> frameId { 0 };
    const Config& cfg = *Config::Instance();

    f.size = sizeof(OptiUpscaleFrame);
    f.api = OptiApi_D3D12;
    f.gameApi = OptiApi_D3D12;
    f.device = device;
    f.commandList = list;
    f.commandQueue = queue;
    f.color = GetNgxResource(params, NVSDK_NGX_Parameter_Color);
    f.output = GetNgxResource(params, NVSDK_NGX_Parameter_Output);
    f.depth = GetNgxResource(params, NVSDK_NGX_Parameter_Depth);
    f.motionVectors = GetNgxResource(params, NVSDK_NGX_Parameter_MotionVectors);
    f.exposure = GetNgxResource(params, NVSDK_NGX_Parameter_ExposureTexture);

    // The configured barriers name the game's states, else the NGX convention the upscalers assume
    constexpr int32_t read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const bool assumed = !cfg.ColorResourceBarrier.has_value() || !cfg.OutputResourceBarrier.has_value();
    f.colorState = (uint32_t) cfg.ColorResourceBarrier.value_or(read);
    f.outputState = (uint32_t) cfg.OutputResourceBarrier.value_or(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    f.depthState = (uint32_t) cfg.DepthResourceBarrier.value_or(read);
    f.motionVectorsState = (uint32_t) cfg.MVResourceBarrier.value_or(read);
    f.exposureState = (uint32_t) cfg.ExposureResourceBarrier.value_or(read);

    unsigned int renderWidth = 0, renderHeight = 0;
    if (params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &renderWidth) != NVSDK_NGX_Result_Success ||
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &renderHeight) != NVSDK_NGX_Result_Success ||
        renderWidth == 0 || renderHeight == 0)
    {
        renderWidth = feature->RenderWidth();
        renderHeight = feature->RenderHeight();
    }
    f.renderWidth = renderWidth;
    f.renderHeight = renderHeight;
    f.outputWidth = feature->TargetWidth();
    f.outputHeight = feature->TargetHeight();

    params->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &f.jitterX);
    params->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &f.jitterY);
    f.mvScaleX = 1.0f;
    f.mvScaleY = 1.0f;
    params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &f.mvScaleX);
    params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &f.mvScaleY);
    f.preExposure = 1.0f;
    params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &f.preExposure);
    if (!(f.preExposure > 0.0f))
        f.preExposure = 1.0f;

    unsigned int reset = 0;
    params->Get(NVSDK_NGX_Parameter_Reset, &reset);

    const auto type = feature->GetUpscalerType();
    f.flags = (reset == 1 ? OptiUpscale_Reset : 0) | (feature->DepthInverted() ? OptiUpscale_DepthInverted : 0) |
              (feature->JitteredMV() ? OptiUpscale_JitteredMV : 0) |
              (!feature->LowResMV() ? OptiUpscale_DisplayResMV : 0) | (feature->IsHdr() ? OptiUpscale_Hdr : 0) |
              (feature->AutoExposure() ? OptiUpscale_AutoExposure : 0) |
              (type == Upscaler::DLSSD ? OptiUpscale_RayReconstruction : 0) |
              (assumed ? OptiUpscale_StatesAssumed : 0);
    f.frameId = ++frameId;
    f.colorOverride = nullptr;
}

bool PluginApi::HasUpscaleCallbacks() { return _hasUpscaleCallbacks.load(std::memory_order_relaxed); }

void PluginApi::RunUpscaleCallbacks(bool after, OptiUpscaleFrame* frame)
{
    std::vector<std::shared_ptr<UpscaleCallbacks>> callbacks;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& c : _upscaleCallbacks)
        {
            // Not before OptiPluginInit returned true
            if (!c->faulted && c->plugin->initState == OptiPlugin::InitState::Accepted)
                callbacks.push_back(c);
        }
    }

    for (const auto& c : callbacks)
    {
        auto fn = after ? c->after : c->before;
        if (fn == nullptr)
            continue;

        _inUpscaleCallback = true;
        _lastUpscaleFrameId = frame->frameId;
        const bool finished = CallUpscale(fn, frame, c->userData);
        _inUpscaleCallback = false;

        if (!finished)
        {
            LOG_ERROR("[{}] Crashed in an upscale callback, disabling its callbacks", c->plugin->name);
            c->faulted = true;
        }
    }
}

std::vector<PluginApi::OverlayLine> PluginApi::GetOverlayLines()
{
    std::vector<std::shared_ptr<OverlayText>> texts;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& t : _overlayTexts)
        {
            if (!t->faulted && t->plugin->initState == OptiPlugin::InitState::Accepted)
                texts.push_back(t);
        }
    }

    std::vector<OverlayLine> lines;
    char buffer[256];

    for (const auto& t : texts)
    {
        buffer[0] = '\0';
        bool shown = false;

        if (!CallOverlayText(t->fn, buffer, sizeof(buffer), t->userData, &shown))
        {
            LOG_ERROR("[{}] Crashed in overlay text {}, removing it", t->plugin->name, t->label);
            t->faulted = true;
            continue;
        }

        if (shown)
        {
            buffer[sizeof(buffer) - 1] = '\0';
            lines.push_back({ t->label, buffer });
        }
    }

    return lines;
}

bool PluginApi::HasPresentCallbacks() { return _hasPresentCallbacks.load(std::memory_order_relaxed); }

bool PluginApi::WantsHudless()
{
    return (_presentRequests.load(std::memory_order_relaxed) & OptiPresentRequest_Hudless) != 0;
}

uint64_t PluginApi::LastUpscaleFrameId() { return _lastUpscaleFrameId.load(std::memory_order_relaxed); }

namespace
{
std::mutex _gameFgMutex;
ID3D12Resource* _gameHudless = nullptr;
uint32_t _gameHudlessState = 0;
std::chrono::steady_clock::time_point _gameFgSeen {};
bool _gameFgGenerating = false;
} // namespace

void PluginApi::SetGameFg(const char* source, ID3D12Resource* hudless, uint32_t hudlessState, bool generating)
{
    static bool loggedHudless = false, loggedNone = false, lastGenerating = false;
    std::lock_guard<std::mutex> lock(_gameFgMutex);
    _gameHudless = hudless;
    _gameHudlessState = hudlessState;
    _gameFgGenerating = generating;
    _gameFgSeen = std::chrono::steady_clock::now();
    if (hudless != nullptr && !std::exchange(loggedHudless, true))
    {
        const auto d = hudless->GetDesc();
        LOG_INFO("Plugin HUD-less: {} gives one: {:X} {}x{} fmt {} state {:X}", source, (size_t) hudless, d.Width,
                 d.Height, (UINT) d.Format, hudlessState);
    }
    else if (hudless == nullptr && !std::exchange(loggedNone, true))
        LOG_INFO("Plugin HUD-less: {} runs without one", source);
    if (generating != std::exchange(lastGenerating, generating))
        LOG_INFO("Plugin present: {} frame generation {}", source, generating ? "on" : "off");
}

ID3D12Resource* PluginApi::TakeGameHudless(uint32_t* state, bool* generating)
{
    std::lock_guard<std::mutex> lock(_gameFgMutex);
    // A game that stops configuring its frame generation no longer generates
    const bool fresh = std::chrono::steady_clock::now() - _gameFgSeen < std::chrono::milliseconds(500);
    *generating = fresh && _gameFgGenerating;
    if (!fresh || _gameFgGenerating)
        return nullptr;
    *state = _gameHudlessState;
    return std::exchange(_gameHudless, nullptr);
}

void PluginApi::RunPresentCallbacks(OptiPresentFrame* frame)
{
    std::vector<std::shared_ptr<PresentCallback>> callbacks;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& c : _presentCallbacks)
        {
            if (!c->faulted && c->plugin->initState == OptiPlugin::InitState::Accepted)
                callbacks.push_back(c);
        }
    }

    for (const auto& c : callbacks)
    {
        if (!CallPresent(c->fn, frame, c->userData))
        {
            LOG_ERROR("[{}] Crashed in its present callback, disabling it", c->plugin->name);
            c->faulted = true;
        }
    }
}

bool PluginApi::UnhookedDeviceCreate() { return _unhookedDeviceCreate; }

IUnknown* PluginApi::OverrideAdapter(IUnknown* requested, void* caller)
{
    AdapterOverride o;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        o = _adapterOverride;
    }

    if (o.fn == nullptr || o.plugin->initState != OptiPlugin::InitState::Accepted)
        return nullptr;

    void* adapter = nullptr;
    if (!CallAdapterOverride(o.fn, requested, caller, o.userData, &adapter))
    {
        LOG_ERROR("[{}] Crashed in its adapter override, removing it", o.plugin->name);
        std::lock_guard<std::mutex> lock(_mutex);
        _adapterOverride = {};
        return nullptr;
    }

    if (adapter != nullptr && adapter != requested)
        LOG_INFO("[{}] Device adapter {:X} -> {:X}", o.plugin->name, (size_t) requested, (size_t) adapter);

    return static_cast<IUnknown*>(adapter);
}

bool PluginApi::HasSwapchainProvider()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _swapchainProvider.fn != nullptr && _swapchainProvider.plugin->initState == OptiPlugin::InitState::Accepted;
}

HRESULT PluginApi::ProvideSwapchain(IDXGIFactory2* factory, IUnknown* queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc, IDXGIOutput* restrictToOutput,
                                    IDXGISwapChain1** swapChain)
{
    SwapchainProvider p;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        p = _swapchainProvider;
    }

    if (p.fn == nullptr)
        return E_NOTIMPL;

    int32_t result = E_NOTIMPL;
    if (!CallSwapchainProvider(p.fn, factory, queue, hwnd, desc, fullscreenDesc, restrictToOutput,
                               (void**) swapChain, p.userData, &result))
    {
        LOG_ERROR("[{}] Crashed in its swapchain provider, removing it", p.plugin->name);
        std::lock_guard<std::mutex> lock(_mutex);
        _swapchainProvider = {};
        return E_NOTIMPL;
    }

    if (result == S_OK && (swapChain == nullptr || *swapChain == nullptr))
        return E_NOTIMPL;

    LOG_INFO("[{}] Swapchain provider: {:X}", p.plugin->name, (uint32_t) result);
    return (HRESULT) result;
}

HRESULT PluginApi::ProvideSwapchainLegacy(IDXGIFactory* factory, IUnknown* queue, const DXGI_SWAP_CHAIN_DESC* desc,
                                          IDXGISwapChain** swapChain)
{
    if (factory == nullptr || queue == nullptr || desc == nullptr || swapChain == nullptr || !HasSwapchainProvider())
        return E_NOTIMPL;

    IDXGIFactory2* factory2 = nullptr;
    if (factory->QueryInterface(IID_PPV_ARGS(&factory2)) != S_OK || factory2 == nullptr)
    {
        LOG_WARN("Legacy CreateSwapChain: no IDXGIFactory2, swapchain provider skipped");
        return E_NOTIMPL;
    }

    DXGI_SWAP_CHAIN_DESC1 desc1 {};
    desc1.Width = desc->BufferDesc.Width;
    desc1.Height = desc->BufferDesc.Height;
    desc1.Format = desc->BufferDesc.Format;
    desc1.SampleDesc = desc->SampleDesc;
    desc1.BufferUsage = desc->BufferUsage;
    desc1.BufferCount = desc->BufferCount;
    desc1.Scaling = DXGI_SCALING_STRETCH; // legacy swapchains stretch to the window
    desc1.SwapEffect = desc->SwapEffect;
    desc1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc1.Flags = desc->Flags;

    // Mode fields and Windowed move to the fullscreen desc
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs {};
    fs.RefreshRate = desc->BufferDesc.RefreshRate;
    fs.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
    fs.Scaling = desc->BufferDesc.Scaling;
    fs.Windowed = desc->Windowed;

    LOG_INFO("Legacy CreateSwapChain -> swapchain provider: {}x{} fmt {} count {} hwnd {:X} windowed {}", desc1.Width,
             desc1.Height, (UINT) desc1.Format, desc1.BufferCount, (size_t) desc->OutputWindow, desc->Windowed);

    IDXGISwapChain1* sc1 = nullptr;
    auto result = ProvideSwapchain(factory2, queue, desc->OutputWindow, &desc1, &fs, nullptr, &sc1);
    factory2->Release();

    if (result != S_OK)
        return result;

    if (sc1->QueryInterface(IID_PPV_ARGS(swapChain)) == S_OK && *swapChain != nullptr)
        sc1->Release();
    else
        *swapChain = sc1; // IDXGISwapChain1 is an IDXGISwapChain

    return S_OK;
}
