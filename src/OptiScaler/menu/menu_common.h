// Modifications Copyright (c) 2026 3zwr1 (AMDNR)
#pragma once

#include "SysUtils.h"
#include <Config.h>

#include <imgui/imgui.h>

// AMDNR 0.3.4 menu look: the main window's palette, taken from the owner-approved mock
// (the owner-approved menu mock, APPROVED_MOCK.png). MenuCommon::ThemeColor() hands
// them out; the helpers that draw by hand (dlssnr/menu/NeuralUi.h) use it.
enum class MenuColor : int
{
    Text,         // #e6e4df labels
    Dim,          // #8d8a84 credits, status lines, inactive tabs, footer
    Tag,          // #7d7a74 the short word after a row ("running", "1.00x cost")
    Section,      // #e05a5a section titles
    Rule,         // #3a2226 the 1 px lines (sections, tab row, footer) and the window border
    Frame,        // #26262a slider track, combo box, button
    FrameHovered, // #2e2e33
    FrameActive,  // #35353b
    Fill,         // #7a1f2b slider fill
    FillActive,   // #8f1d2c slider fill while hovered or dragged
    Value,        // #dddddd the value inside a slider
    CheckOutline, // #777777 an unticked checkbox
    CheckOn,      // #c73a45 a ticked checkbox, the active tab's underline
    ButtonBorder, // #4a2a30
    Selected,     // #8f1d2c a selected button, the title bar
    White,        // #ffffff the active tab, the title text
    Count
};

class ScopedIndent
{
  public:
    explicit ScopedIndent(float indent = 16.0f) : m_indent(indent) { ImGui::Indent(m_indent); }

    ~ScopedIndent() { ImGui::Unindent(m_indent); }

  private:
    float m_indent;
};

class ScopedID
{
  public:
    explicit ScopedID(const char* id) { ImGui::PushID(id); }

    ~ScopedID() { ImGui::PopID(); }
};

struct ImGuiWindow;

// Draws a group box around everything drawn while it is alive. Nothing is drawn when it stays empty
class ScopedCard
{
  public:
    explicit ScopedCard(const char* title = nullptr);
    ~ScopedCard();

    ScopedCard(const ScopedCard&) = delete;
    ScopedCard& operator=(const ScopedCard&) = delete;

    // Turns a separator text into the title of the card when it's the first thing in it
    static bool TryTitle(const char* label);

    // With a filter set, only the box with the same id is drawn
    static bool Shows(const char* id);
    static void SetFilter(const char* id);

  private:
    void DrawTitle(const char* label);

    inline static ScopedCard* _current = nullptr;
    inline static const char* _filter = nullptr;

    ImGuiWindow* _window = nullptr;
    ImDrawListSplitter _splitter;
    ImVec2 _min {};
    ImVec2 _pad {};
    float _width = 0.0f;
    float _contentStartY = 0.0f;
    float _oldWorkRectMaxX = 0.0f;
    float _oldContentRectMaxX = 0.0f;
    bool _active = false;
    ImVec2 _titleMin {};
    ImVec2 _titleMax {};
    bool _hasTitle = false;
};

class ScopedCollapsingHeader
{
  public:
    explicit ScopedCollapsingHeader(const char* label, ImGuiTreeNodeFlags flags = 0)
    {
        ImGui::PushID(label);

        ImGui::BeginChild("##CollapsingHeaderChild", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        _headerOpen = ImGui::CollapsingHeader(label, flags);
        _active = true;
    }

    bool IsHeaderOpen() const { return _headerOpen; }

    ~ScopedCollapsingHeader()
    {
        if (_active)
        {
            ImGui::EndChild();
            ImGui::PopID();
        }
    }

  private:
    bool _active = false;
    bool _headerOpen = false;
};

template <typename T> struct MenuOption
{
    T value;
    std::string label;
    std::string tooltip;
    bool disabled = false;
    bool hidden = false;

    MenuOption& set_disabled(bool condition, const std::string& reason = "")
    {
        if (condition)
        {
            disabled = true;
            if (!reason.empty())
                tooltip = reason;
        }
        return *this;
    }

    MenuOption& set_hidden(bool condition)
    {
        if (condition)
        {
            hidden = true;
        }
        return *this;
    }
};

class MenuCommon
{
  private:
    // internal values
    inline static HWND _handle = nullptr;
    // inline static WNDPROC _oWndProc = nullptr;
    inline static bool _isVisible = false;
    inline static bool _isInited = false;
    inline static bool _isUWP = false;

    // mipmap calculations
    inline static bool _showMipmapCalcWindow = false;
    inline static bool _showHudlessWindow = false;
    inline static float _mipBias = 0.0f;
    inline static float _mipBiasCalculated = 0.0f;
    inline static uint32_t _mipmapUpscalerQuality = 0;
    inline static float _mipmapUpscalerRatio = 0;
    inline static uint32_t _displayWidth = 0;
    inline static uint32_t _renderWidth = 0;

    inline static UINT64 _frameCount = 0;

    // reflex
    inline static float _limitFps = std::numeric_limits<float>::infinity();

    // ffx
    inline static int _ffxFGIndex = -1;

    // output scaling
    inline static float _ssRatio = 0.0f;
    inline static bool _ssEnabled = false;
    inline static Scaler _ssDownsampler = Scaler::FSR1;

    // ui scale
    inline static int _selectedScale = 0;

    // overlay states
    inline static bool _dx11Ready = false;
    inline static bool _dx12Ready = false;
    inline static bool _vulkanReady = false;

    // Blurred game image provided by the renderer, drawn behind the main menu window
    inline static ImTextureID _bgBlurTexture = ImTextureID_Invalid;
    inline static ImVec2 _bgBlurUVScale { 1.0f, 1.0f };
    inline static bool _bgBlurUsed = false;

    inline static void ShowTooltip(const char* tip);

    inline static void ShowResetButton(CustomOptional<bool, NoDefault>* initFlag, std::string buttonName);
    inline static void ReInitUpscaler();

    inline static void SeparatorWithHelpMarker(const char* label, const char* tip);

    inline static bool SliderUInt(const char* label, uint32_t* v, uint32_t v_min, uint32_t v_max,
                                  const char* format = "%d", ImGuiSliderFlags flags = 0);

    static Upscaler GetBackendCode(const API api);
    static void GetCurrentBackendInfo(const API api, Upscaler& upscaler, std::string* name);
    static void RenderUpscalerCombo(const API api, Upscaler currentUpscaler, const std::vector<Upscaler>& options);
    static void AddDx11Backends(Upscaler upscaler);
    static void AddDx12Backends(Upscaler upscaler);
    static void AddVulkanBackends(Upscaler upscaler);
    template <HasDefaultValue B> static void AddResourceBarrier(std::string name, CustomOptional<int32_t, B>* value);
    template <typename TOption>
    static bool ConfigCheckbox(const char* label, TOption& option, const char* tip = nullptr,
                               bool needsRestart = false);
    template <HasDefaultValue B> static void AddDLSSRenderPreset(std::string name, CustomOptional<uint32_t, B>* value);
    template <HasDefaultValue B> static void AddDLSSDRenderPreset(std::string name, CustomOptional<uint32_t, B>* value);
    template <typename TStorage, typename T>
    static void PopulateCombo(const std::string& name, TStorage& currentValue,
                              const std::vector<MenuOption<T>>& options);

    struct RenderMenuContext;

    // RenderMenu orchestration helpers. These keep the public RenderMenu() flow short
    // while preserving the original ImGui layout and draw order.
    static void UpdateRenderTiming(RenderMenuContext& ctx);
    static void UpdateMenuInputMode(RenderMenuContext& ctx);
    static void HandleMenuShortcuts(RenderMenuContext& ctx);
    static void UpdateVersionAndStartupNotifications(RenderMenuContext& ctx);
    static void BeginMenuFrameIfNeeded(RenderMenuContext& ctx);
    static void RenderSplashWindow(RenderMenuContext& ctx);
    static void RenderNotifications(RenderMenuContext& ctx);
    static void UpdateFrameTimeAverages(RenderMenuContext& ctx);
    static void RenderPerformanceOverlay(RenderMenuContext& ctx);

    // AMDNR: labels for the Neural Rendering comparison views. Drawn every frame, into the frame plane,
    // so a screenshot keeps them and the wipe reveals and hides them like the images.
    static void RenderNrCompareTags();
    static void RenderMainMenuWindow(RenderMenuContext& ctx);

    // RenderMainMenuWindow section helpers. These keep the main window flow readable
    // without changing the existing ImGui layout, labels, or setting side effects.
    static void RenderMainMenuHeaderMessages(RenderMenuContext& ctx);
    static void RenderUpscalerStateMessage(RenderMenuContext& ctx);
    static void RenderMainMenuTabs(RenderMenuContext& ctx);
    static void PlotLinesWithRange(const char* label, float (*values_getter)(void* data, int idx), void* data,
                                   int values_count, int values_offset, const char* overlay_text, ImVec2 graph_size,
                                   float padding);
    static void RenderCard(RenderMenuContext& ctx, void (*render)(RenderMenuContext&));

    // Custom tab
    struct MenuBox;
    static const MenuBox* GetMenuBoxes(size_t& count);
    static void RenderMenuBox(RenderMenuContext& ctx, const MenuBox& box);
    static void RenderCustomTab(RenderMenuContext& ctx);
    static void RenderCustomTabSettings(RenderMenuContext& ctx);
    static void RenderStatusInfo(RenderMenuContext& ctx);
    static void RenderDetectedInfo(RenderMenuContext& ctx);
    static void RenderDetectedCard(RenderMenuContext& ctx);
    static void RenderMenuScaleSettings(RenderMenuContext& ctx);
    static void RenderActiveUpscalerSettings(RenderMenuContext& ctx);
    static void RenderFrameGenerationSelection(RenderMenuContext& ctx);
    static void RenderFrameGenerationRuntimeSettings(RenderMenuContext& ctx);
    // AMDNR: Frame Gen > Compatibility, the external FG / MFG unlocker switch
    static void RenderFrameGenerationCompatibility(RenderMenuContext& ctx);
    static void RenderFGModeSlider(RenderMenuContext& ctx, const char* label, int maxInterpolationCount,
                                   CustomOptional<int>* countConfig, const char* tooltip = nullptr);
    static void RenderFsrCommonSettings(RenderMenuContext& ctx);
    static void RenderFramerateSettings(RenderMenuContext& ctx);
    static void RenderVsyncSettings(RenderMenuContext& ctx);
    static void RenderLowLatencySettings(RenderMenuContext& ctx);
    static void RenderActiveImageSettings(RenderMenuContext& ctx);
    static void RenderUpscaleRatioSettings(RenderMenuContext& ctx);
    static void RenderOutputScalingSettings(RenderMenuContext& ctx);
    static void RenderInitFlagsSettings(RenderMenuContext& ctx);
    static void RenderMagnifierSettings(RenderMenuContext& ctx);
    static void RenderQuirksSettings(RenderMenuContext& ctx);
    static void RenderAdvancedSettings(RenderMenuContext& ctx);
    static void RenderLoggingSettings(RenderMenuContext& ctx);
    static void RenderThemeSettings(RenderMenuContext& ctx);
    static void RenderFpsOverlaySettings(RenderMenuContext& ctx);
    static void RenderUpscalerInputsSettings(RenderMenuContext& ctx);
    static void RenderApiAndTextureSettings(RenderMenuContext& ctx);
    static void RenderKeybindSettings(RenderMenuContext& ctx);
    static void RenderMainMenuFooter(RenderMenuContext& ctx);
    static void RenderFrameTimeGraph(RenderMenuContext& ctx);
    static void RenderUpscalerTimeGraph(RenderMenuContext& ctx);
    static void RenderShaderTimes(RenderMenuContext& ctx);
    static void RenderMainMenuBottomBar(RenderMenuContext& ctx);
    static void RenderMipmapBiasWindow(RenderMenuContext& ctx, ImGuiWindowFlags flags);
    static void RenderHudlessResourcesWindow(RenderMenuContext& ctx, ImGuiWindowFlags flags);

    static void UpdateManualInput(HWND targetHwnd);

  public:
    // The main window's tabs, in their order (AMDNR 0.3.4, T11g).
    enum class MenuTab
    {
        Neural,
        Upscaling,
        FrameGen,
        Image,
        Interface,
        Advanced,
    };
    // Selects a tab of the main window on the next frame, e.g. an "Open Upscaling" button on the Neural tab
    // (AMDNR 0.3.4, T11g). Menu thread only.
    static void RequestTab(MenuTab tab);

    // AMDNR 0.3.4 menu look: a palette colour through the menu's HDR tone map (MenuHdrCheck tone-maps the style
    // colours; a colour drawn by hand must get the same, or it glows in an HDR game). With [Menu] LightTheme=true the
    // light style's own colours stand in (the mock has no light variant). Menu thread only.
    static ImVec4 ThemeColor(MenuColor c);
    // The same with the current style alpha applied (a greyed row), ready for an ImDrawList call.
    static ImU32 ThemeColorU32(MenuColor c);
    // AMDNR 0.3.4 menu look: one mock px at the current font size (the menu font is 14 px at Menu Scale 1.0, which
    // draws the mock's 12.5 px CSS text; a [Menu] FontSize override and the Menu Scale scale it). Every hand-drawn
    // size of the main window is in mock px times this (DlssNr::NeuralUi::Px). Menu thread only.
    static float MockPx();
    // AMDNR 0.3.4 (the approved mock, D-3): the Save report button and its result block (Saving / Report saved with
    // Open folder / Hide, or the orange failure), shared state; drawn first in Advanced > Logging and last in the
    // Neural tab's Diagnostics drawer. idSuffix keeps the two places' IDs apart. Menu thread only.
    static void RenderSaveReportRow(const char* idSuffix);

    // The Neural Rendering toggle-key binder, for the Neural tab; and a key's display name.
    static void RenderDlssNrToggleKeybind();
    static std::string KeyName(int virtualKey);
    static void Dx11Inited() { _dx11Ready = true; }
    static void Dx12Inited() { _dx12Ready = true; }
    static void VulkanInited() { _vulkanReady = true; }
    static bool IsInited() { return _isInited; }
    static bool IsVisible() { return _isVisible; }
    static HWND Handle() { return _handle; }

    // Texture should cover the whole display, uvScale is the part of it matching the display
    static void SetBackgroundBlur(ImTextureID texture, ImVec2 uvScale = ImVec2(1.0f, 1.0f))
    {
        _bgBlurTexture = texture;
        _bgBlurUVScale = uvScale;
        _bgBlurUsed = false;
    }

    // True if the last RenderMenu call drew the blur texture, so the renderer needs to update it
    static bool BackgroundBlurUsed() { return _bgBlurUsed; }

    static bool RenderMenu();
    static void Init(HWND InHwnd, bool isUWP);
    static void Shutdown();
    static void HideMenu();
    static void Present();
    static void ApplyThemeStyle();

    // Vulkan FG outputs present the generated frames on their own, the game's presents only show the real ones.
    // FGOutputPresent is called for each frame they present, FGOutputPresents says if the game's frames go through
    // them.
    static void FGOutputPresent();
    static void FGOutputPresents(bool presents);
};
