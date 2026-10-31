#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>
#include <roapi.h>
#include <winstring.h>
#include <gdiplus.h>
#include <uxtheme.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <devicetopology.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "resource.h"
#include "../Shared/HXAudioBridgePeakGuard.h"
#include "..\Shared\HXAudioDspHeadroom.h"

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

#define RETURN_IF_FAILED(expression) do { const HRESULT _result = (expression); if (FAILED(_result)) return _result; } while (0)

namespace
{
    constexpr wchar_t kControlWindowClass[] = L"HXAudioVirtualAudioControl";
    constexpr wchar_t kRegPath[] = L"Software\\HXAudio\\VirtualAudio";
    constexpr wchar_t kDspNotifyPath[] =
        L"Software\\HXAudio\\VirtualAudio\\DspNotify";
    constexpr wchar_t kStopEvent[] = L"Local\\HXAudioVirtualRouterStop";
    constexpr wchar_t kRouterMutex[] = L"Local\\HXAudioVirtualRouter";
    constexpr wchar_t kRouterReadyEvent[] = L"Local\\HXAudioVirtualRouterReady";
    constexpr wchar_t kRoutingSettingsEvent[] = L"Local\\HXAudioRoutingSettingsChanged";
    constexpr wchar_t kDspSettingsEvent[] = L"Local\\HXAudioDspSettingsChanged";
    constexpr wchar_t kStaticGainMutex[] =
        L"Local\\HXAudioStaticGainCompensation";
    constexpr wchar_t kApoRuntimeStatusPath[] =
        L"Software\\HXAudio\\Apo\\RuntimeStatus";
    constexpr wchar_t kVirtualName[] = L"HXAudio Virtual Output";
    constexpr DWORD kBufferChoices[] = { 3, 5, 10, 20, 40 };

    constexpr int kDeviceListId = 100;
    constexpr int kRefreshButtonId = 103;
    constexpr int kBufferSliderId = 104;
    constexpr int kHardwareProbeButtonId = 105;
    constexpr int kHardwareApplyButtonId = 106;
    constexpr int kHardwareRestoreButtonId = 107;
    constexpr int kPreampEditId = 108;
    constexpr int kPeqEditorButtonId = 109;
    constexpr int kPeqSummaryId = 110;
    constexpr int kVirtualBassEditorButtonId = 112;
    constexpr int kVirtualBassMasterEnableId = 113;
    constexpr int kAboutButtonId = 114;
    constexpr int kTrayOpenButtonId = 601;
    constexpr int kTrayExitButtonId = 602;
    constexpr int kAboutCloseButtonId = 501;
    constexpr int kPeqEnabledId = 301;
    constexpr int kPeqTypeId = 302;
    constexpr int kPeqFrequencyId = 303;
    constexpr int kPeqGainId = 304;
    constexpr int kPeqQId = 305;
    constexpr int kPeqApplyValueId = 306;
    constexpr int kPeqFitViewId = 307;
    constexpr int kPeqDeleteId = 308;
    constexpr int kPeqLiveApplyId = 309;
    constexpr int kPeqDeviceId = 310;
    constexpr int kPeqGainZoomOutId = 311;
    constexpr int kPeqGainZoomInId = 312;
    constexpr int kVirtualBassEnabledId = 401;
    constexpr int kVirtualBassModeId = 402;
    constexpr int kVirtualBassAmountId = 403;
    constexpr int kVirtualBassCutoffId = 404;
    constexpr int kVirtualBassSubsonicId = 405;
    constexpr int kVirtualBassApplyId = 406;
    constexpr int kVirtualBassResetId = 407;
    constexpr int kVirtualBassLiveApplyId = 408;
    constexpr int kVirtualBassCustom1Id = 409;
    constexpr int kVirtualBassCustom2Id = 410;
    constexpr int kVirtualBassCustom3Id = 411;
    constexpr UINT_PTR kStatusTimerId = 1;
    constexpr UINT kApplyInitialDpiSizeMessage = WM_APP + 42;
    constexpr UINT kDeviceVolumesUpdatedMessage = WM_APP + 43;
    constexpr UINT kTrayCallbackMessage = WM_APP + 44;
    constexpr UINT kShowControlWindowMessage = WM_APP + 45;
    constexpr UINT kExitControlWindowMessage = WM_APP + 46;
    constexpr UINT kTrayIconId = 1;
    constexpr wchar_t kAboutWindowClass[] = L"HXAudioModernAbout";

    struct DeviceInfo
    {
        std::wstring id;
        std::wstring name;
        std::wstring profileKey;
        float volume = 1.0f;
        bool muted = false;
        bool volumeKnown = false;
        bool playbackSelected = false;
    };

    struct CpuTopology
    {
        DWORD physicalCores = 1;
        std::vector<PROCESSOR_NUMBER> logicalProcessors;
    };

    std::vector<DeviceInfo> g_devices;
    CpuTopology g_cpu;
    HWND g_deviceList = nullptr;
    HWND g_status = nullptr;
    HWND g_aboutWindow = nullptr;
    HWND g_bufferSlider = nullptr;
    HWND g_bufferValue = nullptr;
    HWND g_preampEdit = nullptr;
    HWND g_peqSummary = nullptr;
    HWND g_virtualBassSummary = nullptr;
    HWND g_virtualBassMasterEnable = nullptr;
    HWND g_virtualBassWindow = nullptr;
    HWND g_virtualBassEnabled = nullptr;
    HWND g_virtualBassMode = nullptr;
    HWND g_virtualBassAmount = nullptr;
    HWND g_virtualBassAmountValue = nullptr;
    HWND g_virtualBassCutoff = nullptr;
    HWND g_virtualBassSubsonic = nullptr;
    HWND g_virtualBassCustomLabel = nullptr;
    std::array<HWND, 3> g_virtualBassCustom{};
    HWND g_virtualBassLiveApply = nullptr;
    HWND g_virtualBassStatus = nullptr;
    HWND g_hardwareStatus = nullptr;

    void TogglePlaybackSelection(HWND window, int index);
    void QueueApoPlaybackApply();
    enum class StaticGainApplyMode
    {
        All,
        ReductionsOnly,
    };
    HRESULT ApplyStaticHardwareCompensation(
        StaticGainApplyMode mode = StaticGainApplyMode::All);
    HRESULT RestoreStaticHardwareCompensation();
    void WaitForApoSettingsGeneration(DWORD generation, DWORD timeoutMs);
    void NormalizeStoredPeqGainLimits();
    HWND g_peqWindow = nullptr;
    HWND g_peqEnabled = nullptr;
    HWND g_peqType = nullptr;
    HWND g_peqFrequency = nullptr;
    HWND g_peqGain = nullptr;
    HWND g_peqQ = nullptr;
    HWND g_peqLiveApply = nullptr;
    HWND g_peqDevice = nullptr;
    int g_peqGainZoomHover = 0;
    bool g_uiSmokeTest = false;
    bool g_uiPreview = false;
    bool g_mainUiPreview = false;
    bool g_mainUiSmoke = false;
    bool g_virtualBassUiSmoke = false;
    bool g_virtualBassUiPreview = false;
    bool g_forceDarkUiPreview = false;
    bool g_refreshingDeviceList = false;
    bool g_apoMode = false;
    NOTIFYICONDATAW g_trayIcon{};
    HICON g_ownedTrayIcon = nullptr;
    UINT g_taskbarCreatedMessage = 0;

    constexpr wchar_t kUiDpiProperty[] = L"HXAudio.UiDpi";
    constexpr wchar_t kUiFontRoleProperty[] = L"HXAudio.UiFontRole";
    constexpr wchar_t kUiHoverProperty[] = L"HXAudio.UiHover";
    constexpr wchar_t kUiLogicalWidthProperty[] = L"HXAudio.UiLogicalWidth";
    constexpr wchar_t kUiLogicalHeightProperty[] = L"HXAudio.UiLogicalHeight";
    constexpr wchar_t kUiToggleSwitchProperty[] = L"HXAudio.UiToggleSwitch";
    constexpr wchar_t kUiSurfaceAltProperty[] = L"HXAudio.UiSurfaceAlt";
    constexpr wchar_t kUiListHoverProperty[] = L"HXAudio.UiListHover";
    constexpr wchar_t kUiScrollHoverProperty[] = L"HXAudio.UiScrollHover";
    constexpr wchar_t kUiScrollDragProperty[] = L"HXAudio.UiScrollDrag";
    constexpr wchar_t kUiScrollOffsetProperty[] = L"HXAudio.UiScrollOffset";
    constexpr wchar_t kUiMixerScrollProperty[] = L"HXAudio.UiMixerScroll";
    constexpr wchar_t kUiSurfaceCacheProperty[] = L"HXAudio.UiSurfaceCache";
    constexpr wchar_t kUiAnimationProperty[] = L"HXAudio.UiAnimation";
    constexpr wchar_t kUiVolumeDragProperty[] = L"HXAudio.UiVolumeDrag";
    constexpr wchar_t kUiMainLayoutProperty[] = L"HXAudio.UiMainLayout";
    constexpr UINT_PTR kUiAnimationTimerId = 0x48a1;

    enum class UiFontRole : UINT_PTR
    {
        Body = 1,
        Caption,
        Section,
        Title
    };

    struct UiPalette
    {
        bool dark = false;
        COLORREF background = RGB(243, 243, 243);
        COLORREF surface = RGB(252, 252, 252);
        COLORREF surfaceAlt = RGB(247, 247, 247);
        COLORREF elevated = RGB(255, 255, 255);
        COLORREF border = RGB(224, 224, 224);
        COLORREF text = RGB(31, 31, 31);
        COLORREF muted = RGB(96, 96, 96);
        COLORREF accent = RGB(0, 103, 192);
        COLORREF accentHover = RGB(0, 85, 160);
        COLORREF graph = RGB(247, 250, 253);
        COLORREF grid = RGB(218, 225, 232);
        COLORREF success = RGB(16, 124, 65);
        COLORREF danger = RGB(196, 43, 28);
    };

    struct CachedUiFont
    {
        UINT dpi = 96;
        UiFontRole role = UiFontRole::Body;
        HFONT font = nullptr;
    };

    struct UiSurfaceCache
    {
        HDC canvas = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previousBitmap = nullptr;
        void* pixels = nullptr;
        int width = 0;
        int height = 0;
        bool valid = false;
    };

    struct UiAnimationState
    {
        float hover = 0.0f;
        float press = 0.0f;
        float toggle = 0.0f;
        float hoverTarget = 0.0f;
        float pressTarget = 0.0f;
        float toggleTarget = 0.0f;
        ULONGLONG lastTick = 0;
        bool initialized = false;
    };

    bool CopyCachedParentSurface(HWND child, HDC destination);
    HBITMAP CreateUiCanvasBitmap(int width, int height, void** pixels);
    void FinalizeUiCanvasAlpha(void* pixels, int width, int height);
    UiSurfaceCache* EnsureUiSurfaceCache(HWND window, HDC compatible,
        int width, int height);
    void DestroyUiSurfaceCache(HWND window);
    UiSurfaceCache* BeginUiControlSurface(HWND window, HDC destination,
        const RECT& client, HDC& canvas, HBRUSH fallback);
    void PresentUiControlSurface(HWND window, HDC destination,
        UiSurfaceCache* cache, HDC canvas, int width, int height);

    UiPalette g_ui;
    bool g_windows11 = false;
    std::vector<CachedUiFont> g_uiFonts;
    HBRUSH g_backgroundBrush = nullptr;
    HBRUSH g_surfaceBrush = nullptr;
    HBRUSH g_surfaceAltBrush = nullptr;
    std::vector<HICON> g_uiIcons;
    int g_peqHoveredBand = -1;
    POINT g_peqPointer{};
    POINT g_peqDragOffset{};
    bool g_peqPointerInGraph = false;
    bool g_virtualBassPointerInGraph = false;

    COLORREF BlendColor(COLORREF first, COLORREF second, double secondAmount)
    {
        const double amount = std::clamp(secondAmount, 0.0, 1.0);
        const auto channel = [&](BYTE a, BYTE b)
        {
            return static_cast<BYTE>(std::lround(a + (b - a) * amount));
        };
        return RGB(channel(GetRValue(first), GetRValue(second)),
            channel(GetGValue(first), GetGValue(second)),
            channel(GetBValue(first), GetBValue(second)));
    }

    Gdiplus::Color GdiColor(COLORREF color, BYTE alpha = 255)
    {
        return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
    }

    void AddRoundedPath(Gdiplus::GraphicsPath& path,
        const Gdiplus::RectF& rectangle, float radius)
    {
        const float diameter = std::max(0.0f, std::min(radius * 2.0f,
            std::min(rectangle.Width, rectangle.Height)));
        if (diameter <= 0.0f)
        {
            path.AddRectangle(rectangle);
            return;
        }
        path.AddArc(rectangle.X, rectangle.Y, diameter, diameter, 180.0f, 90.0f);
        path.AddArc(rectangle.GetRight() - diameter, rectangle.Y, diameter, diameter, 270.0f, 90.0f);
        path.AddArc(rectangle.GetRight() - diameter, rectangle.GetBottom() - diameter,
            diameter, diameter, 0.0f, 90.0f);
        path.AddArc(rectangle.X, rectangle.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
        path.CloseFigure();
    }

    void FillRounded(Gdiplus::Graphics& graphics, const Gdiplus::RectF& rectangle,
        float radius, const Gdiplus::Color& color)
    {
        Gdiplus::GraphicsPath path;
        AddRoundedPath(path, rectangle, radius);
        Gdiplus::SolidBrush brush(color);
        graphics.FillPath(&brush, &path);
    }

    void StrokeRounded(Gdiplus::Graphics& graphics, const Gdiplus::RectF& rectangle,
        float radius, const Gdiplus::Color& color, float width = 1.0f)
    {
        Gdiplus::GraphicsPath path;
        AddRoundedPath(path, rectangle, radius);
        Gdiplus::Pen pen(color, width);
        graphics.DrawPath(&pen, &path);
    }

    bool IsWindows11OrGreater()
    {
        using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto getVersion = ntdll ? reinterpret_cast<RtlGetVersionFunction>(
            GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
        OSVERSIONINFOW version{ sizeof(version) };
        return getVersion && getVersion(&version) == 0 && version.dwBuildNumber >= 22000;
    }

    bool SystemUsesDarkApps()
    {
        if (g_forceDarkUiPreview) return true;
        DWORD value = 1;
        DWORD size = sizeof(value);
        const LSTATUS result = RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
        return result == ERROR_SUCCESS && value == 0;
    }

    COLORREF SystemAccentColor()
    {
        DWORD colorization = 0;
        BOOL opaque = FALSE;
        if (SUCCEEDED(DwmGetColorizationColor(&colorization, &opaque)))
        {
            const COLORREF color = RGB((colorization >> 16) & 0xff,
                (colorization >> 8) & 0xff, colorization & 0xff);
            const int brightness = GetRValue(color) + GetGValue(color) + GetBValue(color);
            if (brightness > 90 && brightness < 690) return color;
        }
        return RGB(0, 103, 192);
    }

    void RefreshUiPalette()
    {
        g_ui.dark = SystemUsesDarkApps();
        g_ui.accent = SystemAccentColor();
        if (g_ui.dark)
        {
            g_ui.background = RGB(32, 32, 32);
            g_ui.surface = RGB(44, 44, 44);
            g_ui.surfaceAlt = RGB(51, 51, 51);
            g_ui.elevated = RGB(58, 58, 58);
            g_ui.border = RGB(70, 70, 70);
            g_ui.text = RGB(245, 245, 245);
            g_ui.muted = RGB(184, 184, 184);
            g_ui.graph = RGB(22, 27, 34);
            g_ui.grid = RGB(53, 62, 72);
            g_ui.accentHover = BlendColor(g_ui.accent, RGB(255, 255, 255), 0.18);
            g_ui.success = RGB(108, 203, 95);
            g_ui.danger = RGB(255, 153, 164);
        }
        else
        {
            g_ui.background = RGB(243, 243, 243);
            g_ui.surface = RGB(252, 252, 252);
            g_ui.surfaceAlt = RGB(247, 247, 247);
            g_ui.elevated = RGB(255, 255, 255);
            g_ui.border = RGB(224, 224, 224);
            g_ui.text = RGB(31, 31, 31);
            g_ui.muted = RGB(96, 96, 96);
            g_ui.graph = RGB(247, 250, 253);
            g_ui.grid = RGB(218, 225, 232);
            g_ui.accentHover = BlendColor(g_ui.accent, RGB(0, 0, 0), 0.16);
            g_ui.success = RGB(16, 124, 65);
            g_ui.danger = RGB(196, 43, 28);
        }
        if (g_backgroundBrush) DeleteObject(g_backgroundBrush);
        if (g_surfaceBrush) DeleteObject(g_surfaceBrush);
        if (g_surfaceAltBrush) DeleteObject(g_surfaceAltBrush);
        g_backgroundBrush = CreateSolidBrush(g_ui.background);
        g_surfaceBrush = CreateSolidBrush(g_ui.surface);
        g_surfaceAltBrush = CreateSolidBrush(g_ui.surfaceAlt);
    }

    UINT UiDpi(HWND window)
    {
        if (!window) return GetDpiForSystem();
        const UINT dpi = GetDpiForWindow(window);
        return dpi ? dpi : 96;
    }

    int UiScale(HWND window, int value)
    {
        return MulDiv(value, static_cast<int>(UiDpi(window)), 96);
    }

    bool UiAnimationsEnabled()
    {
        BOOL enabled = TRUE;
        return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0,
            &enabled, 0) == FALSE || enabled != FALSE;
    }

    UiAnimationState* EnsureUiAnimation(HWND window)
    {
        auto* state = reinterpret_cast<UiAnimationState*>(
            GetPropW(window, kUiAnimationProperty));
        if (!state)
        {
            state = new (std::nothrow) UiAnimationState{};
            if (!state) return nullptr;
            SetPropW(window, kUiAnimationProperty, state);
        }
        if (!state->initialized)
        {
            const LRESULT checked = SendMessageW(window, BM_GETCHECK, 0, 0);
            state->toggle = state->toggleTarget =
                checked == BST_CHECKED || checked == BST_INDETERMINATE ? 1.0f : 0.0f;
            state->initialized = true;
        }
        return state;
    }

    void StartUiAnimation(HWND window)
    {
        UiAnimationState* state = EnsureUiAnimation(window);
        if (!state) return;
        if (!UiAnimationsEnabled())
        {
            state->hover = state->hoverTarget;
            state->press = state->pressTarget;
            state->toggle = state->toggleTarget;
            KillTimer(window, kUiAnimationTimerId);
            InvalidateRect(window, nullptr, FALSE);
            return;
        }
        state->lastTick = GetTickCount64();
        SetTimer(window, kUiAnimationTimerId, 15, nullptr);
        InvalidateRect(window, nullptr, FALSE);
    }

    void SetUiHoverTarget(HWND window, bool value)
    {
        UiAnimationState* state = EnsureUiAnimation(window);
        if (!state) return;
        state->hoverTarget = value ? 1.0f : 0.0f;
        StartUiAnimation(window);
    }

    void SetUiPressTarget(HWND window, bool value)
    {
        UiAnimationState* state = EnsureUiAnimation(window);
        if (!state) return;
        state->pressTarget = value ? 1.0f : 0.0f;
        StartUiAnimation(window);
    }

    void SyncUiToggleTarget(HWND window)
    {
        UiAnimationState* state = EnsureUiAnimation(window);
        if (!state) return;
        const LRESULT checked = SendMessageW(window, BM_GETCHECK, 0, 0);
        state->toggleTarget =
            checked == BST_CHECKED || checked == BST_INDETERMINATE ? 1.0f : 0.0f;
        StartUiAnimation(window);
    }

    bool AdvanceUiAnimation(HWND window)
    {
        UiAnimationState* state = EnsureUiAnimation(window);
        if (!state) return false;
        const ULONGLONG now = GetTickCount64();
        const float elapsed = static_cast<float>(std::clamp<ULONGLONG>(
            state->lastTick ? now - state->lastTick : 15, 1, 40));
        state->lastTick = now;
        const float blend = 1.0f - std::exp(-elapsed / 42.0f);
        bool active = false;
        const auto approach = [&](float& value, float target)
        {
            value += (target - value) * blend;
            if (std::fabs(target - value) < 0.003f) value = target;
            else active = true;
        };
        approach(state->hover, state->hoverTarget);
        approach(state->press, state->pressTarget);
        approach(state->toggle, state->toggleTarget);
        InvalidateRect(window, nullptr, FALSE);
        if (!active) KillTimer(window, kUiAnimationTimerId);
        return active;
    }

    float UiHoverAmount(HWND window)
    {
        UiAnimationState* state = EnsureUiAnimation(window);
        return state ? state->hover :
            (GetPropW(window, kUiHoverProperty) ? 1.0f : 0.0f);
    }

    float UiPressAmount(HWND window)
    {
        UiAnimationState* state = EnsureUiAnimation(window);
        return state ? state->press : 0.0f;
    }

    float UiToggleAmount(HWND window, bool checked)
    {
        UiAnimationState* state = EnsureUiAnimation(window);
        return state ? state->toggle : (checked ? 1.0f : 0.0f);
    }

    void DestroyUiAnimation(HWND window)
    {
        KillTimer(window, kUiAnimationTimerId);
        delete reinterpret_cast<UiAnimationState*>(
            RemovePropW(window, kUiAnimationProperty));
    }

    HFONT UiFont(UINT dpi, UiFontRole role)
    {
        for (const auto& entry : g_uiFonts)
            if (entry.dpi == dpi && entry.role == role) return entry.font;

        int pixelHeight = 14;
        int weight = FW_NORMAL;
        const wchar_t* face = g_windows11 ? L"Segoe UI Variable Text" : L"Segoe UI";
        if (role == UiFontRole::Caption) pixelHeight = 12;
        else if (role == UiFontRole::Section)
        {
            pixelHeight = 18;
            weight = FW_SEMIBOLD;
        }
        else if (role == UiFontRole::Title)
        {
            pixelHeight = 28;
            weight = FW_SEMIBOLD;
            face = g_windows11 ? L"Segoe UI Variable Display" : L"Segoe UI";
        }
        HFONT font = CreateFontW(-MulDiv(pixelHeight, static_cast<int>(dpi), 96), 0, 0, 0,
            weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        g_uiFonts.push_back({ dpi, role, font });
        return font;
    }

    void DrawUiText(Gdiplus::Graphics& graphics, HDC dc, HWND window,
        const wchar_t* text, const Gdiplus::RectF& bounds, UiFontRole role, COLORREF color,
        Gdiplus::StringAlignment alignment = Gdiplus::StringAlignmentNear)
    {
        // ClearType assumes an opaque RGB target and leaves colored/duplicated
        // fringes when composited over a transparent Win11 system backdrop.
        // Grayscale antialiasing keeps text crisp in both Mica themes.
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        LOGFONTW logFont{};
        GetObjectW(UiFont(UiDpi(window), role), sizeof(logFont), &logFont);
        Gdiplus::Font font(dc, &logFont);
        Gdiplus::StringFormat format;
        format.SetAlignment(alignment);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        Gdiplus::SolidBrush brush(GdiColor(color));
        graphics.DrawString(text, -1, &font, bounds, &format, &brush);
    }

    void DrawUiWrappedText(Gdiplus::Graphics& graphics, HDC dc, HWND window,
        const wchar_t* text, const Gdiplus::RectF& bounds, UiFontRole role,
        COLORREF color)
    {
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        LOGFONTW logFont{};
        GetObjectW(UiFont(UiDpi(window), role), sizeof(logFont), &logFont);
        Gdiplus::Font font(dc, &logFont);
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetTrimming(Gdiplus::StringTrimmingEllipsisWord);
        Gdiplus::SolidBrush brush(GdiColor(color));
        graphics.DrawString(text, -1, &font, bounds, &format, &brush);
    }

    void SetUiFontRole(HWND control, UiFontRole role)
    {
        if (!control) return;
        SetPropW(control, kUiFontRoleProperty,
            reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(role)));
        SendMessageW(control, WM_SETFONT,
            reinterpret_cast<WPARAM>(UiFont(UiDpi(control), role)), TRUE);
    }

    LRESULT CALLBACK ModernButtonSubclass(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam, UINT_PTR, DWORD_PTR)
    {
        if (message == WM_TIMER && wParam == kUiAnimationTimerId)
        {
            AdvanceUiAnimation(window);
            return 0;
        }
        if (message == WM_MOUSEMOVE && !GetPropW(window, kUiHoverProperty))
        {
            SetPropW(window, kUiHoverProperty, reinterpret_cast<HANDLE>(1));
            TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, window, 0 };
            TrackMouseEvent(&tracking);
            SetUiHoverTarget(window, true);
        }
        else if (message == WM_MOUSELEAVE)
        {
            RemovePropW(window, kUiHoverProperty);
            SetUiHoverTarget(window, false);
            SetUiPressTarget(window, false);
        }
        else if (message == WM_LBUTTONDOWN ||
            (message == WM_KEYDOWN && wParam == VK_SPACE))
        {
            SetUiPressTarget(window, true);
        }
        else if (message == WM_LBUTTONUP || message == WM_CAPTURECHANGED ||
            (message == WM_KEYUP && wParam == VK_SPACE))
        {
            SetUiPressTarget(window, false);
        }
        else if (message == WM_SETFOCUS || message == WM_KILLFOCUS ||
            message == WM_ENABLE || message == WM_THEMECHANGED)
        {
            InvalidateRect(window, nullptr, FALSE);
        }
        else if (message == WM_NCDESTROY)
        {
            RemovePropW(window, kUiHoverProperty);
            DestroyUiAnimation(window);
            DestroyUiSurfaceCache(window);
            RemoveWindowSubclass(window, ModernButtonSubclass, 1);
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    void PaintModernCheckBox(HWND window, HDC destination)
    {
        RECT client{};
        GetClientRect(window, &client);
        const bool onAltSurface = GetPropW(window, kUiSurfaceAltProperty) != nullptr;
        HDC canvas = destination;
        UiSurfaceCache* cache = BeginUiControlSurface(window, destination,
            client, canvas, onAltSurface ? g_surfaceAltBrush : g_surfaceBrush);

        const bool enabled = IsWindowEnabled(window) != FALSE;
        const float hoverAmount = UiHoverAmount(window);
        const bool focused = GetFocus() == window;
        const LRESULT checkState = SendMessageW(window, BM_GETCHECK, 0, 0);
        const bool checked = checkState == BST_CHECKED || checkState == BST_INDETERMINATE;
        const bool toggleSwitch = GetPropW(window, kUiToggleSwitchProperty) != nullptr;
        const float scale = static_cast<float>(UiDpi(window)) / 96.0f;

        const float toggleAmount = UiToggleAmount(window, checked);
        Gdiplus::Graphics graphics(canvas);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        if (toggleSwitch)
        {
            const float trackWidth = 40.0f * scale;
            const float trackHeight = 20.0f * scale;
            const float trackY = std::max(1.0f * scale,
                (static_cast<float>(client.bottom) - trackHeight) * 0.5f);
            const Gdiplus::RectF track(1.0f * scale, trackY, trackWidth, trackHeight);
            COLORREF trackFill = BlendColor(g_ui.surfaceAlt, g_ui.accent,
                toggleAmount);
            COLORREF trackOutline = BlendColor(g_ui.muted, g_ui.accent,
                toggleAmount);
            if (!enabled)
            {
                trackFill = BlendColor(trackFill,
                    onAltSurface ? g_ui.surfaceAlt : g_ui.surface, 0.55);
                trackOutline = g_ui.border;
            }
            else if (hoverAmount > 0.0f)
            {
                const COLORREF hoverFill = checked ? g_ui.accentHover :
                    BlendColor(g_ui.surfaceAlt, g_ui.accent, g_ui.dark ? 0.22 : 0.12);
                trackFill = BlendColor(trackFill, hoverFill, hoverAmount);
                trackOutline = BlendColor(trackOutline, g_ui.accent, hoverAmount);
            }
            FillRounded(graphics, track, trackHeight * 0.5f, GdiColor(trackFill));
            StrokeRounded(graphics, track, trackHeight * 0.5f,
                GdiColor(focused ? g_ui.accent : trackOutline), focused ? 2.0f : 1.2f);

            const float knobSize = 14.0f * scale;
            const float knobLeft = track.X + 3.0f * scale;
            const float knobRight = track.GetRight() - knobSize - 3.0f * scale;
            const float knobX = knobLeft + (knobRight - knobLeft) * toggleAmount;
            const float knobY = track.Y + (track.Height - knobSize) * 0.5f;
            const COLORREF knobColor = BlendColor(
                enabled ? g_ui.muted : g_ui.border,
                RGB(255, 255, 255), toggleAmount);
            Gdiplus::SolidBrush knob(GdiColor(knobColor));
            graphics.FillEllipse(&knob, knobX, knobY, knobSize, knobSize);

            wchar_t label[256]{};
            GetWindowTextW(window, label, ARRAYSIZE(label));
            DrawUiText(graphics, canvas, window, label,
                Gdiplus::RectF(50.0f * scale, 0.0f,
                    std::max(1.0f, static_cast<float>(client.right) - 52.0f * scale),
                    static_cast<float>(client.bottom)),
                UiFontRole::Body, enabled ? g_ui.text : g_ui.muted);
            graphics.Flush(Gdiplus::FlushIntentionSync);
            PresentUiControlSurface(window, destination, cache, canvas,
                std::max<LONG>(client.right, 1), std::max<LONG>(client.bottom, 1));
            return;
        }

        const float boxSize = 18.0f * scale;
        const float boxX = 1.0f * scale;
        const float boxY = std::max(1.0f * scale,
            (static_cast<float>(client.bottom - client.top) - boxSize) * 0.5f);

        Gdiplus::RectF box(boxX, boxY, boxSize, boxSize);
        COLORREF fill = checked ? g_ui.accent : g_ui.surfaceAlt;
        COLORREF outline = checked ? g_ui.accent : g_ui.muted;
        if (!enabled)
        {
            fill = BlendColor(fill, g_ui.surface, 0.50);
            outline = BlendColor(g_ui.muted, g_ui.surface, 0.38);
        }
        else if (hoverAmount > 0.0f && !checked)
        {
            fill = BlendColor(fill, BlendColor(g_ui.surfaceAlt, g_ui.accent,
                g_ui.dark ? 0.20 : 0.10), hoverAmount);
            outline = BlendColor(outline, g_ui.accent, hoverAmount);
        }
        FillRounded(graphics, box, 4.0f * scale, GdiColor(fill));
        StrokeRounded(graphics, box, 4.0f * scale,
            GdiColor(focused ? g_ui.accent : outline), focused ? 2.0f : 1.3f);

        if (checked)
        {
            Gdiplus::Pen checkPen(GdiColor(enabled ? RGB(255, 255, 255) : g_ui.muted),
                2.0f * scale);
            checkPen.SetStartCap(Gdiplus::LineCapRound);
            checkPen.SetEndCap(Gdiplus::LineCapRound);
            checkPen.SetLineJoin(Gdiplus::LineJoinRound);
            Gdiplus::PointF points[] = {
                { box.X + 4.0f * scale, box.Y + 9.0f * scale },
                { box.X + 7.5f * scale, box.Y + 12.5f * scale },
                { box.X + 14.0f * scale, box.Y + 5.5f * scale }
            };
            graphics.DrawLines(&checkPen, points, ARRAYSIZE(points));
        }

        wchar_t label[256]{};
        GetWindowTextW(window, label, ARRAYSIZE(label));
        DrawUiText(graphics, canvas, window, label,
            Gdiplus::RectF(28.0f * scale, 0.0f,
                std::max(1.0f, static_cast<float>(client.right) - 30.0f * scale),
                static_cast<float>(client.bottom)),
            UiFontRole::Body, enabled ? g_ui.text : g_ui.muted);
        graphics.Flush(Gdiplus::FlushIntentionSync);
        PresentUiControlSurface(window, destination, cache, canvas,
            std::max<LONG>(client.right, 1), std::max<LONG>(client.bottom, 1));
    }

    LRESULT CALLBACK ModernCheckBoxSubclass(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam, UINT_PTR, DWORD_PTR)
    {
        if (message == WM_TIMER && wParam == kUiAnimationTimerId)
        {
            AdvanceUiAnimation(window);
            return 0;
        }
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            PaintModernCheckBox(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_PRINTCLIENT)
        {
            PaintModernCheckBox(window, reinterpret_cast<HDC>(wParam));
            return 0;
        }
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_MOUSEMOVE && !GetPropW(window, kUiHoverProperty))
        {
            SetPropW(window, kUiHoverProperty, reinterpret_cast<HANDLE>(1));
            TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, window, 0 };
            TrackMouseEvent(&tracking);
            SetUiHoverTarget(window, true);
        }
        else if (message == WM_MOUSELEAVE)
        {
            RemovePropW(window, kUiHoverProperty);
            SetUiHoverTarget(window, false);
            SetUiPressTarget(window, false);
        }
        else if (message == BM_SETCHECK)
        {
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            SyncUiToggleTarget(window);
            return result;
        }
        else if (message == WM_LBUTTONDOWN ||
            (message == WM_KEYDOWN && wParam == VK_SPACE))
        {
            SetUiPressTarget(window, true);
        }
        else if (message == WM_LBUTTONUP ||
            (message == WM_KEYUP && wParam == VK_SPACE))
        {
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            SetUiPressTarget(window, false);
            SyncUiToggleTarget(window);
            return result;
        }
        else if (message == WM_SETFOCUS || message == WM_KILLFOCUS ||
            message == WM_ENABLE || message == WM_THEMECHANGED ||
            message == WM_KEYUP)
        {
            InvalidateRect(window, nullptr, FALSE);
        }
        else if (message == WM_NCDESTROY)
        {
            RemovePropW(window, kUiHoverProperty);
            DestroyUiAnimation(window);
            DestroyUiSurfaceCache(window);
            RemoveWindowSubclass(window, ModernCheckBoxSubclass, 2);
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    void ApplyRoundedControlRegion(HWND control, int logicalRadius)
    {
        RECT bounds{};
        GetClientRect(control, &bounds);
        const int radius = UiScale(control, logicalRadius);
        HRGN region = CreateRoundRectRgn(0, 0, bounds.right + 1, bounds.bottom + 1,
            radius * 2, radius * 2);
        if (region && !SetWindowRgn(control, region, TRUE)) DeleteObject(region);
    }

    void PaintModernComboBox(HWND window, HDC destination)
    {
        RECT client{};
        GetClientRect(window, &client);
        HDC canvas = destination;
        UiSurfaceCache* cache = BeginUiControlSurface(window, destination,
            client, canvas, g_surfaceBrush);
        const bool enabled = IsWindowEnabled(window) != FALSE;
        const float hoverAmount = UiHoverAmount(window);
        const bool active = GetFocus() == window ||
            SendMessageW(window, CB_GETDROPPEDSTATE, 0, 0) != FALSE;
        const float scale = static_cast<float>(UiDpi(window)) / 96.0f;

        Gdiplus::Graphics graphics(canvas);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::RectF body(0.6f * scale, 0.6f * scale,
            std::max(1.0f, static_cast<float>(client.right) - 1.2f * scale),
            std::max(1.0f, static_cast<float>(client.bottom) - 1.2f * scale));
        const COLORREF fill = enabled
            ? BlendColor(g_ui.surfaceAlt,
                BlendColor(g_ui.surfaceAlt, g_ui.text, g_ui.dark ? 0.06 : 0.025),
                hoverAmount)
            : BlendColor(g_ui.surfaceAlt, g_ui.surface, 0.45);
        FillRounded(graphics, body, 6.0f * scale, GdiColor(fill));
        StrokeRounded(graphics, body, 6.0f * scale,
            GdiColor(active ? g_ui.accent : g_ui.border), active ? 2.0f : 1.0f);

        wchar_t label[512]{};
        const LRESULT selection = SendMessageW(window, CB_GETCURSEL, 0, 0);
        if (selection >= 0)
            SendMessageW(window, CB_GETLBTEXT, selection, reinterpret_cast<LPARAM>(label));
        DrawUiText(graphics, canvas, window, label,
            Gdiplus::RectF(11.0f * scale, 0.0f,
                std::max(1.0f, static_cast<float>(client.right) - 48.0f * scale),
                static_cast<float>(client.bottom)), UiFontRole::Body,
            enabled ? g_ui.text : g_ui.muted);

        const float arrowX = static_cast<float>(client.right) - 17.0f * scale;
        const float arrowY = static_cast<float>(client.bottom) * 0.5f;
        Gdiplus::Pen arrow(GdiColor(enabled ? g_ui.text : g_ui.muted), 1.5f * scale);
        arrow.SetStartCap(Gdiplus::LineCapRound);
        arrow.SetEndCap(Gdiplus::LineCapRound);
        graphics.DrawLine(&arrow, arrowX - 4.0f * scale, arrowY - 2.0f * scale,
            arrowX, arrowY + 2.0f * scale);
        graphics.DrawLine(&arrow, arrowX, arrowY + 2.0f * scale,
            arrowX + 4.0f * scale, arrowY - 2.0f * scale);
        graphics.Flush(Gdiplus::FlushIntentionSync);
        PresentUiControlSurface(window, destination, cache, canvas,
            std::max<LONG>(client.right, 1), std::max<LONG>(client.bottom, 1));
    }

    LRESULT CALLBACK ModernComboSubclass(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam, UINT_PTR, DWORD_PTR)
    {
        if (message == WM_TIMER && wParam == kUiAnimationTimerId)
        {
            AdvanceUiAnimation(window);
            return 0;
        }
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            PaintModernComboBox(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_PRINTCLIENT)
        {
            PaintModernComboBox(window, reinterpret_cast<HDC>(wParam));
            return 0;
        }
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_MOUSEMOVE && !GetPropW(window, kUiHoverProperty))
        {
            SetPropW(window, kUiHoverProperty, reinterpret_cast<HANDLE>(1));
            TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, window, 0 };
            TrackMouseEvent(&tracking);
            SetUiHoverTarget(window, true);
        }
        else if (message == WM_MOUSELEAVE)
        {
            RemovePropW(window, kUiHoverProperty);
            SetUiHoverTarget(window, false);
        }
        else if (message == CB_SETCURSEL)
        {
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            InvalidateRect(window, nullptr, FALSE);
            return result;
        }
        else if (message == WM_SETFOCUS || message == WM_KILLFOCUS ||
            message == WM_ENABLE || message == WM_THEMECHANGED ||
            message == WM_LBUTTONUP || message == WM_KEYUP)
        {
            InvalidateRect(window, nullptr, FALSE);
        }
        else if (message == WM_NCDESTROY)
        {
            RemovePropW(window, kUiHoverProperty);
            DestroyUiAnimation(window);
            DestroyUiSurfaceCache(window);
            RemoveWindowSubclass(window, ModernComboSubclass, 3);
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    void PaintModernEdit(HWND window, HDC destination)
    {
        RECT client{};
        GetClientRect(window, &client);
        const int width = std::max<LONG>(client.right, 1);
        const int height = std::max<LONG>(client.bottom, 1);
        HDC canvas = destination;
        UiSurfaceCache* cache = BeginUiControlSurface(window, destination,
            client, canvas, g_surfaceBrush);

        const bool enabled = IsWindowEnabled(window) != FALSE;
        const bool focused = GetFocus() == window;
        const float scale = static_cast<float>(UiDpi(window)) / 96.0f;
        Gdiplus::Graphics graphics(canvas);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const Gdiplus::RectF field(0.8f * scale, 0.8f * scale,
            std::max(1.0f, static_cast<float>(width) - 1.6f * scale),
            std::max(1.0f, static_cast<float>(height) - 1.6f * scale));
        FillRounded(graphics, field, 5.0f * scale,
            GdiColor(enabled ? g_ui.elevated : g_ui.surfaceAlt));
        StrokeRounded(graphics, field, 5.0f * scale,
            GdiColor(focused ? g_ui.accent : g_ui.border), focused ? 2.0f : 1.0f);

        wchar_t text[512]{};
        GetWindowTextW(window, text, ARRAYSIZE(text));
        const int textLength = static_cast<int>(wcslen(text));
        HFONT font = UiFont(UiDpi(window), UiFontRole::Body);
        HGDIOBJ oldFont = SelectObject(canvas, font);
        SetBkMode(canvas, TRANSPARENT);
        SetTextColor(canvas, enabled ? g_ui.text : g_ui.muted);
        TEXTMETRICW metrics{};
        GetTextMetricsW(canvas, &metrics);
        const int left = UiScale(window, 10);
        const int baseline = std::max<int>(0, (height - metrics.tmHeight) / 2);
        DrawUiText(graphics, canvas, window, text,
            Gdiplus::RectF(static_cast<float>(left), 0.0f,
                static_cast<float>(std::max(1, width - left - UiScale(window, 8))),
                static_cast<float>(height)), UiFontRole::Body,
            enabled ? g_ui.text : g_ui.muted);

        DWORD selectionStart = 0;
        DWORD selectionEnd = 0;
        SendMessageW(window, EM_GETSEL,
            reinterpret_cast<WPARAM>(&selectionStart),
            reinterpret_cast<LPARAM>(&selectionEnd));
        selectionStart = std::min<DWORD>(selectionStart, textLength);
        selectionEnd = std::min<DWORD>(selectionEnd, textLength);
        if (focused && selectionEnd > selectionStart)
        {
            SIZE prefix{};
            SIZE selected{};
            GetTextExtentPoint32W(canvas, text, selectionStart, &prefix);
            GetTextExtentPoint32W(canvas, text + selectionStart,
                selectionEnd - selectionStart, &selected);
            const int selectionLeft = left + prefix.cx;
            Gdiplus::SolidBrush accent(GdiColor(g_ui.accent));
            graphics.FillRectangle(&accent, static_cast<float>(selectionLeft),
                static_cast<float>(baseline), static_cast<float>(selected.cx),
                static_cast<float>(metrics.tmHeight));
            const std::wstring selectedText(text + selectionStart,
                text + selectionEnd);
            DrawUiText(graphics, canvas, window, selectedText.c_str(),
                Gdiplus::RectF(static_cast<float>(selectionLeft), 0.0f,
                    static_cast<float>(std::max(1L, selected.cx + metrics.tmOverhang)),
                    static_cast<float>(height)), UiFontRole::Body, RGB(255, 255, 255));
        }

        graphics.Flush(Gdiplus::FlushIntentionSync);
        SelectObject(canvas, oldFont);
        PresentUiControlSurface(window, destination, cache, canvas, width, height);
    }

    LRESULT CALLBACK ModernEditSubclass(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam, UINT_PTR, DWORD_PTR)
    {
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            PaintModernEdit(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_PRINTCLIENT)
        {
            PaintModernEdit(window, reinterpret_cast<HDC>(wParam));
            return 0;
        }
        if (message == WM_ERASEBKGND) return 1;
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        if (message == WM_SETTEXT || message == WM_SETFONT || message == WM_SETFOCUS ||
            message == WM_KILLFOCUS || message == WM_ENABLE || message == WM_THEMECHANGED ||
            message == WM_CHAR || message == WM_KEYDOWN || message == WM_KEYUP ||
            message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == EM_SETSEL)
            InvalidateRect(window, nullptr, FALSE);
        else if (message == WM_NCDESTROY)
        {
            DestroyUiSurfaceCache(window);
            RemoveWindowSubclass(window, ModernEditSubclass, 4);
        }
        return result;
    }

    void PaintModernSlider(HWND window, HDC destination)
    {
        RECT client{};
        GetClientRect(window, &client);
        const int width = std::max<LONG>(client.right, 1);
        const int height = std::max<LONG>(client.bottom, 1);
        HDC canvas = destination;
        UiSurfaceCache* cache = BeginUiControlSurface(window, destination,
            client, canvas, g_surfaceBrush);

        const bool enabled = IsWindowEnabled(window) != FALSE;
        const float hoverAmount = UiHoverAmount(window);
        const float pressAmount = UiPressAmount(window);
        const bool focused = GetFocus() == window;
        const LRESULT minimum = SendMessageW(window, TBM_GETRANGEMIN, 0, 0);
        const LRESULT maximum = SendMessageW(window, TBM_GETRANGEMAX, 0, 0);
        const LRESULT position = SendMessageW(window, TBM_GETPOS, 0, 0);
        const float scale = static_cast<float>(UiDpi(window)) / 96.0f;
        const float left = 12.0f * scale;
        const float right = std::max(left + 1.0f, static_cast<float>(client.right) - 12.0f * scale);
        const float centerY = static_cast<float>(client.bottom) * 0.46f;
        const double fraction = maximum > minimum
            ? std::clamp(static_cast<double>(position - minimum) /
                static_cast<double>(maximum - minimum), 0.0, 1.0) : 0.0;
        const float thumbX = left + static_cast<float>(fraction) * (right - left);

        Gdiplus::Graphics graphics(canvas);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        FillRounded(graphics,
            Gdiplus::RectF(left, centerY - 2.0f * scale, right - left, 4.0f * scale),
            2.0f * scale, GdiColor(enabled ? g_ui.border :
                BlendColor(g_ui.border, g_ui.surface, 0.45)));
        if (thumbX > left)
        {
            FillRounded(graphics,
                Gdiplus::RectF(left, centerY - 2.0f * scale, thumbX - left, 4.0f * scale),
                2.0f * scale, GdiColor(enabled ? g_ui.accent : g_ui.muted));
        }

        const LRESULT ticks = std::max<LRESULT>(maximum - minimum, 0);
        if (ticks > 0 && ticks <= 20)
        {
            Gdiplus::Pen tickPen(GdiColor(enabled ? g_ui.muted : g_ui.border), 1.0f * scale);
            for (LRESULT tick = 0; tick <= ticks; ++tick)
            {
                const float x = left + static_cast<float>(tick) / ticks * (right - left);
                graphics.DrawLine(&tickPen, x, centerY + 9.0f * scale,
                    x, centerY + 12.0f * scale);
            }
        }

        const float radius = (7.0f + hoverAmount + 0.55f * pressAmount) * scale;
        const float haloAmount = std::max(hoverAmount,
            focused ? 0.65f : 0.0f);
        Gdiplus::SolidBrush halo(GdiColor(g_ui.accent,
            static_cast<BYTE>(std::lround(48.0f * haloAmount))));
        if (haloAmount > 0.001f)
            graphics.FillEllipse(&halo, thumbX - radius - 4.0f * scale,
                centerY - radius - 4.0f * scale,
                (radius + 4.0f * scale) * 2.0f, (radius + 4.0f * scale) * 2.0f);
        Gdiplus::SolidBrush thumb(GdiColor(enabled ? g_ui.accent : g_ui.muted));
        graphics.FillEllipse(&thumb, thumbX - radius, centerY - radius,
            radius * 2.0f, radius * 2.0f);
        Gdiplus::Pen thumbBorder(GdiColor(g_ui.elevated), 2.0f * scale);
        graphics.DrawEllipse(&thumbBorder, thumbX - radius, centerY - radius,
            radius * 2.0f, radius * 2.0f);

        graphics.Flush(Gdiplus::FlushIntentionSync);
        PresentUiControlSurface(window, destination, cache, canvas, width, height);
    }

    LRESULT CALLBACK ModernSliderSubclass(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam, UINT_PTR, DWORD_PTR)
    {
        if (message == WM_TIMER && wParam == kUiAnimationTimerId)
        {
            AdvanceUiAnimation(window);
            return 0;
        }
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            PaintModernSlider(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_PRINTCLIENT)
        {
            PaintModernSlider(window, reinterpret_cast<HDC>(wParam));
            return 0;
        }
        if (message == WM_ERASEBKGND) return 1;
        if (message == TBM_SETPOS || message == TBM_SETRANGE ||
            message == TBM_SETRANGEMIN || message == TBM_SETRANGEMAX)
        {
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            InvalidateRect(window, nullptr, FALSE);
            return result;
        }
        if (message == WM_MOUSEMOVE && !GetPropW(window, kUiHoverProperty))
        {
            SetPropW(window, kUiHoverProperty, reinterpret_cast<HANDLE>(1));
            TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, window, 0 };
            TrackMouseEvent(&tracking);
            SetUiHoverTarget(window, true);
        }
        else if (message == WM_MOUSELEAVE)
        {
            RemovePropW(window, kUiHoverProperty);
            SetUiHoverTarget(window, false);
            SetUiPressTarget(window, false);
        }
        else if (message == WM_LBUTTONDOWN ||
            (message == WM_KEYDOWN && (wParam == VK_LEFT || wParam == VK_RIGHT ||
                wParam == VK_HOME || wParam == VK_END)))
        {
            SetUiPressTarget(window, true);
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            InvalidateRect(window, nullptr, FALSE);
            return result;
        }
        else if (message == WM_LBUTTONUP || message == WM_CAPTURECHANGED ||
            message == WM_KEYUP)
        {
            SetUiPressTarget(window, false);
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            InvalidateRect(window, nullptr, FALSE);
            return result;
        }
        else if (message == WM_SETFOCUS || message == WM_KILLFOCUS ||
            message == WM_ENABLE)
        {
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            InvalidateRect(window, nullptr, FALSE);
            return result;
        }
        else if (message == WM_NCDESTROY)
        {
            RemovePropW(window, kUiHoverProperty);
            DestroyUiAnimation(window);
            DestroyUiSurfaceCache(window);
            RemoveWindowSubclass(window, ModernSliderSubclass, 5);
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    struct EndpointVolumeRequest
    {
        std::wstring id;
        float volume = 1.0f;
        bool muted = false;
        bool setVolume = false;
        bool setMute = false;
    };

    struct EndpointVolumeSnapshot
    {
        std::wstring id;
        float volume = 1.0f;
        bool muted = false;
        bool valid = false;
    };

    SRWLOCK g_endpointVolumeLock = SRWLOCK_INIT;
    std::vector<EndpointVolumeRequest> g_endpointVolumeRequests;
    std::vector<std::wstring> g_endpointVolumeIds;
    std::vector<EndpointVolumeSnapshot> g_endpointVolumeSnapshots;
    HANDLE g_endpointVolumeEvent = nullptr;
    HANDLE g_endpointVolumeStopEvent = nullptr;
    HANDLE g_endpointVolumeThread = nullptr;
    HWND g_endpointVolumeNotifyWindow = nullptr;

    HRESULT OpenEndpointVolume(IMMDeviceEnumerator* enumerator,
        const std::wstring& id, ComPtr<IAudioEndpointVolume>& endpointVolume)
    {
        if (!enumerator || id.empty()) return E_INVALIDARG;
        ComPtr<IMMDevice> device;
        RETURN_IF_FAILED(enumerator->GetDevice(id.c_str(), &device));
        return device->Activate(__uuidof(IAudioEndpointVolume),
            CLSCTX_ALL, nullptr, &endpointVolume);
    }

    DWORD WINAPI EndpointVolumeWorker(void*)
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IMMDeviceEnumerator> enumerator;
        const HRESULT enumeratorResult = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator));
        HANDLE events[] = { g_endpointVolumeStopEvent, g_endpointVolumeEvent };
        while (SUCCEEDED(enumeratorResult))
        {
            const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(events),
                events, FALSE, 750);
            if (wait == WAIT_OBJECT_0) break;
            if (wait != WAIT_OBJECT_0 + 1 && wait != WAIT_TIMEOUT) break;

            std::vector<EndpointVolumeRequest> requests;
            std::vector<std::wstring> ids;
            AcquireSRWLockExclusive(&g_endpointVolumeLock);
            requests.swap(g_endpointVolumeRequests);
            ids = g_endpointVolumeIds;
            ReleaseSRWLockExclusive(&g_endpointVolumeLock);

            for (const auto& request : requests)
            {
                ComPtr<IAudioEndpointVolume> volume;
                if (FAILED(OpenEndpointVolume(enumerator.Get(),
                    request.id, volume))) continue;
                if (request.setVolume)
                    volume->SetMasterVolumeLevelScalar(
                        std::clamp(request.volume, 0.0f, 1.0f), nullptr);
                if (request.setMute)
                    volume->SetMute(request.muted ? TRUE : FALSE, nullptr);
            }

            std::vector<EndpointVolumeSnapshot> snapshots;
            snapshots.reserve(ids.size());
            for (const auto& id : ids)
            {
                EndpointVolumeSnapshot snapshot;
                snapshot.id = id;
                ComPtr<IAudioEndpointVolume> volume;
                BOOL muted = FALSE;
                if (SUCCEEDED(OpenEndpointVolume(enumerator.Get(), id, volume)) &&
                    SUCCEEDED(volume->GetMasterVolumeLevelScalar(&snapshot.volume)) &&
                    SUCCEEDED(volume->GetMute(&muted)))
                {
                    snapshot.volume = std::clamp(snapshot.volume, 0.0f, 1.0f);
                    snapshot.muted = muted != FALSE;
                    snapshot.valid = true;
                }
                snapshots.push_back(std::move(snapshot));
            }

            HWND notify = nullptr;
            AcquireSRWLockExclusive(&g_endpointVolumeLock);
            g_endpointVolumeSnapshots = std::move(snapshots);
            notify = g_endpointVolumeNotifyWindow;
            ReleaseSRWLockExclusive(&g_endpointVolumeLock);
            if (notify) PostMessageW(notify,
                kDeviceVolumesUpdatedMessage, 0, 0);
        }
        if (SUCCEEDED(co)) CoUninitialize();
        return SUCCEEDED(enumeratorResult)
            ? ERROR_SUCCESS : static_cast<DWORD>(enumeratorResult);
    }

    bool EnsureEndpointVolumeWorker(HWND notifyWindow)
    {
        AcquireSRWLockExclusive(&g_endpointVolumeLock);
        g_endpointVolumeNotifyWindow = notifyWindow;
        ReleaseSRWLockExclusive(&g_endpointVolumeLock);
        if (g_endpointVolumeThread) return true;
        g_endpointVolumeStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_endpointVolumeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_endpointVolumeStopEvent || !g_endpointVolumeEvent) return false;
        g_endpointVolumeThread = CreateThread(nullptr, 0,
            EndpointVolumeWorker, nullptr, 0, nullptr);
        return g_endpointVolumeThread != nullptr;
    }

    void QueueEndpointVolumeRefresh(HWND notifyWindow)
    {
        if (!EnsureEndpointVolumeWorker(notifyWindow)) return;
        std::vector<std::wstring> ids;
        ids.reserve(g_devices.size());
        for (const auto& device : g_devices) ids.push_back(device.id);
        AcquireSRWLockExclusive(&g_endpointVolumeLock);
        g_endpointVolumeIds = std::move(ids);
        ReleaseSRWLockExclusive(&g_endpointVolumeLock);
        SetEvent(g_endpointVolumeEvent);
    }

    void QueueEndpointVolumeChange(HWND notifyWindow, const std::wstring& id,
        float volume, bool setVolume, bool muted, bool setMute)
    {
        if (id.empty() || !EnsureEndpointVolumeWorker(notifyWindow)) return;
        AcquireSRWLockExclusive(&g_endpointVolumeLock);
        auto request = std::find_if(g_endpointVolumeRequests.begin(),
            g_endpointVolumeRequests.end(),
            [&](const EndpointVolumeRequest& item) { return item.id == id; });
        if (request == g_endpointVolumeRequests.end())
        {
            g_endpointVolumeRequests.push_back({ id });
            request = std::prev(g_endpointVolumeRequests.end());
        }
        if (setVolume)
        {
            request->volume = std::clamp(volume, 0.0f, 1.0f);
            request->setVolume = true;
        }
        if (setMute)
        {
            request->muted = muted;
            request->setMute = true;
        }
        ReleaseSRWLockExclusive(&g_endpointVolumeLock);
        SetEvent(g_endpointVolumeEvent);
    }

    void ShutdownEndpointVolumeWorker()
    {
        AcquireSRWLockExclusive(&g_endpointVolumeLock);
        g_endpointVolumeNotifyWindow = nullptr;
        ReleaseSRWLockExclusive(&g_endpointVolumeLock);
        if (!g_endpointVolumeThread) return;
        SetEvent(g_endpointVolumeStopEvent);
        SetEvent(g_endpointVolumeEvent);
        const bool stopped = WaitForSingleObject(
            g_endpointVolumeThread, 1500) == WAIT_OBJECT_0;
        CloseHandle(g_endpointVolumeThread);
        g_endpointVolumeThread = nullptr;
        if (stopped)
        {
            CloseHandle(g_endpointVolumeEvent);
            CloseHandle(g_endpointVolumeStopEvent);
            g_endpointVolumeEvent = nullptr;
            g_endpointVolumeStopEvent = nullptr;
        }
    }

    RECT MixerChannelRect(HWND window, int index);

    void ApplyEndpointVolumeSnapshots(HWND window)
    {
        std::vector<EndpointVolumeSnapshot> snapshots;
        AcquireSRWLockShared(&g_endpointVolumeLock);
        snapshots = g_endpointVolumeSnapshots;
        ReleaseSRWLockShared(&g_endpointVolumeLock);
        RECT client{};
        GetClientRect(window, &client);
        const int activeDrag = static_cast<int>(reinterpret_cast<INT_PTR>(
            GetPropW(window, kUiVolumeDragProperty))) - 1;
        for (size_t index = 0; index < g_devices.size(); ++index)
        {
            // A worker snapshot can be one pointer event behind the UI. Never
            // overwrite the thumb while the user owns that fader; the final
            // endpoint value is refreshed after capture is released.
            if (static_cast<int>(index) == activeDrag) continue;
            const auto snapshot = std::find_if(snapshots.begin(), snapshots.end(),
                [&](const EndpointVolumeSnapshot& item)
                { return item.id == g_devices[index].id; });
            if (snapshot == snapshots.end() || !snapshot->valid) continue;
            DeviceInfo& device = g_devices[index];
            const bool changed = !device.volumeKnown ||
                std::fabs(device.volume - snapshot->volume) > 0.002f ||
                device.muted != snapshot->muted;
            device.volume = snapshot->volume;
            device.muted = snapshot->muted;
            device.volumeKnown = true;
            if (changed)
            {
                RECT channel = MixerChannelRect(window,
                    static_cast<int>(index));
                RECT visible{};
                if (IntersectRect(&visible, &client, &channel))
                    InvalidateRect(window, &visible, FALSE);
            }
        }
    }

    struct ModernScrollMetrics
    {
        int count = 0;
        int topIndex = 0;
        int visibleItems = 0;
        int maximumTop = 0;
        int trackTop = 0;
        int trackHeight = 0;
        int thumbTop = 0;
        int thumbHeight = 0;
    };

    ModernScrollMetrics DeviceListScrollMetrics(HWND window)
    {
        RECT client{};
        GetClientRect(window, &client);
        ModernScrollMetrics metrics;
        metrics.count = static_cast<int>(SendMessageW(window, LB_GETCOUNT, 0, 0));
        metrics.topIndex = std::max<int>(0,
            static_cast<int>(SendMessageW(window, LB_GETTOPINDEX, 0, 0)));
        const int itemHeight = std::max<int>(1,
            static_cast<int>(SendMessageW(window, LB_GETITEMHEIGHT, 0, 0)));
        // Only fully visible rows count. Using ceil() made the list stop one
        // row too early, leaving the final device clipped at the bottom.
        metrics.visibleItems = std::max(1,
            static_cast<int>(client.bottom) / itemHeight);
        metrics.maximumTop = std::max(0, metrics.count - metrics.visibleItems);
        metrics.trackTop = UiScale(window, 7);
        metrics.trackHeight = std::max(1,
            static_cast<int>(client.bottom) - UiScale(window, 14));
        if (metrics.maximumTop > 0)
        {
            metrics.thumbHeight = std::max(UiScale(window, 28),
                MulDiv(metrics.trackHeight, metrics.visibleItems, std::max(metrics.count, 1)));
            metrics.thumbHeight = std::min(metrics.thumbHeight, metrics.trackHeight);
            metrics.thumbTop = metrics.trackTop + MulDiv(
                metrics.trackHeight - metrics.thumbHeight,
                std::min(metrics.topIndex, metrics.maximumTop), metrics.maximumTop);
        }
        return metrics;
    }

    RECT DeviceVolumeBarRect(HWND window, int row)
    {
        const int itemHeight = std::max<int>(1,
            static_cast<int>(SendMessageW(window, LB_GETITEMHEIGHT, 0, 0)));
        RECT client{};
        GetClientRect(window, &client);
        return { UiScale(window, 44), row * itemHeight + UiScale(window, 29),
            std::max<LONG>(UiScale(window, 80),
                client.right - UiScale(window, 132)),
            row * itemHeight + UiScale(window, 51) };
    }

    RECT DeviceMuteRect(HWND window, int row)
    {
        const int itemHeight = std::max<int>(1,
            static_cast<int>(SendMessageW(window, LB_GETITEMHEIGHT, 0, 0)));
        RECT client{};
        GetClientRect(window, &client);
        return { client.right - UiScale(window, 67),
            row * itemHeight + UiScale(window, 25),
            client.right - UiScale(window, 29),
            row * itemHeight + UiScale(window, 53) };
    }

    bool DeviceListPoint(HWND window, int x, int y, int& index, int& row)
    {
        RECT client{};
        GetClientRect(window, &client);
        const int itemHeight = std::max<int>(1,
            static_cast<int>(SendMessageW(window, LB_GETITEMHEIGHT, 0, 0)));
        row = std::max(0, y / itemHeight);
        const int topIndex = std::max<int>(0,
            static_cast<int>(SendMessageW(window, LB_GETTOPINDEX, 0, 0)));
        index = topIndex + row;
        const int count = static_cast<int>(SendMessageW(window, LB_GETCOUNT, 0, 0));
        return x >= 0 && y >= 0 && index >= 0 && index < count &&
            (row + 1) * itemHeight <= client.bottom &&
            static_cast<size_t>(index) < g_devices.size();
    }

    void SetDeviceVolumeFromPointer(HWND window, int index, int row, int x)
    {
        if (index < 0 || static_cast<size_t>(index) >= g_devices.size()) return;
        const RECT bar = DeviceVolumeBarRect(window, row);
        const float value = bar.right > bar.left
            ? std::clamp(static_cast<float>(x - bar.left) /
                static_cast<float>(bar.right - bar.left), 0.0f, 1.0f)
            : 0.0f;
        DeviceInfo& device = g_devices[static_cast<size_t>(index)];
        if (device.volumeKnown && std::fabs(device.volume - value) < 0.001f) return;
        device.volume = value;
        device.volumeKnown = true;
        QueueEndpointVolumeChange(window, device.id, value, true,
            device.muted, false);
        RECT item{};
        GetClientRect(window, &item);
        item.top = row * std::max<int>(1,
            static_cast<int>(SendMessageW(window, LB_GETITEMHEIGHT, 0, 0)));
        item.bottom = std::min(item.bottom, item.top +
            static_cast<int>(SendMessageW(window, LB_GETITEMHEIGHT, 0, 0)));
        InvalidateRect(window, &item, FALSE);
    }

    void PaintModernDeviceList(HWND window, HDC destination)
    {
        RECT client{};
        GetClientRect(window, &client);
        const int width = std::max<LONG>(client.right, 1);
        const int height = std::max<LONG>(client.bottom, 1);
        HDC canvas = destination;
        UiSurfaceCache* cache = EnsureUiSurfaceCache(window, destination, width, height);
        if (cache) canvas = cache->canvas;
        FillRect(canvas, &client, g_surfaceAltBrush);

        const bool enabled = IsWindowEnabled(window) != FALSE;
        const int itemHeight = std::max<int>(1,
            static_cast<int>(SendMessageW(window, LB_GETITEMHEIGHT, 0, 0)));
        const ModernScrollMetrics metrics = DeviceListScrollMetrics(window);
        const int hoveredIndex = static_cast<int>(reinterpret_cast<INT_PTR>(
            GetPropW(window, kUiListHoverProperty))) - 1;
        const float scale = static_cast<float>(UiDpi(window)) / 96.0f;
        Gdiplus::Graphics graphics(canvas);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        for (int index = metrics.topIndex, row = 0;
            index < metrics.count &&
                (row + 1) * itemHeight <= client.bottom; ++index, ++row)
        {
            const float top = static_cast<float>(row * itemHeight);
            const Gdiplus::RectF itemBounds(5.0f * scale, top + 2.0f * scale,
                std::max(1.0f, static_cast<float>(client.right) - 22.0f * scale),
                std::max(1.0f, static_cast<float>(itemHeight) - 4.0f * scale));
            const bool selected = SendMessageW(window, LB_GETSEL,
                static_cast<WPARAM>(index), 0) > 0;
            if (selected)
                FillRounded(graphics, itemBounds, 7.0f * scale,
                    GdiColor(BlendColor(g_ui.surfaceAlt, g_ui.accent,
                        g_ui.dark ? 0.24 : 0.10)));
            else if (index == hoveredIndex)
                FillRounded(graphics, itemBounds, 7.0f * scale,
                    GdiColor(BlendColor(g_ui.surfaceAlt, g_ui.text,
                        g_ui.dark ? 0.08 : 0.035)));

            const float centerX = 22.0f * scale;
            const float centerY = top + 15.0f * scale;
            const float radius = 8.0f * scale;
            Gdiplus::Pen ring(GdiColor(selected ? g_ui.accent : g_ui.muted),
                (selected ? 2.0f : 1.3f) * scale);
            graphics.DrawEllipse(&ring, centerX - radius, centerY - radius,
                radius * 2.0f, radius * 2.0f);
            if (selected)
            {
                Gdiplus::SolidBrush dot(GdiColor(g_ui.accent));
                const float dotRadius = 4.0f * scale;
                graphics.FillEllipse(&dot, centerX - dotRadius, centerY - dotRadius,
                    dotRadius * 2.0f, dotRadius * 2.0f);
            }

            wchar_t label[512]{};
            SendMessageW(window, LB_GETTEXT, static_cast<WPARAM>(index),
                reinterpret_cast<LPARAM>(label));
            DrawUiText(graphics, canvas, window, label,
                Gdiplus::RectF(44.0f * scale, top + 1.0f * scale,
                    std::max(1.0f, static_cast<float>(client.right) -
                        234.0f * scale),
                    26.0f * scale), UiFontRole::Body,
                enabled ? g_ui.text : g_ui.muted);

            const DeviceInfo* device = static_cast<size_t>(index) < g_devices.size()
                ? &g_devices[static_cast<size_t>(index)] : nullptr;
            const float deviceVolume = device && device->volumeKnown
                ? std::clamp(device->volume, 0.0f, 1.0f) : 0.0f;
            const bool deviceMuted = device && device->muted;
            const RECT volumeRect = DeviceVolumeBarRect(window, row);
            const float volumeLeft = static_cast<float>(volumeRect.left);
            const float volumeRight = static_cast<float>(volumeRect.right);
            const float volumeY = top + 40.0f * scale;
            FillRounded(graphics,
                Gdiplus::RectF(volumeLeft, volumeY - 2.0f * scale,
                    std::max(1.0f, volumeRight - volumeLeft), 4.0f * scale),
                2.0f * scale, GdiColor(g_ui.border));
            if (deviceVolume > 0.0f)
                FillRounded(graphics,
                    Gdiplus::RectF(volumeLeft, volumeY - 2.0f * scale,
                        std::max(1.0f, (volumeRight - volumeLeft) * deviceVolume),
                        4.0f * scale), 2.0f * scale,
                    GdiColor(deviceMuted ? g_ui.muted : g_ui.accent));
            const float thumbX = volumeLeft +
                (volumeRight - volumeLeft) * deviceVolume;
            Gdiplus::SolidBrush volumeThumb(GdiColor(
                deviceMuted ? g_ui.muted : g_ui.accent));
            graphics.FillEllipse(&volumeThumb, thumbX - 6.0f * scale,
                volumeY - 6.0f * scale, 12.0f * scale, 12.0f * scale);
            Gdiplus::Pen volumeBorder(GdiColor(g_ui.elevated), 1.5f * scale);
            graphics.DrawEllipse(&volumeBorder, thumbX - 6.0f * scale,
                volumeY - 6.0f * scale, 12.0f * scale, 12.0f * scale);

            const int percent = static_cast<int>(std::lround(deviceVolume * 100.0f));
            const std::wstring percentText = device && device->volumeKnown
                ? std::to_wstring(percent) + L"%" : L"--";
            DrawUiText(graphics, canvas, window, percentText.c_str(),
                Gdiplus::RectF(static_cast<float>(client.right) - 125.0f * scale,
                    top + 27.0f * scale, 54.0f * scale, 26.0f * scale),
                UiFontRole::Caption, deviceMuted ? g_ui.muted : g_ui.text,
                Gdiplus::StringAlignmentFar);

            const RECT muteRect = DeviceMuteRect(window, row);
            const float speakerX = static_cast<float>(muteRect.left + UiScale(window, 7));
            const float speakerY = top + 40.0f * scale;
            Gdiplus::Pen speaker(GdiColor(deviceMuted ? g_ui.danger : g_ui.muted),
                1.7f * scale);
            speaker.SetStartCap(Gdiplus::LineCapRound);
            speaker.SetEndCap(Gdiplus::LineCapRound);
            Gdiplus::PointF speakerBody[] = {
                { speakerX, speakerY - 3.0f * scale },
                { speakerX + 4.0f * scale, speakerY - 3.0f * scale },
                { speakerX + 9.0f * scale, speakerY - 8.0f * scale },
                { speakerX + 9.0f * scale, speakerY + 8.0f * scale },
                { speakerX + 4.0f * scale, speakerY + 3.0f * scale },
                { speakerX, speakerY + 3.0f * scale }
            };
            graphics.DrawLines(&speaker, speakerBody, ARRAYSIZE(speakerBody));
            if (deviceMuted)
            {
                graphics.DrawLine(&speaker, speakerX + 13.0f * scale,
                    speakerY - 5.0f * scale, speakerX + 21.0f * scale,
                    speakerY + 5.0f * scale);
                graphics.DrawLine(&speaker, speakerX + 21.0f * scale,
                    speakerY - 5.0f * scale, speakerX + 13.0f * scale,
                    speakerY + 5.0f * scale);
            }
            else
            {
                graphics.DrawArc(&speaker, speakerX + 10.0f * scale,
                    speakerY - 7.0f * scale, 11.0f * scale, 14.0f * scale,
                    -60.0f, 120.0f);
            }
        }

        if (metrics.maximumTop > 0)
        {
            const float scrollHover = UiHoverAmount(window);
            const float railWidth = (3.0f + 3.0f * scrollHover) * scale;
            const float railX = static_cast<float>(client.right) - 9.0f * scale - railWidth * 0.5f;
            FillRounded(graphics,
                Gdiplus::RectF(railX, static_cast<float>(metrics.thumbTop), railWidth,
                    static_cast<float>(metrics.thumbHeight)), railWidth * 0.5f,
                GdiColor(BlendColor(g_ui.muted, g_ui.text, scrollHover),
                    static_cast<BYTE>(std::lround(125.0f + 55.0f * scrollHover))));
        }

        graphics.Flush(Gdiplus::FlushIntentionSync);
        PresentUiControlSurface(window, destination, cache, canvas, width, height);
    }

    LRESULT CALLBACK ModernListSubclass(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam, UINT_PTR, DWORD_PTR)
    {
        if (message == WM_TIMER && wParam == kUiAnimationTimerId)
        {
            AdvanceUiAnimation(window);
            return 0;
        }
        if (message == kDeviceVolumesUpdatedMessage)
        {
            ApplyEndpointVolumeSnapshots(window);
            return 0;
        }
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            PaintModernDeviceList(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_PRINTCLIENT)
        {
            PaintModernDeviceList(window, reinterpret_cast<HDC>(wParam));
            return 0;
        }
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_MOUSEWHEEL)
        {
            const ModernScrollMetrics metrics = DeviceListScrollMetrics(window);
            const int steps = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            if (metrics.maximumTop > 0 && steps != 0)
            {
                const int target = std::clamp(metrics.topIndex - steps * 3,
                    0, metrics.maximumTop);
                SendMessageW(window, LB_SETTOPINDEX, static_cast<WPARAM>(target), 0);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        if (message == WM_LBUTTONDOWN)
        {
            RECT client{};
            GetClientRect(window, &client);
            const int x = static_cast<short>(LOWORD(lParam));
            const int y = static_cast<short>(HIWORD(lParam));
            const ModernScrollMetrics metrics = DeviceListScrollMetrics(window);
            if (metrics.maximumTop > 0 && x >= client.right - UiScale(window, 22))
            {
                if (y >= metrics.thumbTop && y <= metrics.thumbTop + metrics.thumbHeight)
                {
                    SetPropW(window, kUiScrollDragProperty, reinterpret_cast<HANDLE>(1));
                    SetPropW(window, kUiScrollOffsetProperty,
                        reinterpret_cast<HANDLE>(static_cast<INT_PTR>(y - metrics.thumbTop + 1)));
                    SetCapture(window);
                }
                else
                {
                    const int trackSpan = std::max(1,
                        metrics.trackHeight - metrics.thumbHeight);
                    const int target = std::clamp(MulDiv(
                        y - metrics.trackTop - metrics.thumbHeight / 2,
                        metrics.maximumTop, trackSpan), 0, metrics.maximumTop);
                    SendMessageW(window, LB_SETTOPINDEX, static_cast<WPARAM>(target), 0);
                }
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            int deviceIndex = -1;
            int row = -1;
            if (DeviceListPoint(window, x, y, deviceIndex, row))
            {
                POINT pointer{ x, y };
                const RECT volumeRect = DeviceVolumeBarRect(window, row);
                const RECT muteRect = DeviceMuteRect(window, row);
                if (PtInRect(&volumeRect, pointer))
                {
                    SetPropW(window, kUiVolumeDragProperty,
                        reinterpret_cast<HANDLE>(static_cast<INT_PTR>(deviceIndex + 1)));
                    SetCapture(window);
                    SetDeviceVolumeFromPointer(window, deviceIndex, row, x);
                    return 0;
                }
                if (PtInRect(&muteRect, pointer))
                {
                    DeviceInfo& device = g_devices[static_cast<size_t>(deviceIndex)];
                    device.muted = !device.muted;
                    device.volumeKnown = true;
                    QueueEndpointVolumeChange(window, device.id, device.volume,
                        false, device.muted, true);
                    const int itemHeight = std::max<int>(1,
                        static_cast<int>(SendMessageW(window,
                            LB_GETITEMHEIGHT, 0, 0)));
                    RECT item{ 0, row * itemHeight, client.right,
                        std::min<LONG>(client.bottom, (row + 1) * itemHeight) };
                    InvalidateRect(window, &item, FALSE);
                    return 0;
                }
                if (g_apoMode) return 0;
            }
            // The remaining pixels below the last fully visible row are not an
            // item. Keep the native list box from selecting a clipped row there.
            const int itemHeight = std::max<int>(1,
                static_cast<int>(SendMessageW(window, LB_GETITEMHEIGHT, 0, 0)));
            if (y >= metrics.visibleItems * itemHeight) return 0;
        }
        if (message == WM_MOUSEMOVE)
        {
            RECT client{};
            GetClientRect(window, &client);
            const int x = static_cast<short>(LOWORD(lParam));
            const int y = static_cast<short>(HIWORD(lParam));
            TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, window, 0 };
            TrackMouseEvent(&tracking);
            if (GetPropW(window, kUiVolumeDragProperty))
            {
                const int deviceIndex = static_cast<int>(reinterpret_cast<INT_PTR>(
                    GetPropW(window, kUiVolumeDragProperty))) - 1;
                const int topIndex = std::max<int>(0,
                    static_cast<int>(SendMessageW(window,
                        LB_GETTOPINDEX, 0, 0)));
                const int row = deviceIndex - topIndex;
                if (row >= 0)
                    SetDeviceVolumeFromPointer(window, deviceIndex, row, x);
                return 0;
            }
            if (GetPropW(window, kUiScrollDragProperty))
            {
                const ModernScrollMetrics metrics = DeviceListScrollMetrics(window);
                const int offset = static_cast<int>(reinterpret_cast<INT_PTR>(
                    GetPropW(window, kUiScrollOffsetProperty))) - 1;
                const int trackSpan = std::max(1,
                    metrics.trackHeight - metrics.thumbHeight);
                const int target = std::clamp(MulDiv(
                    y - offset - metrics.trackTop, metrics.maximumTop, trackSpan),
                    0, metrics.maximumTop);
                SendMessageW(window, LB_SETTOPINDEX, static_cast<WPARAM>(target), 0);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            const bool scrollHover = x >= client.right - UiScale(window, 22);
            const bool previousScrollHover = GetPropW(window, kUiScrollHoverProperty) != nullptr;
            if (scrollHover != previousScrollHover)
            {
                if (scrollHover) SetPropW(window, kUiScrollHoverProperty, reinterpret_cast<HANDLE>(1));
                else RemovePropW(window, kUiScrollHoverProperty);
                SetUiHoverTarget(window, scrollHover);
            }
            const int itemHeight = std::max<int>(1,
                static_cast<int>(SendMessageW(window, LB_GETITEMHEIGHT, 0, 0)));
            const int topIndex = std::max<int>(0,
                static_cast<int>(SendMessageW(window, LB_GETTOPINDEX, 0, 0)));
            const int count = static_cast<int>(SendMessageW(window, LB_GETCOUNT, 0, 0));
            const int hoverIndex = scrollHover ||
                (y / itemHeight + 1) * itemHeight > client.bottom
                ? -1 : topIndex + y / itemHeight;
            const int clampedHover = hoverIndex >= 0 && hoverIndex < count ? hoverIndex : -1;
            const int previousHover = static_cast<int>(reinterpret_cast<INT_PTR>(
                GetPropW(window, kUiListHoverProperty))) - 1;
            if (clampedHover != previousHover)
            {
                if (clampedHover >= 0)
                    SetPropW(window, kUiListHoverProperty,
                        reinterpret_cast<HANDLE>(static_cast<INT_PTR>(clampedHover + 1)));
                else RemovePropW(window, kUiListHoverProperty);
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        else if (message == WM_MOUSELEAVE)
        {
            RemovePropW(window, kUiListHoverProperty);
            RemovePropW(window, kUiScrollHoverProperty);
            SetUiHoverTarget(window, false);
            InvalidateRect(window, nullptr, FALSE);
        }
        else if (message == WM_LBUTTONUP || message == WM_CAPTURECHANGED)
        {
            if (GetPropW(window, kUiVolumeDragProperty))
            {
                RemovePropW(window, kUiVolumeDragProperty);
                if (GetCapture() == window) ReleaseCapture();
                return 0;
            }
            if (GetPropW(window, kUiScrollDragProperty))
            {
                RemovePropW(window, kUiScrollDragProperty);
                RemovePropW(window, kUiScrollOffsetProperty);
                if (GetCapture() == window) ReleaseCapture();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
        }
        else if (message == LB_RESETCONTENT || message == LB_ADDSTRING ||
            message == LB_DELETESTRING || message == LB_SETSEL ||
            message == LB_SETTOPINDEX)
        {
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            InvalidateRect(window, nullptr, FALSE);
            return result;
        }
        else if (message == WM_KEYDOWN || message == WM_KEYUP ||
            message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE)
        {
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            InvalidateRect(window, nullptr, FALSE);
            return result;
        }
        else if (message == WM_NCDESTROY)
        {
            RemovePropW(window, kUiListHoverProperty);
            RemovePropW(window, kUiScrollHoverProperty);
            RemovePropW(window, kUiScrollDragProperty);
            RemovePropW(window, kUiScrollOffsetProperty);
            RemovePropW(window, kUiVolumeDragProperty);
            DestroyUiAnimation(window);
            DestroyUiSurfaceCache(window);
            RemoveWindowSubclass(window, ModernListSubclass, 6);
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    struct MixerScrollMetrics
    {
        int count = 0;
        int channelWidth = 0;
        int channelGap = 0;
        int channelPitch = 0;
        int padding = 0;
        int contentWidth = 0;
        int viewportWidth = 0;
        int offset = 0;
        int maximumOffset = 0;
        int trackLeft = 0;
        int trackTop = 0;
        int trackWidth = 0;
        int thumbLeft = 0;
        int thumbWidth = 0;
    };

    int MixerStoredScrollOffset(HWND window)
    {
        const INT_PTR encoded = reinterpret_cast<INT_PTR>(
            GetPropW(window, kUiMixerScrollProperty));
        return encoded > 0 ? static_cast<int>(encoded - 1) : 0;
    }

    void StoreMixerScrollOffset(HWND window, int offset)
    {
        if (offset <= 0)
            RemovePropW(window, kUiMixerScrollProperty);
        else
            SetPropW(window, kUiMixerScrollProperty,
                reinterpret_cast<HANDLE>(static_cast<INT_PTR>(offset + 1)));
    }

    MixerScrollMetrics DeviceMixerScrollMetrics(HWND window)
    {
        RECT client{};
        GetClientRect(window, &client);
        MixerScrollMetrics metrics;
        metrics.count = std::max<int>(0,
            static_cast<int>(SendMessageW(window, LB_GETCOUNT, 0, 0)));
        metrics.channelWidth = UiScale(window, 136);
        metrics.channelGap = UiScale(window, 12);
        metrics.channelPitch = metrics.channelWidth + metrics.channelGap;
        metrics.padding = UiScale(window, 8);
        metrics.viewportWidth = std::max<LONG>(client.right, 1);
        metrics.contentWidth = metrics.padding * 2;
        if (metrics.count > 0)
        {
            metrics.contentWidth += metrics.count * metrics.channelWidth +
                (metrics.count - 1) * metrics.channelGap;
        }
        metrics.maximumOffset = std::max(0,
            metrics.contentWidth - metrics.viewportWidth);
        metrics.offset = std::clamp(MixerStoredScrollOffset(window),
            0, metrics.maximumOffset);
        metrics.trackLeft = UiScale(window, 10);
        metrics.trackTop = std::max(0, static_cast<int>(client.bottom) -
            UiScale(window, 10));
        metrics.trackWidth = std::max(1, static_cast<int>(client.right) -
            UiScale(window, 20));
        if (metrics.maximumOffset > 0)
        {
            metrics.thumbWidth = std::max(UiScale(window, 40),
                MulDiv(metrics.trackWidth, metrics.viewportWidth,
                    std::max(metrics.contentWidth, 1)));
            metrics.thumbWidth = std::min(metrics.thumbWidth,
                metrics.trackWidth);
            metrics.thumbLeft = metrics.trackLeft + MulDiv(
                metrics.trackWidth - metrics.thumbWidth,
                metrics.offset, metrics.maximumOffset);
        }
        return metrics;
    }

    void SetMixerScrollOffset(HWND window, int offset)
    {
        const MixerScrollMetrics metrics = DeviceMixerScrollMetrics(window);
        const int clamped = std::clamp(offset, 0, metrics.maximumOffset);
        if (clamped == metrics.offset &&
            clamped == MixerStoredScrollOffset(window)) return;
        StoreMixerScrollOffset(window, clamped);
        InvalidateRect(window, nullptr, FALSE);
    }

    RECT MixerChannelRect(HWND window, int index)
    {
        RECT client{};
        GetClientRect(window, &client);
        const MixerScrollMetrics metrics = DeviceMixerScrollMetrics(window);
        const int left = metrics.padding + index * metrics.channelPitch -
            metrics.offset;
        const int scrollReserve = metrics.maximumOffset > 0
            ? UiScale(window, 20) : UiScale(window, 6);
        return { left, UiScale(window, 5), left + metrics.channelWidth,
            std::max(UiScale(window, 80),
                static_cast<int>(client.bottom) - scrollReserve) };
    }

    RECT MixerRadioRect(HWND window, int index)
    {
        RECT channel = MixerChannelRect(window, index);
        const int inset = UiScale(window, 9);
        return { channel.left + inset, channel.top + UiScale(window, 6),
            channel.right - inset, channel.top + UiScale(window, 36) };
    }

    RECT MixerPlaybackRect(HWND window, int index)
    {
        const RECT output = MixerRadioRect(window, index);
        return { output.left, output.bottom + UiScale(window, 4),
            output.right, output.bottom + UiScale(window, 34) };
    }

    RECT MixerMuteRect(HWND window, int index);
    RECT MixerNameRect(HWND window, int index);

    RECT MixerFaderRect(HWND window, int index)
    {
        RECT channel = MixerChannelRect(window, index);
        const RECT playback = MixerPlaybackRect(window, index);
        const RECT name = MixerNameRect(window, index);
        return { channel.left + UiScale(window, 5),
            playback.bottom + UiScale(window, 3),
            channel.right - UiScale(window, 5),
            name.top - UiScale(window, 3) };
    }

    RECT MixerFaderRailRect(HWND window, int index)
    {
        const RECT view = MixerFaderRect(window, index);
        const RECT mute = MixerMuteRect(window, index);
        const int center = (view.left + view.right) / 2;
        const int top = view.top + UiScale(window, 8);
        const int bottom = std::max<int>(top + UiScale(window, 28),
            static_cast<int>(mute.top) - UiScale(window, 6));
        return { center - UiScale(window, 24), top,
            center + UiScale(window, 24), bottom };
    }

    RECT MixerFaderHitRect(HWND window, int index)
    {
        RECT view = MixerFaderRect(window, index);
        const RECT rail = MixerFaderRailRect(window, index);
        view.left = std::min(view.left, rail.left - UiScale(window, 8));
        view.right = std::max(view.right, rail.right + UiScale(window, 8));
        view.top = std::min(view.top, rail.top - UiScale(window, 10));
        view.bottom = std::max(view.bottom, rail.bottom + UiScale(window, 10));
        const RECT channel = MixerChannelRect(window, index);
        view.left = std::max(view.left, channel.left);
        view.right = std::min(view.right, channel.right);
        view.top = std::max(view.top, MixerPlaybackRect(window, index).bottom);
        view.bottom = std::min(view.bottom, MixerNameRect(window, index).top);
        return view;
    }

    RECT MixerMuteRect(HWND window, int index)
    {
        RECT channel = MixerChannelRect(window, index);
        return { channel.right - UiScale(window, 40),
            channel.bottom - UiScale(window, 64),
            channel.right - UiScale(window, 10),
            channel.bottom - UiScale(window, 40) };
    }

    RECT MixerNameRect(HWND window, int index)
    {
        RECT channel = MixerChannelRect(window, index);
        return { channel.left + UiScale(window, 8),
            channel.bottom - UiScale(window, 38),
            channel.right - UiScale(window, 8),
            channel.bottom - UiScale(window, 5) };
    }

    bool MixerIndexAtPoint(HWND window, int x, int y, int& index)
    {
        const MixerScrollMetrics metrics = DeviceMixerScrollMetrics(window);
        const int contentX = x + metrics.offset - metrics.padding;
        if (contentX < 0 || metrics.channelPitch <= 0) return false;
        index = contentX / metrics.channelPitch;
        if (index < 0 || index >= metrics.count ||
            contentX % metrics.channelPitch >= metrics.channelWidth) return false;
        const RECT channel = MixerChannelRect(window, index);
        POINT point{ x, y };
        return PtInRect(&channel, point) != FALSE;
    }

    void InvalidateMixerChannel(HWND window, int index)
    {
        RECT client{};
        RECT channel = MixerChannelRect(window, index);
        RECT visible{};
        GetClientRect(window, &client);
        if (IntersectRect(&visible, &client, &channel))
            InvalidateRect(window, &visible, FALSE);
    }

    void SetMixerVolumeFromPointer(HWND window, int index, int y)
    {
        if (index < 0 || static_cast<size_t>(index) >= g_devices.size()) return;
        const RECT fader = MixerFaderRailRect(window, index);
        const int position = std::clamp<int>(y,
            static_cast<int>(fader.top), static_cast<int>(fader.bottom));
        const float value = fader.bottom > fader.top
            ? std::clamp(static_cast<float>(fader.bottom - position) /
                static_cast<float>(fader.bottom - fader.top), 0.0f, 1.0f)
            : 0.0f;
        DeviceInfo& device = g_devices[static_cast<size_t>(index)];
        if (device.volumeKnown && std::fabs(device.volume - value) < 0.001f)
            return;
        device.volume = value;
        device.volumeKnown = true;
        QueueEndpointVolumeChange(window, device.id, value, true,
            device.muted, false);
        InvalidateMixerChannel(window, index);
    }

    void NotifyMixerSelectionChanged(HWND window)
    {
        SendMessageW(GetParent(window), WM_COMMAND,
            MAKEWPARAM(kDeviceListId, LBN_SELCHANGE),
            reinterpret_cast<LPARAM>(window));
    }

    void PaintModernDeviceMixer(HWND window, HDC destination)
    {
        RECT client{};
        GetClientRect(window, &client);
        const int width = std::max<LONG>(client.right, 1);
        const int height = std::max<LONG>(client.bottom, 1);
        HDC canvas = destination;
        UiSurfaceCache* cache = EnsureUiSurfaceCache(window, destination,
            width, height);
        if (cache) canvas = cache->canvas;
        FillRect(canvas, &client, g_surfaceAltBrush);

        const MixerScrollMetrics metrics = DeviceMixerScrollMetrics(window);
        const int hoveredIndex = static_cast<int>(reinterpret_cast<INT_PTR>(
            GetPropW(window, kUiListHoverProperty))) - 1;
        const bool enabled = IsWindowEnabled(window) != FALSE;
        const float scale = static_cast<float>(UiDpi(window)) / 96.0f;
        Gdiplus::Graphics graphics(canvas);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        for (int index = 0; index < metrics.count; ++index)
        {
            const RECT channelRect = MixerChannelRect(window, index);
            RECT visible{};
            if (!IntersectRect(&visible, &client, &channelRect)) continue;
            const Gdiplus::RectF channel(
                static_cast<float>(channelRect.left),
                static_cast<float>(channelRect.top),
                static_cast<float>(channelRect.right - channelRect.left),
                static_cast<float>(channelRect.bottom - channelRect.top));
            const bool selected = SendMessageW(window, LB_GETSEL,
                static_cast<WPARAM>(index), 0) > 0;
            COLORREF channelColor = g_ui.surface;
            if (selected)
                channelColor = BlendColor(g_ui.surface, g_ui.accent,
                    g_ui.dark ? 0.18 : 0.075);
            else if (index == hoveredIndex)
                channelColor = BlendColor(g_ui.surface, g_ui.text,
                    g_ui.dark ? 0.07 : 0.025);
            FillRounded(graphics, channel, 10.0f * scale,
                GdiColor(channelColor));
            StrokeRounded(graphics, channel, 10.0f * scale,
                GdiColor(selected ? BlendColor(g_ui.border, g_ui.accent, 0.45)
                    : g_ui.border), 1.0f);

            const RECT radioRect = MixerRadioRect(window, index);
            const Gdiplus::RectF radioBounds(
                static_cast<float>(radioRect.left),
                static_cast<float>(radioRect.top),
                static_cast<float>(radioRect.right - radioRect.left),
                static_cast<float>(radioRect.bottom - radioRect.top));
            FillRounded(graphics, radioBounds, 7.0f * scale,
                GdiColor(selected
                    ? BlendColor(g_ui.surfaceAlt, g_ui.accent,
                        g_ui.dark ? 0.30 : 0.12)
                    : g_ui.surfaceAlt));
            const float radioX = static_cast<float>(radioRect.left) +
                16.0f * scale;
            const float radioY = (radioRect.top + radioRect.bottom) * 0.5f;
            const float radioRadius = 7.0f * scale;
            Gdiplus::Pen ring(GdiColor(selected ? g_ui.accent : g_ui.muted),
                (selected ? 2.0f : 1.3f) * scale);
            graphics.DrawEllipse(&ring, radioX - radioRadius,
                radioY - radioRadius, radioRadius * 2.0f,
                radioRadius * 2.0f);
            if (selected)
            {
                Gdiplus::SolidBrush dot(GdiColor(g_ui.accent));
                const float dotRadius = 3.5f * scale;
                graphics.FillEllipse(&dot, radioX - dotRadius,
                    radioY - dotRadius, dotRadius * 2.0f,
                    dotRadius * 2.0f);
            }
            DrawUiText(graphics, canvas, window, L"输出",
                Gdiplus::RectF(radioX + 12.0f * scale,
                    static_cast<float>(radioRect.top),
                    std::max(1.0f, static_cast<float>(radioRect.right) -
                        radioX - 17.0f * scale),
                    static_cast<float>(radioRect.bottom - radioRect.top)),
                UiFontRole::Caption,
                enabled ? g_ui.text : g_ui.muted);

            const bool playbackSelected = static_cast<size_t>(index) <
                g_devices.size() &&
                g_devices[static_cast<size_t>(index)].playbackSelected;
            const RECT playbackRect = MixerPlaybackRect(window, index);
            const Gdiplus::RectF playbackBounds(
                static_cast<float>(playbackRect.left),
                static_cast<float>(playbackRect.top),
                static_cast<float>(playbackRect.right - playbackRect.left),
                static_cast<float>(playbackRect.bottom - playbackRect.top));
            FillRounded(graphics, playbackBounds, 7.0f * scale,
                GdiColor(playbackSelected
                    ? BlendColor(g_ui.surfaceAlt, g_ui.success,
                        g_ui.dark ? 0.30 : 0.14)
                    : g_ui.surfaceAlt));
            const float playbackX = static_cast<float>(playbackRect.left) +
                16.0f * scale;
            const float playbackY =
                (playbackRect.top + playbackRect.bottom) * 0.5f;
            Gdiplus::Pen playbackRing(
                GdiColor(playbackSelected ? g_ui.success : g_ui.muted),
                (playbackSelected ? 2.0f : 1.3f) * scale);
            graphics.DrawEllipse(&playbackRing, playbackX - radioRadius,
                playbackY - radioRadius, radioRadius * 2.0f,
                radioRadius * 2.0f);
            if (playbackSelected)
            {
                Gdiplus::SolidBrush playbackDot(GdiColor(g_ui.success));
                const float dotRadius = 3.5f * scale;
                graphics.FillEllipse(&playbackDot,
                    playbackX - dotRadius, playbackY - dotRadius,
                    dotRadius * 2.0f, dotRadius * 2.0f);
            }
            DrawUiText(graphics, canvas, window, L"回放",
                Gdiplus::RectF(playbackX + 12.0f * scale,
                    static_cast<float>(playbackRect.top),
                    std::max(1.0f, static_cast<float>(playbackRect.right) -
                        playbackX - 17.0f * scale),
                    static_cast<float>(playbackRect.bottom - playbackRect.top)),
                UiFontRole::Caption,
                enabled ? g_ui.success : g_ui.muted);

            const DeviceInfo* device = static_cast<size_t>(index) <
                g_devices.size() ? &g_devices[static_cast<size_t>(index)] : nullptr;
            const float volume = device && device->volumeKnown
                ? std::clamp(device->volume, 0.0f, 1.0f) : 0.0f;
            const bool muted = device && device->muted;
            const RECT faderView = MixerFaderRect(window, index);
            FillRounded(graphics,
                Gdiplus::RectF(static_cast<float>(faderView.left),
                    static_cast<float>(faderView.top),
                    static_cast<float>(faderView.right - faderView.left),
                    static_cast<float>(faderView.bottom - faderView.top)),
                8.0f * scale,
                GdiColor(BlendColor(g_ui.surfaceAlt, g_ui.text,
                    index == hoveredIndex ? (g_ui.dark ? 0.06 : 0.025) : 0.0)));
            const RECT fader = MixerFaderRailRect(window, index);
            const float centerX = (fader.left + fader.right) * 0.5f;
            const float railTop = static_cast<float>(fader.top);
            const float railBottom = static_cast<float>(fader.bottom);
            const float thumbY = railBottom -
                (railBottom - railTop) * volume;
            FillRounded(graphics,
                Gdiplus::RectF(centerX - 2.0f * scale, railTop,
                    4.0f * scale, std::max(1.0f, railBottom - railTop)),
                2.0f * scale, GdiColor(g_ui.border));
            if (volume > 0.0f)
            {
                FillRounded(graphics,
                    Gdiplus::RectF(centerX - 2.0f * scale, thumbY,
                        4.0f * scale, std::max(1.0f, railBottom - thumbY)),
                    2.0f * scale,
                    GdiColor(muted ? g_ui.muted : g_ui.accent));
            }
            Gdiplus::Pen tickPen(GdiColor(g_ui.border), 1.0f * scale);
            for (int tick = 0; tick <= 4; ++tick)
            {
                const float tickY = railTop + (railBottom - railTop) *
                    static_cast<float>(tick) / 4.0f;
                graphics.DrawLine(&tickPen, centerX - 12.0f * scale,
                    tickY, centerX - 7.0f * scale, tickY);
                graphics.DrawLine(&tickPen, centerX + 7.0f * scale,
                    tickY, centerX + 12.0f * scale, tickY);
            }
            const Gdiplus::RectF thumb(centerX - 24.0f * scale,
                thumbY - 7.0f * scale, 48.0f * scale, 14.0f * scale);
            FillRounded(graphics, thumb, 4.0f * scale,
                GdiColor(muted ? g_ui.muted : g_ui.accent));
            StrokeRounded(graphics, thumb, 4.0f * scale,
                GdiColor(g_ui.elevated), 1.2f);

            const int percent = static_cast<int>(std::lround(volume * 100.0f));
            const std::wstring percentText = device && device->volumeKnown
                ? std::to_wstring(percent) + L"%" : L"--";
            const RECT muteRect = MixerMuteRect(window, index);
            DrawUiText(graphics, canvas, window, percentText.c_str(),
                Gdiplus::RectF(static_cast<float>(channelRect.left) +
                    12.0f * scale, static_cast<float>(muteRect.top),
                    std::max(1.0f, static_cast<float>(muteRect.left -
                        channelRect.left) - 16.0f * scale),
                    static_cast<float>(muteRect.bottom - muteRect.top)),
                UiFontRole::Caption, muted ? g_ui.muted : g_ui.text,
                Gdiplus::StringAlignmentCenter);

            const float speakerX = static_cast<float>(muteRect.left) +
                5.0f * scale;
            const float speakerY = (muteRect.top + muteRect.bottom) * 0.5f;
            Gdiplus::Pen speaker(GdiColor(muted ? g_ui.danger : g_ui.muted),
                1.6f * scale);
            speaker.SetStartCap(Gdiplus::LineCapRound);
            speaker.SetEndCap(Gdiplus::LineCapRound);
            Gdiplus::PointF body[] = {
                { speakerX, speakerY - 3.0f * scale },
                { speakerX + 4.0f * scale, speakerY - 3.0f * scale },
                { speakerX + 8.0f * scale, speakerY - 7.0f * scale },
                { speakerX + 8.0f * scale, speakerY + 7.0f * scale },
                { speakerX + 4.0f * scale, speakerY + 3.0f * scale },
                { speakerX, speakerY + 3.0f * scale }
            };
            graphics.DrawLines(&speaker, body, ARRAYSIZE(body));
            if (muted)
            {
                graphics.DrawLine(&speaker, speakerX + 12.0f * scale,
                    speakerY - 5.0f * scale, speakerX + 20.0f * scale,
                    speakerY + 5.0f * scale);
                graphics.DrawLine(&speaker, speakerX + 20.0f * scale,
                    speakerY - 5.0f * scale, speakerX + 12.0f * scale,
                    speakerY + 5.0f * scale);
            }
            else
            {
                graphics.DrawArc(&speaker, speakerX + 9.0f * scale,
                    speakerY - 6.0f * scale, 10.0f * scale,
                    12.0f * scale, -60.0f, 120.0f);
            }

            wchar_t label[512]{};
            SendMessageW(window, LB_GETTEXT, static_cast<WPARAM>(index),
                reinterpret_cast<LPARAM>(label));
            const RECT nameRect = MixerNameRect(window, index);
            DrawUiWrappedText(graphics, canvas, window, label,
                Gdiplus::RectF(static_cast<float>(nameRect.left),
                    static_cast<float>(nameRect.top),
                    static_cast<float>(nameRect.right - nameRect.left),
                    static_cast<float>(nameRect.bottom - nameRect.top)),
                UiFontRole::Caption, enabled ? g_ui.text : g_ui.muted);
        }

        if (metrics.maximumOffset > 0)
        {
            const float scrollHover = UiHoverAmount(window);
            const float railHeight = (3.0f + 2.0f * scrollHover) * scale;
            FillRounded(graphics,
                Gdiplus::RectF(static_cast<float>(metrics.trackLeft),
                    static_cast<float>(metrics.trackTop),
                    static_cast<float>(metrics.trackWidth), railHeight),
                railHeight * 0.5f, GdiColor(g_ui.border));
            FillRounded(graphics,
                Gdiplus::RectF(static_cast<float>(metrics.thumbLeft),
                    static_cast<float>(metrics.trackTop),
                    static_cast<float>(metrics.thumbWidth), railHeight),
                railHeight * 0.5f,
                GdiColor(BlendColor(g_ui.muted, g_ui.text, scrollHover)));
        }

        graphics.Flush(Gdiplus::FlushIntentionSync);
        PresentUiControlSurface(window, destination, cache, canvas,
            width, height);
    }

    LRESULT CALLBACK ModernMixerSubclass(HWND window, UINT message,
        WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
    {
        if (message == WM_TIMER && wParam == kUiAnimationTimerId)
        {
            AdvanceUiAnimation(window);
            return 0;
        }
        if (message == kDeviceVolumesUpdatedMessage)
        {
            ApplyEndpointVolumeSnapshots(window);
            return 0;
        }
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            PaintModernDeviceMixer(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_PRINTCLIENT)
        {
            PaintModernDeviceMixer(window, reinterpret_cast<HDC>(wParam));
            return 0;
        }
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
        {
            const MixerScrollMetrics metrics = DeviceMixerScrollMetrics(window);
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (metrics.maximumOffset > 0 && delta != 0)
            {
                const int direction = message == WM_MOUSEHWHEEL ? 1 : -1;
                SetMixerScrollOffset(window, metrics.offset + direction *
                    MulDiv(delta, metrics.channelPitch, WHEEL_DELTA));
            }
            return 0;
        }
        if (message == WM_LBUTTONDOWN)
        {
            SetFocus(window);
            RECT client{};
            GetClientRect(window, &client);
            const int x = static_cast<short>(LOWORD(lParam));
            const int y = static_cast<short>(HIWORD(lParam));
            const MixerScrollMetrics metrics = DeviceMixerScrollMetrics(window);
            if (metrics.maximumOffset > 0 &&
                y >= metrics.trackTop - UiScale(window, 6))
            {
                if (x >= metrics.thumbLeft &&
                    x <= metrics.thumbLeft + metrics.thumbWidth)
                {
                    SetPropW(window, kUiScrollDragProperty,
                        reinterpret_cast<HANDLE>(1));
                    SetPropW(window, kUiScrollOffsetProperty,
                        reinterpret_cast<HANDLE>(static_cast<INT_PTR>(
                            x - metrics.thumbLeft + 1)));
                    SetCapture(window);
                }
                else
                {
                    const int span = std::max(1,
                        metrics.trackWidth - metrics.thumbWidth);
                    const int target = MulDiv(
                        x - metrics.trackLeft - metrics.thumbWidth / 2,
                        metrics.maximumOffset, span);
                    SetMixerScrollOffset(window, target);
                }
                return 0;
            }

            int index = -1;
            if (!MixerIndexAtPoint(window, x, y, index)) return 0;
            POINT point{ x, y };
            const RECT radio = MixerRadioRect(window, index);
            const RECT playback = MixerPlaybackRect(window, index);
            const RECT fader = MixerFaderHitRect(window, index);
            const RECT mute = MixerMuteRect(window, index);
            if (PtInRect(&radio, point))
            {
                if (static_cast<size_t>(index) < g_devices.size())
                {
                    const bool selected = SendMessageW(window, LB_GETSEL,
                        static_cast<WPARAM>(index), 0) > 0;
                    SendMessageW(window, LB_SETSEL, selected ? FALSE : TRUE,
                        static_cast<LPARAM>(index));
                    NotifyMixerSelectionChanged(window);
                }
                return 0;
            }
            if (PtInRect(&playback, point))
            {
                TogglePlaybackSelection(window, index);
                return 0;
            }
            if (PtInRect(&mute, point) &&
                static_cast<size_t>(index) < g_devices.size())
            {
                DeviceInfo& device = g_devices[static_cast<size_t>(index)];
                device.muted = !device.muted;
                device.volumeKnown = true;
                QueueEndpointVolumeChange(window, device.id, device.volume,
                    false, device.muted, true);
                InvalidateMixerChannel(window, index);
                return 0;
            }
            if (PtInRect(&fader, point) &&
                static_cast<size_t>(index) < g_devices.size())
            {
                SetPropW(window, kUiVolumeDragProperty,
                    reinterpret_cast<HANDLE>(static_cast<INT_PTR>(index + 1)));
                SetCapture(window);
                SetMixerVolumeFromPointer(window, index, y);
                return 0;
            }
            // The device name and the rest of the channel are deliberately
            // non-selecting. The blue and green pills own their independent
            // APO-attachment and playback selections.
            return 0;
        }
        if (message == WM_MOUSEMOVE)
        {
            const int x = static_cast<short>(LOWORD(lParam));
            const int y = static_cast<short>(HIWORD(lParam));
            TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, window, 0 };
            TrackMouseEvent(&tracking);
            if (GetPropW(window, kUiVolumeDragProperty))
            {
                const int index = static_cast<int>(reinterpret_cast<INT_PTR>(
                    GetPropW(window, kUiVolumeDragProperty))) - 1;
                if (GetCapture() != window) SetCapture(window);
                SetMixerVolumeFromPointer(window, index, y);
                return 0;
            }
            if (GetPropW(window, kUiScrollDragProperty))
            {
                const MixerScrollMetrics metrics = DeviceMixerScrollMetrics(window);
                const int offset = static_cast<int>(reinterpret_cast<INT_PTR>(
                    GetPropW(window, kUiScrollOffsetProperty))) - 1;
                const int span = std::max(1,
                    metrics.trackWidth - metrics.thumbWidth);
                const int target = MulDiv(x - offset - metrics.trackLeft,
                    metrics.maximumOffset, span);
                SetMixerScrollOffset(window, target);
                return 0;
            }

            const MixerScrollMetrics metrics = DeviceMixerScrollMetrics(window);
            const bool scrollHover = metrics.maximumOffset > 0 &&
                y >= metrics.trackTop - UiScale(window, 6);
            const bool previousScrollHover =
                GetPropW(window, kUiScrollHoverProperty) != nullptr;
            if (scrollHover != previousScrollHover)
            {
                if (scrollHover)
                    SetPropW(window, kUiScrollHoverProperty,
                        reinterpret_cast<HANDLE>(1));
                else
                    RemovePropW(window, kUiScrollHoverProperty);
                SetUiHoverTarget(window, scrollHover);
            }
            int hoverIndex = -1;
            if (!scrollHover)
                MixerIndexAtPoint(window, x, y, hoverIndex);
            const int previousHover = static_cast<int>(
                reinterpret_cast<INT_PTR>(GetPropW(window,
                    kUiListHoverProperty))) - 1;
            if (hoverIndex != previousHover)
            {
                if (hoverIndex >= 0)
                    SetPropW(window, kUiListHoverProperty,
                        reinterpret_cast<HANDLE>(static_cast<INT_PTR>(
                            hoverIndex + 1)));
                else
                    RemovePropW(window, kUiListHoverProperty);
                if (previousHover >= 0)
                    InvalidateMixerChannel(window, previousHover);
                if (hoverIndex >= 0)
                    InvalidateMixerChannel(window, hoverIndex);
            }
        }
        else if (message == WM_MOUSELEAVE)
        {
            const int previousHover = static_cast<int>(
                reinterpret_cast<INT_PTR>(GetPropW(window,
                    kUiListHoverProperty))) - 1;
            RemovePropW(window, kUiListHoverProperty);
            RemovePropW(window, kUiScrollHoverProperty);
            SetUiHoverTarget(window, false);
            if (previousHover >= 0)
                InvalidateMixerChannel(window, previousHover);
        }
        else if (message == WM_LBUTTONUP || message == WM_CAPTURECHANGED)
        {
            const bool volumeDrag =
                GetPropW(window, kUiVolumeDragProperty) != nullptr;
            const bool scrollDrag =
                GetPropW(window, kUiScrollDragProperty) != nullptr;
            if (message == WM_LBUTTONUP && volumeDrag)
            {
                const int index = static_cast<int>(reinterpret_cast<INT_PTR>(
                    GetPropW(window, kUiVolumeDragProperty))) - 1;
                const int y = static_cast<short>(HIWORD(lParam));
                SetMixerVolumeFromPointer(window, index, y);
            }
            RemovePropW(window, kUiVolumeDragProperty);
            RemovePropW(window, kUiScrollDragProperty);
            RemovePropW(window, kUiScrollOffsetProperty);
            if ((volumeDrag || scrollDrag) && GetCapture() == window)
                ReleaseCapture();
            if (volumeDrag) QueueEndpointVolumeRefresh(window);
            if (volumeDrag || scrollDrag) return 0;
        }
        else if (message == LB_RESETCONTENT || message == LB_ADDSTRING ||
            message == LB_DELETESTRING || message == LB_SETSEL)
        {
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            if (message == LB_RESETCONTENT) StoreMixerScrollOffset(window, 0);
            InvalidateRect(window, nullptr, FALSE);
            return result;
        }
        else if (message == WM_SIZE)
        {
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            // SetWindowRgn is not resized automatically. The stale rounded
            // region from the original mixer size clipped both painting and
            // mouse hit-testing after the main window was enlarged.
            ApplyRoundedControlRegion(window, 7);
            DestroyUiSurfaceCache(window);
            const MixerScrollMetrics metrics = DeviceMixerScrollMetrics(window);
            StoreMixerScrollOffset(window,
                std::min(metrics.offset, metrics.maximumOffset));
            InvalidateRect(window, nullptr, FALSE);
            return result;
        }
        else if (message == WM_KEYDOWN)
        {
            const MixerScrollMetrics metrics = DeviceMixerScrollMetrics(window);
            if (wParam == VK_LEFT || wParam == VK_RIGHT ||
                wParam == VK_PRIOR || wParam == VK_NEXT ||
                wParam == VK_HOME || wParam == VK_END)
            {
                int target = metrics.offset;
                if (wParam == VK_LEFT) target -= metrics.channelPitch;
                else if (wParam == VK_RIGHT) target += metrics.channelPitch;
                else if (wParam == VK_PRIOR) target -= metrics.viewportWidth;
                else if (wParam == VK_NEXT) target += metrics.viewportWidth;
                else if (wParam == VK_HOME) target = 0;
                else target = metrics.maximumOffset;
                SetMixerScrollOffset(window, target);
                return 0;
            }
        }
        else if (message == WM_SETFOCUS || message == WM_KILLFOCUS ||
            message == WM_ENABLE)
        {
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            InvalidateRect(window, nullptr, FALSE);
            return result;
        }
        else if (message == WM_NCDESTROY)
        {
            RemovePropW(window, kUiListHoverProperty);
            RemovePropW(window, kUiScrollHoverProperty);
            RemovePropW(window, kUiScrollDragProperty);
            RemovePropW(window, kUiScrollOffsetProperty);
            RemovePropW(window, kUiMixerScrollProperty);
            RemovePropW(window, kUiVolumeDragProperty);
            DestroyUiAnimation(window);
            DestroyUiSurfaceCache(window);
            RemoveWindowSubclass(window, ModernMixerSubclass, 16);
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    void PaintModernStatic(HWND window, HDC destination)
    {
        RECT client{};
        GetClientRect(window, &client);
        const bool alternate = GetPropW(window, kUiSurfaceAltProperty) != nullptr;
        // Recompose the exact custom-painted parent surface below the label.
        // Filling every STATIC with a guessed solid colour left conspicuous
        // rectangles on Mica/card/nested surfaces, especially in dark mode.
        HDC canvas = destination;
        UiSurfaceCache* cache = BeginUiControlSurface(window, destination,
            client, canvas, alternate ? g_surfaceAltBrush : g_surfaceBrush);

        wchar_t label[1024]{};
        GetWindowTextW(window, label, ARRAYSIZE(label));
        const bool enabled = IsWindowEnabled(window) != FALSE;
        UINT_PTR roleValue = reinterpret_cast<UINT_PTR>(
            GetPropW(window, kUiFontRoleProperty));
        if (!roleValue) roleValue = static_cast<UINT_PTR>(UiFontRole::Body);
        const UiFontRole role = static_cast<UiFontRole>(roleValue);
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
        const Gdiplus::StringAlignment alignment = (style & SS_CENTER) == SS_CENTER
            ? Gdiplus::StringAlignmentCenter
            : ((style & SS_RIGHT) == SS_RIGHT ? Gdiplus::StringAlignmentFar
                : Gdiplus::StringAlignmentNear);

        Gdiplus::Graphics graphics(canvas);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        LOGFONTW logFont{};
        GetObjectW(UiFont(UiDpi(window), role), sizeof(logFont), &logFont);
        Gdiplus::Font font(canvas, &logFont);
        Gdiplus::StringFormat format;
        format.SetAlignment(alignment);
        format.SetTrimming(Gdiplus::StringTrimmingEllipsisWord);
        const int singleLineHeight = UiScale(window,
            role == UiFontRole::Title ? 34 : (role == UiFontRole::Section ? 24 : 20));
        if (client.bottom <= singleLineHeight + UiScale(window, 4))
        {
            format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        }
        else
        {
            format.SetLineAlignment(Gdiplus::StringAlignmentNear);
        }
        Gdiplus::SolidBrush textBrush(GdiColor(enabled ? g_ui.text : g_ui.muted));
        graphics.DrawString(label, -1, &font,
            Gdiplus::RectF(0.0f, 0.0f, static_cast<float>(client.right),
                static_cast<float>(client.bottom)), &format, &textBrush);
        graphics.Flush(Gdiplus::FlushIntentionSync);
        PresentUiControlSurface(window, destination, cache, canvas,
            std::max<LONG>(client.right, 1), std::max<LONG>(client.bottom, 1));
    }

    LRESULT CALLBACK ModernStaticSubclass(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam, UINT_PTR, DWORD_PTR)
    {
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            PaintModernStatic(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_PRINTCLIENT)
        {
            PaintModernStatic(window, reinterpret_cast<HDC>(wParam));
            return 0;
        }
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_SETTEXT || message == WM_SETFONT)
        {
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            InvalidateRect(window, nullptr, FALSE);
            return result;
        }
        if (message == WM_ENABLE || message == WM_THEMECHANGED)
            InvalidateRect(window, nullptr, FALSE);
        else if (message == WM_NCDESTROY)
        {
            DestroyUiSurfaceCache(window);
            RemoveWindowSubclass(window, ModernStaticSubclass, 7);
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    void ModernizeChild(HWND child)
    {
        wchar_t className[32]{};
        GetClassNameW(child, className, ARRAYSIZE(className));
        const wchar_t* theme = g_ui.dark ? L"DarkMode_Explorer" : L"Explorer";
        SetWindowTheme(child, theme, nullptr);
        UINT_PTR roleValue = reinterpret_cast<UINT_PTR>(GetPropW(child, kUiFontRoleProperty));
        if (!roleValue) roleValue = static_cast<UINT_PTR>(UiFontRole::Body);
        const UiFontRole role = static_cast<UiFontRole>(roleValue);
        SendMessageW(child, WM_SETFONT,
            reinterpret_cast<WPARAM>(UiFont(UiDpi(child), role)), TRUE);

        if (_wcsicmp(className, L"Button") == 0)
        {
            LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
            const LONG_PTR type = style & BS_TYPEMASK;
            if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON || type == BS_PUSHBOX)
            {
                style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
                SetWindowLongPtrW(child, GWL_STYLE, style);
                SetWindowSubclass(child, ModernButtonSubclass, 1, 0);
            }
            else if (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX ||
                type == BS_3STATE || type == BS_AUTO3STATE)
            {
                SetWindowSubclass(child, ModernCheckBoxSubclass, 2, 0);
                InvalidateRect(child, nullptr, FALSE);
            }
        }
        else if (_wcsicmp(className, L"Edit") == 0)
        {
            const LONG_PTR exStyle = GetWindowLongPtrW(child, GWL_EXSTYLE);
            if ((exStyle & WS_EX_CLIENTEDGE) != 0)
            {
                SetWindowLongPtrW(child, GWL_EXSTYLE, exStyle & ~WS_EX_CLIENTEDGE);
                SetWindowPos(child, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
            SendMessageW(child, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                MAKELPARAM(UiScale(child, 9), UiScale(child, 9)));
            ApplyRoundedControlRegion(child, 5);
            SetWindowSubclass(child, ModernEditSubclass, 4, 0);
        }
        else if (_wcsicmp(className, WC_LISTBOXW) == 0)
        {
            const LONG_PTR exStyle = GetWindowLongPtrW(child, GWL_EXSTYLE);
            if ((exStyle & WS_EX_CLIENTEDGE) != 0)
            {
                SetWindowLongPtrW(child, GWL_EXSTYLE, exStyle & ~WS_EX_CLIENTEDGE);
                SetWindowPos(child, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
            const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
            if ((style & (WS_VSCROLL | WS_HSCROLL)) != 0)
            {
                SetWindowLongPtrW(child, GWL_STYLE,
                    style & ~(WS_VSCROLL | WS_HSCROLL));
                SetWindowPos(child, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
            ApplyRoundedControlRegion(child, 7);
            SetWindowSubclass(child, ModernMixerSubclass, 16, 0);
        }
        else if (_wcsicmp(className, WC_COMBOBOXW) == 0)
        {
            SendMessageW(child, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), UiScale(child, 28));
            SendMessageW(child, CB_SETITEMHEIGHT, 0, UiScale(child, 30));
            ApplyRoundedControlRegion(child, 6);
            SetWindowSubclass(child, ModernComboSubclass, 3, 0);
        }
        else if (_wcsicmp(className, TRACKBAR_CLASSW) == 0)
        {
            SetWindowSubclass(child, ModernSliderSubclass, 5, 0);
            InvalidateRect(child, nullptr, FALSE);
        }
        else if (_wcsicmp(className, L"Static") == 0)
        {
            SetWindowSubclass(child, ModernStaticSubclass, 7, 0);
            InvalidateRect(child, nullptr, FALSE);
        }
    }

    void ModernizeChildren(HWND parent)
    {
        EnumChildWindows(parent, [](HWND child, LPARAM value) -> BOOL
        {
            const HWND directParent = reinterpret_cast<HWND>(value);
            if (GetParent(child) == directParent) ModernizeChild(child);
            return TRUE;
        }, reinterpret_cast<LPARAM>(parent));
    }

    void ScaleChildrenForDpi(HWND parent, UINT oldDpi, UINT newDpi)
    {
        if (!oldDpi || oldDpi == newDpi) return;
        struct ScaleContext { HWND parent; UINT oldDpi; UINT newDpi; } context{ parent, oldDpi, newDpi };
        EnumChildWindows(parent, [](HWND child, LPARAM value) -> BOOL
        {
            const auto* context = reinterpret_cast<const ScaleContext*>(value);
            if (GetParent(child) != context->parent) return TRUE;
            RECT rectangle{};
            GetWindowRect(child, &rectangle);
            MapWindowPoints(nullptr, context->parent, reinterpret_cast<POINT*>(&rectangle), 2);
            const int x = MulDiv(rectangle.left, context->newDpi, context->oldDpi);
            const int y = MulDiv(rectangle.top, context->newDpi, context->oldDpi);
            const int width = MulDiv(rectangle.right - rectangle.left, context->newDpi, context->oldDpi);
            const int height = MulDiv(rectangle.bottom - rectangle.top, context->newDpi, context->oldDpi);
            MoveWindow(child, x, y, width, height, FALSE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
    }

    void ConfigureModernWindow(HWND window)
    {
        const BOOL dark = g_ui.dark ? TRUE : FALSE;
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        DwmSetWindowAttribute(window, 19, &dark, sizeof(dark));
        if (g_windows11)
        {
            const int rounded = 2;
            const int mica = 2;
            const COLORREF systemDefault = 0xffffffff;
            DwmSetWindowAttribute(window, 33, &rounded, sizeof(rounded));
            DwmSetWindowAttribute(window, 38, &mica, sizeof(mica));
            DwmSetWindowAttribute(window, 34, &systemDefault, sizeof(systemDefault));
            DwmSetWindowAttribute(window, 35, &systemDefault, sizeof(systemDefault));
            DwmSetWindowAttribute(window, 36, &systemDefault, sizeof(systemDefault));
            const MARGINS glassMargins{ -1, -1, -1, -1 };
            DwmExtendFrameIntoClientArea(window, &glassMargins);
        }
        else
        {
            const COLORREF caption = g_ui.background;
            const COLORREF captionText = g_ui.text;
            const COLORREF border = g_ui.border;
            DwmSetWindowAttribute(window, 35, &caption, sizeof(caption));
            DwmSetWindowAttribute(window, 36, &captionText, sizeof(captionText));
            DwmSetWindowAttribute(window, 34, &border, sizeof(border));
        }
        SetPropW(window, kUiDpiProperty,
            reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(UiDpi(window))));
    }

    void PaintWindowBackdrop(HDC dc, const RECT& client)
    {
        // A black GDI pixel in a fully extended DWM frame is transparent to
        // the Windows 11 system backdrop. Windows 10 receives the solid
        // Fluent fallback instead.
        FillRect(dc, &client, g_windows11
            ? static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)) : g_backgroundBrush);
    }

    HBITMAP CreateUiCanvasBitmap(int width, int height, void** pixels)
    {
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
        bitmapInfo.bmiHeader.biWidth = width;
        bitmapInfo.bmiHeader.biHeight = -height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;
        return CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, pixels, nullptr, 0);
    }

    UiSurfaceCache* EnsureUiSurfaceCache(HWND window, HDC compatible,
        int width, int height)
    {
        auto* cache = reinterpret_cast<UiSurfaceCache*>(
            GetPropW(window, kUiSurfaceCacheProperty));
        if (cache && cache->width == width && cache->height == height)
            return cache;
        if (cache)
        {
            if (cache->canvas && cache->previousBitmap)
                SelectObject(cache->canvas, cache->previousBitmap);
            if (cache->bitmap) DeleteObject(cache->bitmap);
            if (cache->canvas) DeleteDC(cache->canvas);
            delete cache;
            RemovePropW(window, kUiSurfaceCacheProperty);
        }

        cache = new (std::nothrow) UiSurfaceCache{};
        if (!cache) return nullptr;
        cache->width = width;
        cache->height = height;
        cache->canvas = CreateCompatibleDC(compatible);
        cache->bitmap = CreateUiCanvasBitmap(width, height, &cache->pixels);
        if (!cache->bitmap)
            cache->bitmap = CreateCompatibleBitmap(compatible, width, height);
        if (!cache->canvas || !cache->bitmap)
        {
            if (cache->bitmap) DeleteObject(cache->bitmap);
            if (cache->canvas) DeleteDC(cache->canvas);
            delete cache;
            return nullptr;
        }
        cache->previousBitmap = SelectObject(cache->canvas, cache->bitmap);
        SetPropW(window, kUiSurfaceCacheProperty, cache);
        return cache;
    }

    void DestroyUiSurfaceCache(HWND window)
    {
        auto* cache = reinterpret_cast<UiSurfaceCache*>(
            RemovePropW(window, kUiSurfaceCacheProperty));
        if (!cache) return;
        if (cache->canvas && cache->previousBitmap)
            SelectObject(cache->canvas, cache->previousBitmap);
        if (cache->bitmap) DeleteObject(cache->bitmap);
        if (cache->canvas) DeleteDC(cache->canvas);
        delete cache;
    }

    bool CopyCachedParentSurface(HWND child, HDC destination)
    {
        HWND parent = GetParent(child);
        if (!parent) return false;
        const auto* cache = reinterpret_cast<const UiSurfaceCache*>(
            GetPropW(parent, kUiSurfaceCacheProperty));
        if (!cache || !cache->canvas || !cache->valid) return false;
        RECT client{};
        GetClientRect(child, &client);
        POINT origin{ 0, 0 };
        MapWindowPoints(child, parent, &origin, 1);
        return BitBlt(destination, 0, 0, client.right, client.bottom,
            cache->canvas, origin.x, origin.y, SRCCOPY) != FALSE;
    }

    UiSurfaceCache* BeginUiControlSurface(HWND window, HDC destination,
        const RECT& client, HDC& canvas, HBRUSH fallback)
    {
        const int width = std::max<LONG>(client.right, 1);
        const int height = std::max<LONG>(client.bottom, 1);
        UiSurfaceCache* cache = EnsureUiSurfaceCache(window, destination,
            width, height);
        canvas = cache ? cache->canvas : destination;
        if (!CopyCachedParentSurface(window, canvas) &&
            FAILED(DrawThemeParentBackground(window, canvas,
                const_cast<RECT*>(&client))))
            FillRect(canvas, &client, fallback);
        return cache;
    }

    void PresentUiControlSurface(HWND, HDC destination,
        UiSurfaceCache* cache, HDC canvas, int width, int height)
    {
        if (!cache) return;
        FinalizeUiCanvasAlpha(cache->pixels, width, height);
        cache->valid = true;
        BitBlt(destination, 0, 0, width, height, canvas, 0, 0, SRCCOPY);
    }

    void PrepareUiCanvas(void* pixels, int width, int height, HDC dc, const RECT& client)
    {
        if (pixels) ZeroMemory(pixels, static_cast<size_t>(width) * height * sizeof(DWORD));
        PaintWindowBackdrop(dc, client);
    }

    void PrepareUiCanvasRegion(void* pixels, int width, int height, HDC dc,
        const RECT& region)
    {
        if (pixels)
        {
            auto* values = static_cast<DWORD*>(pixels);
            const int left = std::clamp<LONG>(region.left, 0, width);
            const int right = std::clamp<LONG>(region.right, left, width);
            const int top = std::clamp<LONG>(region.top, 0, height);
            const int bottom = std::clamp<LONG>(region.bottom, top, height);
            for (int y = top; y < bottom; ++y)
                ZeroMemory(values + static_cast<size_t>(y) * width + left,
                    static_cast<size_t>(right - left) * sizeof(DWORD));
        }
        PaintWindowBackdrop(dc, region);
    }

    void FinalizeUiCanvasAlpha(void* pixels, int width, int height)
    {
        if (!g_windows11 || !pixels) return;
        auto* values = static_cast<DWORD*>(pixels);
        const size_t count = static_cast<size_t>(width) * height;
        for (size_t index = 0; index < count; ++index)
        {
            // GDI text does not populate the alpha channel. Keep genuinely
            // transparent black backdrop pixels at alpha 0 and make all GDI
            // content opaque; GDI+ pixels already carry the correct alpha.
            if ((values[index] & 0xff000000u) == 0 &&
                (values[index] & 0x00ffffffu) != 0)
                values[index] |= 0xff000000u;
        }
    }

    void FinalizeUiCanvasAlphaRegion(void* pixels, int width, int height,
        const RECT& region)
    {
        if (!g_windows11 || !pixels) return;
        auto* values = static_cast<DWORD*>(pixels);
        const int left = std::clamp<LONG>(region.left, 0, width);
        const int right = std::clamp<LONG>(region.right, left, width);
        const int top = std::clamp<LONG>(region.top, 0, height);
        const int bottom = std::clamp<LONG>(region.bottom, top, height);
        for (int y = top; y < bottom; ++y)
        {
            DWORD* row = values + static_cast<size_t>(y) * width;
            for (int x = left; x < right; ++x)
                if ((row[x] & 0xff000000u) == 0 && (row[x] & 0x00ffffffu) != 0)
                    row[x] |= 0xff000000u;
        }
    }

    bool UiPaintRegion(HDC destination, const RECT& client,
        bool cacheValid, RECT& region)
    {
        if (!cacheValid)
        {
            region = client;
            return true;
        }
        RECT clip{};
        if (GetClipBox(destination, &clip) == ERROR) clip = client;
        return IntersectRect(&region, &client, &clip) != FALSE;
    }

    void ApplyDpiChange(HWND window, WPARAM wParam, LPARAM lParam)
    {
        const UINT newDpi = HIWORD(wParam);
        const UINT oldDpi = static_cast<UINT>(reinterpret_cast<UINT_PTR>(
            GetPropW(window, kUiDpiProperty)));
        ScaleChildrenForDpi(window, oldDpi ? oldDpi : 96, newDpi);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        const int logicalWidth = static_cast<int>(reinterpret_cast<INT_PTR>(
            GetPropW(window, kUiLogicalWidthProperty)));
        const int logicalHeight = static_cast<int>(reinterpret_cast<INT_PTR>(
            GetPropW(window, kUiLogicalHeightProperty)));
        const int width = logicalWidth > 0
            ? MulDiv(logicalWidth, static_cast<int>(newDpi), 96)
            : suggested->right - suggested->left;
        const int height = logicalHeight > 0
            ? MulDiv(logicalHeight, static_cast<int>(newDpi), 96)
            : suggested->bottom - suggested->top;
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
            width, height,
            SWP_NOZORDER | SWP_NOACTIVATE);
        SetPropW(window, kUiDpiProperty,
            reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(newDpi)));
        ModernizeChildren(window);
        InvalidateRect(window, nullptr, FALSE);
    }

    void SizeWindowForCurrentDpi(HWND window, int logicalWidth, int logicalHeight)
    {
        if (!window) return;
        SetPropW(window, kUiLogicalWidthProperty,
            reinterpret_cast<HANDLE>(static_cast<INT_PTR>(logicalWidth)));
        SetPropW(window, kUiLogicalHeightProperty,
            reinterpret_cast<HANDLE>(static_cast<INT_PTR>(logicalHeight)));
        const UINT dpi = UiDpi(window);
        SetWindowPos(window, nullptr, 0, 0,
            MulDiv(logicalWidth, static_cast<int>(dpi), 96),
            MulDiv(logicalHeight, static_cast<int>(dpi), 96),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    bool IsAccentButton(int id)
    {
        return id == kHardwareApplyButtonId || id == kPeqEditorButtonId ||
            id == kVirtualBassEditorButtonId || id == kPeqApplyValueId ||
            id == kVirtualBassApplyId;
    }

    bool DrawModernButton(const DRAWITEMSTRUCT& item)
    {
        if (item.CtlType != ODT_BUTTON || !item.hwndItem) return false;
        RECT bounds{};
        GetClientRect(item.hwndItem, &bounds);
        HDC canvas = item.hDC;
        UiSurfaceCache* cache = BeginUiControlSurface(item.hwndItem,
            item.hDC, bounds, canvas, g_surfaceBrush);
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const float hoverAmount = UiHoverAmount(item.hwndItem);
        const float pressAmount = UiPressAmount(item.hwndItem);
        const bool focused = (item.itemState & ODS_FOCUS) != 0;
        const int id = GetDlgCtrlID(item.hwndItem);
        const bool accent = IsAccentButton(id);
        const bool danger = id == kHardwareRestoreButtonId || id == kPeqDeleteId;

        COLORREF fill = g_ui.surfaceAlt;
        COLORREF outline = g_ui.border;
        COLORREF text = g_ui.text;
        if (accent)
        {
            fill = BlendColor(g_ui.accent, g_ui.accentHover, hoverAmount);
            fill = BlendColor(fill,
                BlendColor(g_ui.accent, RGB(0, 0, 0), 0.24), pressAmount);
            outline = fill;
            text = RGB(255, 255, 255);
        }
        else if (danger && hoverAmount > 0.0f)
        {
            fill = BlendColor(fill,
                BlendColor(g_ui.danger, g_ui.background, g_ui.dark ? 0.72 : 0.88),
                hoverAmount);
            outline = BlendColor(outline, g_ui.danger, hoverAmount);
            text = BlendColor(text, g_ui.danger, hoverAmount);
        }
        else
        {
            fill = BlendColor(fill,
                BlendColor(g_ui.surfaceAlt, g_ui.text, g_ui.dark ? 0.10 : 0.05),
                hoverAmount);
            fill = BlendColor(fill,
                BlendColor(g_ui.surfaceAlt, g_ui.text, g_ui.dark ? 0.18 : 0.10),
                pressAmount);
        }
        if (disabled)
        {
            fill = BlendColor(g_ui.surfaceAlt, g_ui.background, 0.45);
            text = BlendColor(g_ui.muted, g_ui.background, 0.35);
            outline = g_ui.border;
        }

        Gdiplus::Graphics graphics(canvas);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const float inset = focused ? 2.0f : 1.0f;
        const Gdiplus::RectF rectangle(inset, inset,
            static_cast<float>(bounds.right - bounds.left) - inset * 2.0f,
            static_cast<float>(bounds.bottom - bounds.top) - inset * 2.0f);
        FillRounded(graphics, rectangle, static_cast<float>(UiScale(item.hwndItem, 7)), GdiColor(fill));
        StrokeRounded(graphics, rectangle, static_cast<float>(UiScale(item.hwndItem, 7)),
            GdiColor(focused ? g_ui.accent : outline), focused ? 2.0f : 1.0f);

        wchar_t label[256]{};
        GetWindowTextW(item.hwndItem, label, ARRAYSIZE(label));
        LOGFONTW logFont{};
        GetObjectW(UiFont(UiDpi(item.hwndItem), UiFontRole::Body), sizeof(logFont), &logFont);
        Gdiplus::Font font(canvas, &logFont);
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        Gdiplus::SolidBrush textBrush(GdiColor(text));
        Gdiplus::RectF textRect(0.0f,
            pressAmount * static_cast<float>(UiScale(item.hwndItem, 1)),
            static_cast<float>(bounds.right - bounds.left),
            static_cast<float>(bounds.bottom - bounds.top));
        graphics.DrawString(label, -1, &font, textRect, &format, &textBrush);
        graphics.Flush(Gdiplus::FlushIntentionSync);
        PresentUiControlSurface(item.hwndItem, item.hDC, cache, canvas,
            std::max<LONG>(bounds.right, 1), std::max<LONG>(bounds.bottom, 1));
        return true;
    }

    bool DrawModernChoiceItem(const DRAWITEMSTRUCT& item)
    {
        if ((item.CtlType != ODT_LISTBOX && item.CtlType != ODT_COMBOBOX) ||
            !item.hwndItem) return false;
        RECT bounds = item.rcItem;
        FillRect(item.hDC, &bounds, g_surfaceAltBrush);
        LRESULT effectiveItem = item.itemID;
        // DRAWITEMSTRUCT::itemID is an unsigned 32-bit value.  On x64, casting it
        // to LRESULT before comparing with -1 turns UINT_MAX into 4294967295 and
        // leaves the collapsed owner-draw combo with no label.
        if (item.itemID == static_cast<UINT>(-1) && item.CtlType == ODT_COMBOBOX)
            effectiveItem = SendMessageW(item.hwndItem, CB_GETCURSEL, 0, 0);
        if (effectiveItem < 0) return true;

        wchar_t label[512]{};
        if (item.CtlType == ODT_LISTBOX)
            SendMessageW(item.hwndItem, LB_GETTEXT, effectiveItem, reinterpret_cast<LPARAM>(label));
        else
            SendMessageW(item.hwndItem, CB_GETLBTEXT, effectiveItem, reinterpret_cast<LPARAM>(label));
        const bool selected = (item.itemState & ODS_SELECTED) != 0;
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const int leftPadding = item.CtlType == ODT_LISTBOX ? UiScale(item.hwndItem, 42) : UiScale(item.hwndItem, 10);

        if (selected)
        {
            Gdiplus::Graphics graphics(item.hDC);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::RectF selection(static_cast<float>(bounds.left + UiScale(item.hwndItem, 4)),
                static_cast<float>(bounds.top + UiScale(item.hwndItem, 2)),
                static_cast<float>(bounds.right - bounds.left - UiScale(item.hwndItem, 8)),
                static_cast<float>(bounds.bottom - bounds.top - UiScale(item.hwndItem, 4)));
            FillRounded(graphics, selection, static_cast<float>(UiScale(item.hwndItem, 6)),
                GdiColor(g_ui.accent, g_ui.dark ? 72 : 38));
        }

        if (item.CtlType == ODT_LISTBOX)
        {
            Gdiplus::Graphics graphics(item.hDC);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const float centerX = static_cast<float>(bounds.left + UiScale(item.hwndItem, 22));
            const float centerY = static_cast<float>((bounds.top + bounds.bottom) / 2);
            const float radius = static_cast<float>(UiScale(item.hwndItem, 8));
            Gdiplus::Pen ring(GdiColor(selected ? g_ui.accent : g_ui.muted), selected ? 2.2f : 1.4f);
            graphics.DrawEllipse(&ring, centerX - radius, centerY - radius, radius * 2.0f, radius * 2.0f);
            if (selected)
            {
                Gdiplus::SolidBrush dot(GdiColor(g_ui.accent));
                graphics.FillEllipse(&dot, centerX - radius + 3.0f, centerY - radius + 3.0f,
                    radius * 2.0f - 6.0f, radius * 2.0f - 6.0f);
            }
        }

        Gdiplus::Graphics textGraphics(item.hDC);
        textGraphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        LOGFONTW logFont{};
        GetObjectW(UiFont(UiDpi(item.hwndItem), UiFontRole::Body), sizeof(logFont), &logFont);
        Gdiplus::Font font(item.hDC, &logFont);
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentNear);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        Gdiplus::SolidBrush textBrush(GdiColor(disabled ? g_ui.muted : g_ui.text));
        Gdiplus::RectF textRect(static_cast<float>(bounds.left + leftPadding),
            static_cast<float>(bounds.top),
            static_cast<float>(bounds.right - bounds.left - leftPadding - UiScale(item.hwndItem, 8)),
            static_cast<float>(bounds.bottom - bounds.top));
        textGraphics.DrawString(label, -1, &font, textRect, &format, &textBrush);
        return true;
    }

    LRESULT ControlColor(UINT message, WPARAM wParam, LPARAM lParam)
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, IsWindowEnabled(control) ? g_ui.text : g_ui.muted);
        if (message == WM_CTLCOLORSTATIC)
        {
            const bool alternate = GetPropW(control, kUiSurfaceAltProperty) != nullptr;
            const COLORREF background = alternate ? g_ui.surfaceAlt : g_ui.surface;
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, background);
            return reinterpret_cast<LRESULT>(alternate ? g_surfaceAltBrush : g_surfaceBrush);
        }
        if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX)
        {
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, g_ui.surfaceAlt);
            return reinterpret_cast<LRESULT>(g_surfaceAltBrush);
        }
        if (message == WM_CTLCOLORBTN)
            return reinterpret_cast<LRESULT>(g_surfaceBrush);
        return reinterpret_cast<LRESULT>(GetStockObject(HOLLOW_BRUSH));
    }

    HICON CreateHxAudioIcon(int size, bool trackLifetime = true)
    {
        size = std::max(size, 16);
        HMODULE module = GetModuleHandleW(nullptr);
        // The embedded ICO contains individually prefiltered 16/24/32/48/64/
        // 128/256 px frames with finished alpha edges. Loading the exact frame
        // avoids DrawIconEx resampling a runtime-clipped bitmap and producing
        // dark stair-step fringes around the rounded corners.
        HICON icon = reinterpret_cast<HICON>(LoadImageW(module,
            MAKEINTRESOURCEW(IDI_HXAUDIO), IMAGE_ICON, size, size,
            LR_DEFAULTCOLOR));
        if (icon)
        {
            if (trackLifetime) g_uiIcons.push_back(icon);
            return icon;
        }

        // Retain the PNG path as a fallback for damaged/missing icon resources.
        HRSRC resource = FindResourceW(module,
            MAKEINTRESOURCEW(IDR_HXAUDIO_PNG), RT_RCDATA);
        HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
        const DWORD resourceSize = resource ? SizeofResource(module, resource) : 0;
        const void* resourceBytes = loaded ? LockResource(loaded) : nullptr;
        if (resourceBytes && resourceSize != 0)
        {
            HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, resourceSize);
            if (memory)
            {
                void* destination = GlobalLock(memory);
                if (destination) CopyMemory(destination, resourceBytes, resourceSize);
                GlobalUnlock(memory);
                ComPtr<IStream> stream;
                if (destination && SUCCEEDED(CreateStreamOnHGlobal(
                    memory, TRUE, &stream)))
                {
                    Gdiplus::Bitmap source(stream.Get(), FALSE);
                    Gdiplus::Bitmap roundedIcon(size, size,
                        PixelFormat32bppARGB);
                    if (source.GetLastStatus() == Gdiplus::Ok &&
                        roundedIcon.GetLastStatus() == Gdiplus::Ok)
                    {
                        Gdiplus::Graphics graphics(&roundedIcon);
                        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                        graphics.SetInterpolationMode(
                            Gdiplus::InterpolationModeHighQualityBicubic);
                        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                        graphics.SetCompositingMode(
                            Gdiplus::CompositingModeSourceCopy);
                        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
                        Gdiplus::GraphicsPath rounded;
                        const float inset = std::max(1.0f, size * 0.02f);
                        const float diameter = std::max(4.0f, size * 0.48f);
                        const float extent = size - inset * 2.0f;
                        rounded.AddArc(inset, inset, diameter, diameter, 180, 90);
                        rounded.AddArc(inset + extent - diameter, inset,
                            diameter, diameter, 270, 90);
                        rounded.AddArc(inset + extent - diameter,
                            inset + extent - diameter, diameter, diameter, 0, 90);
                        rounded.AddArc(inset, inset + extent - diameter,
                            diameter, diameter, 90, 90);
                        rounded.CloseFigure();
                        graphics.SetClip(&rounded);
                        graphics.DrawImage(&source,
                            Gdiplus::RectF(inset, inset, extent, extent),
                            0.0f, 0.0f,
                            static_cast<float>(source.GetWidth()),
                            static_cast<float>(source.GetHeight()),
                            Gdiplus::UnitPixel);
                        graphics.ResetClip();
                        graphics.Flush(Gdiplus::FlushIntentionSync);
                        roundedIcon.GetHICON(&icon);
                    }
                }
                else if (memory)
                {
                    GlobalFree(memory);
                }
            }
        }
        if (!icon)
            icon = reinterpret_cast<HICON>(LoadImageW(module,
                MAKEINTRESOURCEW(IDI_HXAUDIO), IMAGE_ICON, size, size,
                LR_DEFAULTCOLOR));
        if (icon && trackLifetime) g_uiIcons.push_back(icon);
        return icon;
    }

    int TrayIconPixelSize(HWND window)
    {
        const UINT dpi = window ? GetDpiForWindow(window) : USER_DEFAULT_SCREEN_DPI;
        // The notification area renders the small-icon metric in physical
        // pixels. Ask the ICO loader for that exact size so it selects the
        // prefiltered 16/24/32/48 frame instead of shrinking the 256 px frame.
        return std::max(16, std::max(
            GetSystemMetricsForDpi(SM_CXSMICON, dpi),
            GetSystemMetricsForDpi(SM_CYSMICON, dpi)));
    }

    bool ReplaceTrayIconImage(HWND window, DWORD operation)
    {
        HICON replacement = CreateHxAudioIcon(TrayIconPixelSize(window), false);
        if (!replacement) return false;
        HICON previous = g_ownedTrayIcon;
        g_trayIcon.hIcon = replacement;
        const bool changed = Shell_NotifyIconW(operation, &g_trayIcon) != FALSE;
        if (changed)
        {
            g_ownedTrayIcon = replacement;
            if (previous) DestroyIcon(previous);
        }
        else
        {
            g_trayIcon.hIcon = previous;
            DestroyIcon(replacement);
        }
        return changed;
    }

    bool TrayUiEnabled()
    {
        return !g_uiSmokeTest && !g_uiPreview && !g_mainUiPreview &&
            !g_mainUiSmoke && !g_virtualBassUiSmoke &&
            !g_virtualBassUiPreview;
    }

    void AddTrayIcon(HWND window)
    {
        if (!window || !TrayUiEnabled()) return;
        if (g_trayIcon.cbSize == 0)
        {
            g_trayIcon.cbSize = sizeof(g_trayIcon);
            g_trayIcon.hWnd = window;
            g_trayIcon.uID = kTrayIconId;
            g_trayIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP |
                NIF_SHOWTIP;
            g_trayIcon.uCallbackMessage = kTrayCallbackMessage;
            wcscpy_s(g_trayIcon.szTip, L"HXAudio Desktop");
        }
        if (!ReplaceTrayIconImage(window, NIM_ADD)) return;
        g_trayIcon.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_trayIcon);
    }

    void RefreshTrayIconImage(HWND window)
    {
        if (g_trayIcon.cbSize != 0)
            ReplaceTrayIconImage(window, NIM_MODIFY);
    }

    void RemoveTrayIcon()
    {
        if (g_trayIcon.cbSize != 0)
            Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
        if (g_ownedTrayIcon) DestroyIcon(g_ownedTrayIcon);
        g_ownedTrayIcon = nullptr;
        ZeroMemory(&g_trayIcon, sizeof(g_trayIcon));
    }

    void RestoreControlWindow(HWND window)
    {
        if (!window) return;
        ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(window);
    }

    void ShowTrayMenu(HWND window, POINT point)
    {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING | MF_DEFAULT, kTrayOpenButtonId,
            L"打开 HXAudio Desktop");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kTrayExitButtonId, L"退出控制面板");
        SetForegroundWindow(window);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN |
            TPM_LEFTALIGN, point.x, point.y, 0, window, nullptr);
        DestroyMenu(menu);
        PostMessageW(window, WM_NULL, 0, 0);
    }

    class UiRuntime
    {
    public:
        UiRuntime()
        {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            Gdiplus::GdiplusStartupInput input;
            Gdiplus::GdiplusStartup(&m_token, &input, nullptr);
            g_windows11 = IsWindows11OrGreater();
            RefreshUiPalette();
        }

        ~UiRuntime()
        {
            for (const auto& entry : g_uiFonts)
                if (entry.font) DeleteObject(entry.font);
            g_uiFonts.clear();
            for (HICON icon : g_uiIcons)
                if (icon) DestroyIcon(icon);
            g_uiIcons.clear();
            if (g_backgroundBrush) DeleteObject(g_backgroundBrush);
            if (g_surfaceBrush) DeleteObject(g_surfaceBrush);
            if (g_surfaceAltBrush) DeleteObject(g_surfaceAltBrush);
            g_backgroundBrush = nullptr;
            g_surfaceBrush = nullptr;
            g_surfaceAltBrush = nullptr;
            if (m_token) Gdiplus::GdiplusShutdown(m_token);
        }

    private:
        ULONG_PTR m_token = 0;
    };

    std::wstring ReadMachineRegistryString(const wchar_t* name)
    {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegPath, 0,
            KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return {};
        DWORD type = 0;
        DWORD size = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
            type != REG_SZ || size < sizeof(wchar_t))
        {
            RegCloseKey(key);
            return {};
        }
        std::vector<wchar_t> value(size / sizeof(wchar_t) + 1, L'\0');
        const LONG result = RegQueryValueExW(key, name, nullptr, &type,
            reinterpret_cast<BYTE*>(value.data()), &size);
        RegCloseKey(key);
        return result == ERROR_SUCCESS ? std::wstring(value.data()) : std::wstring();
    }

    bool ApoModeIsInstalled()
    {
        return _wcsicmp(ReadMachineRegistryString(L"InstallationMode").c_str(), L"APO") == 0;
    }

    DWORD NotifyApoDspSettingsChanged()
    {
        if (!ApoModeIsInstalled()) return 0;
        HKEY key = nullptr;
        DWORD disposition = 0;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kDspNotifyPath, 0, nullptr, 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr,
            &key, &disposition) != ERROR_SUCCESS) return 0;
        DWORD generation = 0;
        DWORD type = 0;
        DWORD size = sizeof(generation);
        if (RegQueryValueExW(key, L"Generation", nullptr, &type,
            reinterpret_cast<BYTE*>(&generation), &size) != ERROR_SUCCESS ||
            type != REG_DWORD || size != sizeof(generation)) generation = 0;
        ++generation;
        RegSetValueExW(key, L"Generation", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&generation), sizeof(generation));
        RegCloseKey(key);
        return generation;
    }

    bool IsApoSharedRegistryValue(const wchar_t* name)
    {
        return name && (_wcsnicmp(name, L"Dsp", 3) == 0 ||
            _wcsicmp(name, L"TargetEndpoints") == 0 ||
            _wcsicmp(name, L"TargetEndpointProfiles") == 0 ||
            _wcsicmp(name, L"ApoEndpointSelectionSerial") == 0);
    }

    void MirrorApoRegistryValue(const wchar_t* name, DWORD type,
        const BYTE* data, DWORD size)
    {
        if (!IsApoSharedRegistryValue(name) || !ApoModeIsInstalled()) return;
        HKEY key{};
        DWORD disposition{};
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegPath, 0, nullptr, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, &disposition) == ERROR_SUCCESS)
        {
            RegSetValueExW(key, name, 0, type, data, size);
            RegCloseKey(key);
        }
    }

    void DeleteMachineApoRegistryValue(const wchar_t* name)
    {
        if (!IsApoSharedRegistryValue(name) || !ApoModeIsInstalled()) return;
        HKEY key{};
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegPath, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS)
        {
            RegDeleteValueW(key, name);
            RegCloseKey(key);
        }
    }

    std::wstring ReadRegistryString(const wchar_t* name)
    {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &key) != ERROR_SUCCESS) return {};
        DWORD type = 0;
        DWORD size = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ || size < sizeof(wchar_t))
        {
            RegCloseKey(key);
            return {};
        }
        std::vector<wchar_t> value(size / sizeof(wchar_t) + 1, L'\0');
        const LONG result = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &size);
        RegCloseKey(key);
        return result == ERROR_SUCCESS ? std::wstring(value.data()) : std::wstring();
    }

    bool WriteRegistryString(const wchar_t* name, const std::wstring& value)
    {
        HKEY key{};
        DWORD disposition{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0, KEY_WRITE, nullptr, &key, &disposition) != ERROR_SUCCESS) return false;
        const LONG result = RegSetValueExW(key, name, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        if (result == ERROR_SUCCESS)
            MirrorApoRegistryValue(name, REG_SZ,
                reinterpret_cast<const BYTE*>(value.c_str()),
                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        return result == ERROR_SUCCESS;
    }

    std::vector<std::wstring> ReadRegistryMultiString(const wchar_t* name)
    {
        std::vector<std::wstring> result;
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &key) != ERROR_SUCCESS) return result;
        DWORD type = 0;
        DWORD size = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
            type == REG_MULTI_SZ && size >= 2 * sizeof(wchar_t))
        {
            std::vector<wchar_t> value(size / sizeof(wchar_t) + 2, L'\0');
            if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &size) == ERROR_SUCCESS)
            {
                for (const wchar_t* item = value.data(); *item; item += wcslen(item) + 1)
                {
                    result.emplace_back(item);
                }
            }
        }
        RegCloseKey(key);
        return result;
    }

    bool WriteRegistryMultiString(const wchar_t* name, const std::vector<std::wstring>& values)
    {
        size_t characters = 1;
        for (const auto& value : values) characters += value.size() + 1;
        characters = std::max<size_t>(characters, 2);
        std::vector<wchar_t> data(characters, L'\0');
        wchar_t* cursor = data.data();
        for (const auto& value : values)
        {
            CopyMemory(cursor, value.c_str(), value.size() * sizeof(wchar_t));
            cursor += value.size() + 1;
        }

        HKEY key{};
        DWORD disposition{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0, KEY_WRITE, nullptr, &key, &disposition) != ERROR_SUCCESS) return false;
        const LONG result = RegSetValueExW(key, name, 0, REG_MULTI_SZ,
            reinterpret_cast<const BYTE*>(data.data()),
            static_cast<DWORD>(data.size() * sizeof(wchar_t)));
        RegCloseKey(key);
        if (result == ERROR_SUCCESS)
            MirrorApoRegistryValue(name, REG_MULTI_SZ,
                reinterpret_cast<const BYTE*>(data.data()),
                static_cast<DWORD>(data.size() * sizeof(wchar_t)));
        return result == ERROR_SUCCESS;
    }

    bool RegistryValueIsType(const wchar_t* name, DWORD expectedType)
    {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
        DWORD type = 0;
        DWORD size = 0;
        const LONG result = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
        RegCloseKey(key);
        return result == ERROR_SUCCESS && type == expectedType;
    }

    DWORD ReadRegistryDword(const wchar_t* name, DWORD fallback)
    {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &key) != ERROR_SUCCESS) return fallback;
        DWORD type = 0;
        DWORD size = sizeof(DWORD);
        DWORD value = fallback;
        const LONG result = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
        RegCloseKey(key);
        return result == ERROR_SUCCESS && type == REG_DWORD ? value : fallback;
    }

    DWORD ReadMachineRegistryDword(const wchar_t* name, DWORD fallback)
    {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegPath, 0,
            KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return fallback;
        DWORD type = 0;
        DWORD size = sizeof(DWORD);
        DWORD value = fallback;
        const LONG result = RegQueryValueExW(key, name, nullptr, &type,
            reinterpret_cast<BYTE*>(&value), &size);
        RegCloseKey(key);
        return result == ERROR_SUCCESS && type == REG_DWORD ? value : fallback;
    }

    DWORD ReadApoRuntimeDword(const std::wstring& name, DWORD fallback)
    {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kApoRuntimeStatusPath, 0,
            KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return fallback;
        DWORD type = 0;
        DWORD size = sizeof(DWORD);
        DWORD value = fallback;
        const LONG result = RegQueryValueExW(key, name.c_str(), nullptr, &type,
            reinterpret_cast<BYTE*>(&value), &size);
        RegCloseKey(key);
        return result == ERROR_SUCCESS && type == REG_DWORD &&
            size == sizeof(DWORD) ? value : fallback;
    }

    std::wstring EngineBypassStatusText()
    {
        const DWORD state = ReadMachineRegistryDword(L"EngineBypassState", 0);
        const DWORD processes = ReadMachineRegistryDword(L"EngineBypassPatchedProcesses", 0);
        if (state == 1) return L" | Windows Limiter：绕过仍在运行(" +
            std::to_wstring(processes) + L")";
        if (state == 2) return L" | Windows Limiter：正在恢复";
        return L" | Windows Limiter：已启用";
    }

    void WriteRegistryDword(const wchar_t* name, DWORD value)
    {
        HKEY key{};
        DWORD disposition{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0, KEY_WRITE, nullptr, &key, &disposition) == ERROR_SUCCESS)
        {
            RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
            RegCloseKey(key);
            MirrorApoRegistryValue(name, REG_DWORD,
                reinterpret_cast<const BYTE*>(&value), sizeof(value));
        }
    }

    void DeleteRegistryValue(const wchar_t* name)
    {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
        {
            RegDeleteValueW(key, name);
            RegCloseKey(key);
            DeleteMachineApoRegistryValue(name);
        }
    }

    void DeleteRegistryValuesWithPrefixFromKey(HKEY key, const std::wstring& prefix)
    {
        DWORD index = 0;
        std::vector<wchar_t> name(16384, L'\0');
        while (true)
        {
            DWORD length = static_cast<DWORD>(name.size());
            const LONG result = RegEnumValueW(key, index, name.data(), &length,
                nullptr, nullptr, nullptr, nullptr);
            if (result == ERROR_NO_MORE_ITEMS) break;
            if (result != ERROR_SUCCESS)
            {
                ++index;
                continue;
            }

            const std::wstring valueName(name.data(), length);
            if (valueName.rfind(prefix, 0) == 0)
            {
                RegDeleteValueW(key, valueName.c_str());
            }
            else
            {
                ++index;
            }
        }
    }

    void DeleteRegistryValuesWithPrefix(const std::wstring& prefix)
    {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE, &key) == ERROR_SUCCESS)
        {
            DeleteRegistryValuesWithPrefixFromKey(key, prefix);
            RegCloseKey(key);
        }

        if (prefix.rfind(L"Dsp", 0) == 0 && ApoModeIsInstalled() &&
            RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegPath, 0,
                KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS)
        {
            DeleteRegistryValuesWithPrefixFromKey(key, prefix);
            RegCloseKey(key);
        }
    }

    int SyncApoDspSettings()
    {
        NormalizeStoredPeqGainLimits();
        HKEY user = nullptr;
        HKEY machine = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegPath, 0, nullptr, 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr,
            &machine, nullptr) != ERROR_SUCCESS) return 31;
        DeleteRegistryValuesWithPrefixFromKey(machine, L"Dsp");

        DWORD userDisposition = 0;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &user,
            &userDisposition) == ERROR_SUCCESS)
        {
            DWORD index = 0;
            std::vector<wchar_t> name(16384, L'\0');
            while (true)
            {
                DWORD nameLength = static_cast<DWORD>(name.size());
                DWORD type = 0;
                DWORD dataLength = 0;
                LONG result = RegEnumValueW(user, index++, name.data(), &nameLength,
                    nullptr, &type, nullptr, &dataLength);
                if (result == ERROR_NO_MORE_ITEMS) break;
                if (result != ERROR_SUCCESS && result != ERROR_MORE_DATA) continue;
                if (!IsApoSharedRegistryValue(name.data())) continue;
                std::vector<BYTE> data(dataLength);
                if (RegQueryValueExW(user, name.data(), nullptr, &type,
                    data.empty() ? nullptr : data.data(), &dataLength) == ERROR_SUCCESS)
                    RegSetValueExW(machine, name.data(), 0, type,
                        data.empty() ? nullptr : data.data(), dataLength);
            }
            DWORD userSerial = 0;
            DWORD machineSerial = 0;
            DWORD type = 0;
            DWORD size = sizeof(DWORD);
            RegQueryValueExW(user, L"ApoEndpointSelectionSerial", nullptr,
                &type, reinterpret_cast<BYTE*>(&userSerial), &size);
            type = 0;
            size = sizeof(DWORD);
            RegQueryValueExW(machine, L"ApoEndpointSelectionSerial", nullptr,
                &type, reinterpret_cast<BYTE*>(&machineSerial), &size);
            const DWORD nextSerial = std::max(userSerial, machineSerial) + 1;
            RegSetValueExW(user, L"ApoEndpointSelectionSerial", 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&nextSerial), sizeof(nextSerial));
            RegSetValueExW(machine, L"ApoEndpointSelectionSerial", 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&nextSerial), sizeof(nextSerial));
            RegCloseKey(user);
        }
        RegCloseKey(machine);
        NotifyApoDspSettingsChanged();
        return 0;
    }

    void SetAutoStart(bool enabled)
    {
        HKEY key{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
        if (enabled)
        {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
            const std::wstring command = L"\"" + std::wstring(path) + L"\" --background";
            RegSetValueExW(key, L"HXAudioVirtualRouter", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(command.c_str()),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
        else
        {
            RegDeleteValueW(key, L"HXAudioVirtualRouter");
        }
        RegCloseKey(key);
    }

    std::wstring DeviceId(IMMDevice* device)
    {
        wchar_t* id = nullptr;
        if (FAILED(device->GetId(&id))) return {};
        std::wstring result(id);
        CoTaskMemFree(id);
        return result;
    }

    std::wstring DeviceName(IMMDevice* device)
    {
        ComPtr<IPropertyStore> store;
        PROPVARIANT value;
        PropVariantInit(&value);
        std::wstring result;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) &&
            SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
            value.vt == VT_LPWSTR && value.pwszVal)
        {
            result = value.pwszVal;
        }
        PropVariantClear(&value);
        return result;
    }

    std::wstring DevicePropertyString(IMMDevice* device, const PROPERTYKEY& key)
    {
        ComPtr<IPropertyStore> store;
        PROPVARIANT value;
        PropVariantInit(&value);
        std::wstring result;
        if (device && SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) &&
            SUCCEEDED(store->GetValue(key, &value)))
        {
            if (value.vt == VT_LPWSTR && value.pwszVal) result = value.pwszVal;
            else if (value.vt == VT_BSTR && value.bstrVal) result = value.bstrVal;
        }
        PropVariantClear(&value);
        return result;
    }

    std::wstring DeviceProfileKey(IMMDevice* device)
    {
        if (!device) return {};
        const std::wstring name = DeviceName(device);
        ComPtr<IPropertyStore> store;
        PROPVARIANT container;
        PropVariantInit(&container);
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) &&
            SUCCEEDED(store->GetValue(PKEY_Device_ContainerId, &container)) &&
            container.vt == VT_CLSID && container.puuid)
        {
            wchar_t text[64]{};
            StringFromGUID2(*container.puuid, text, ARRAYSIZE(text));
            PropVariantClear(&container);
            return L"CONTAINER|" + std::wstring(text) + L"|" + name;
        }
        PropVariantClear(&container);

        const std::wstring instanceId = DevicePropertyString(device, PKEY_Device_InstanceId);
        if (!instanceId.empty() && instanceId.rfind(L"SWD\\MMDEVAPI\\", 0) != 0)
            return L"INSTANCE|" + instanceId + L"|" + name;
        return name.empty() ? L"ENDPOINT|" + DeviceId(device) : L"NAME|" + name;
    }

    bool IsVirtualName(const std::wstring& name)
    {
        return name.find(kVirtualName) != std::wstring::npos || name.find(L"HXAudio Virtual") != std::wstring::npos;
    }

    HRESULT FindVirtualDevice(IMMDeviceEnumerator* enumerator, ComPtr<IMMDevice>& result)
    {
        ComPtr<IMMDeviceCollection> collection;
        // Include hidden/disabled states so Stop can hide the virtual endpoint
        // to invalidate bound sessions and Start can find and show it again.
        RETURN_IF_FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, &collection));
        UINT count{};
        RETURN_IF_FAILED(collection->GetCount(&count));
        for (UINT i = 0; i < count; ++i)
        {
            ComPtr<IMMDevice> item;
            if (SUCCEEDED(collection->Item(i, &item)) && IsVirtualName(DeviceName(item.Get())))
            {
                result = item;
                return S_OK;
            }
        }
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    HRESULT EnumeratePhysicalDevices(IMMDeviceEnumerator* enumerator)
    {
        g_devices.clear();
        ComPtr<IMMDeviceCollection> collection;
        RETURN_IF_FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection));
        UINT count{};
        RETURN_IF_FAILED(collection->GetCount(&count));
        for (UINT i = 0; i < count; ++i)
        {
            ComPtr<IMMDevice> item;
            if (FAILED(collection->Item(i, &item))) continue;
            const std::wstring name = DeviceName(item.Get());
            const std::wstring id = DeviceId(item.Get());
            if (!id.empty() && !IsVirtualName(name))
            {
                DeviceInfo info{ id, name, DeviceProfileKey(item.Get()) };
                ComPtr<IAudioEndpointVolume> endpointVolume;
                if (SUCCEEDED(item->Activate(__uuidof(IAudioEndpointVolume),
                    CLSCTX_ALL, nullptr, &endpointVolume)))
                {
                    BOOL muted = FALSE;
                    if (SUCCEEDED(endpointVolume->GetMasterVolumeLevelScalar(
                            &info.volume)) &&
                        SUCCEEDED(endpointVolume->GetMute(&muted)))
                    {
                        info.volume = std::clamp(info.volume, 0.0f, 1.0f);
                        info.muted = muted != FALSE;
                        info.volumeKnown = true;
                    }
                }
                g_devices.push_back(std::move(info));
            }
        }
        std::sort(g_devices.begin(), g_devices.end(), [](const auto& a, const auto& b)
        {
            if (a.name != b.name) return a.name < b.name;
            return a.id < b.id;
        });
        return S_OK;
    }

    enum class HardwareControlKind : int
    {
        Amplifier = 0,
        Bass = 1,
        Midrange = 2,
        Treble = 3,
    };

    struct HardwareControl
    {
        HardwareControlKind kind{};
        UINT partLocalId = 0;
        UINT ordinal = 0;
        UINT channels = 0;
        float minimumDb = 0.0f;
        float maximumDb = 0.0f;
        ComPtr<IPerChannelDbLevel> level;
        ComPtr<IKsControl> ksControl;
    };

    struct HardwareOriginal
    {
        std::wstring deviceId;
        HardwareControlKind kind{};
        UINT ordinal = 0;
        UINT channel = 0;
        float levelDb = 0.0f;
    };

    struct HardwareCapabilitySummary
    {
        UINT devices = 0;
        UINT amplifierDevices = 0;
        UINT bassDevices = 0;
        UINT midrangeDevices = 0;
        UINT trebleDevices = 0;
        float maximumAmplifierDb = 0.0f;
    };

    struct EndpointHardwareIdentity
    {
        bool available = false;
        std::wstring endpointName;
        std::wstring chipModel;
        std::wstring hardwareVersion;
        std::wstring driverProvider;
        std::wstring driverVersion;
    };

    bool DevNodeProperty(DEVINST node, const DEVPROPKEY& key,
        DEVPROPTYPE& type, std::vector<BYTE>& data)
    {
        type = DEVPROP_TYPE_EMPTY;
        data.clear();
        ULONG size = 0;
        CONFIGRET result = CM_Get_DevNode_PropertyW(node, &key, &type,
            nullptr, &size, 0);
        if (result != CR_BUFFER_SMALL || size == 0) return false;
        data.resize(size + sizeof(wchar_t) * 2, 0);
        result = CM_Get_DevNode_PropertyW(node, &key, &type,
            data.data(), &size, 0);
        if (result != CR_SUCCESS)
        {
            data.clear();
            return false;
        }
        data.resize(size);
        return true;
    }

    std::wstring DevNodeString(DEVINST node, const DEVPROPKEY& key)
    {
        DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
        std::vector<BYTE> data;
        if (!DevNodeProperty(node, key, type, data) ||
            (type != DEVPROP_TYPE_STRING && type != DEVPROP_TYPE_STRING_INDIRECT) ||
            data.size() < sizeof(wchar_t)) return {};
        const wchar_t* text = reinterpret_cast<const wchar_t*>(data.data());
        return std::wstring(text, wcsnlen(text, data.size() / sizeof(wchar_t)));
    }

    std::vector<std::wstring> DevNodeStrings(DEVINST node,
        const DEVPROPKEY& key)
    {
        DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
        std::vector<BYTE> data;
        if (!DevNodeProperty(node, key, type, data) ||
            data.size() < sizeof(wchar_t)) return {};
        const wchar_t* begin = reinterpret_cast<const wchar_t*>(data.data());
        const size_t count = data.size() / sizeof(wchar_t);
        if (type == DEVPROP_TYPE_STRING || type == DEVPROP_TYPE_STRING_INDIRECT)
            return { std::wstring(begin, wcsnlen(begin, count)) };
        if (type != DEVPROP_TYPE_STRING_LIST) return {};
        std::vector<std::wstring> result;
        const wchar_t* end = begin + count;
        for (const wchar_t* item = begin; item < end && *item; )
        {
            const size_t remaining = static_cast<size_t>(end - item);
            const size_t length = wcsnlen(item, remaining);
            if (length == remaining) break;
            result.emplace_back(item, length);
            item += length + 1;
        }
        return result;
    }

    std::wstring HardwareIdField(const std::wstring& id,
        const wchar_t* field, size_t maximum = 16)
    {
        const size_t start = id.find(field);
        if (start == std::wstring::npos) return {};
        const size_t value = start + wcslen(field);
        size_t end = id.find_first_of(L"&\\", value);
        if (end == std::wstring::npos) end = id.size();
        end = std::min(end, value + maximum);
        return id.substr(value, end - value);
    }

    bool DecimalDigitsOnly(const std::wstring& value)
    {
        return !value.empty() && std::all_of(value.begin(), value.end(),
            [](wchar_t character) { return character >= L'0' && character <= L'9'; });
    }

    std::wstring ChipModelFromHardwareId(const std::wstring& description,
        const std::wstring& id)
    {
        const std::wstring vendor = HardwareIdField(id, L"VEN_", 4);
        const std::wstring device = HardwareIdField(id, L"DEV_", 4);
        const std::wstring vid = HardwareIdField(id, L"VID_", 4);
        const std::wstring pid = HardwareIdField(id, L"PID_", 4);
        std::wstring model = description;
        if (_wcsicmp(vendor.c_str(), L"10EC") == 0 && DecimalDigitsOnly(device))
        {
            const size_t first = device.find_first_not_of(L'0');
            const std::wstring number = first == std::wstring::npos
                ? L"0" : device.substr(first);
            model = L"Realtek ALC" + number;
        }
        std::wstring identifier;
        const size_t separator = id.find(L'\\');
        if (separator != std::wstring::npos) identifier = id.substr(0, separator);
        if (!vendor.empty()) identifier += L" VEN_" + vendor;
        if (!device.empty()) identifier += L" DEV_" + device;
        if (!vid.empty()) identifier += L" VID_" + vid;
        if (!pid.empty()) identifier += L" PID_" + pid;
        if (model.empty()) model = identifier.empty() ? id : identifier;
        else if (!identifier.empty()) model += L" [" + identifier + L"]";
        return model;
    }

    EndpointHardwareIdentity QueryEndpointHardwareIdentity(const std::wstring& endpointId)
    {
        EndpointHardwareIdentity identity;
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> endpoint;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
            FAILED(enumerator->GetDevice(endpointId.c_str(), &endpoint))) return identity;

        identity.endpointName = DeviceName(endpoint.Get());
        std::wstring instanceId = DevicePropertyString(endpoint.Get(),
            PKEY_Device_InstanceId);
        DEVINST node = 0;
        if (instanceId.empty() || CM_Locate_DevNodeW(&node,
            const_cast<wchar_t*>(instanceId.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
            return identity;
        if (instanceId.rfind(L"SWD\\MMDEVAPI\\", 0) == 0)
        {
            DEVINST parent = 0;
            if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) return identity;
            node = parent;
        }

        std::vector<std::wstring> hardwareIds = DevNodeStrings(node,
            DEVPKEY_Device_HardwareIds);
        if (hardwareIds.empty()) hardwareIds = DevNodeStrings(node,
            DEVPKEY_Device_CompatibleIds);
        wchar_t instance[MAX_DEVICE_ID_LEN]{};
        if (hardwareIds.empty() &&
            CM_Get_Device_IDW(node, instance, ARRAYSIZE(instance), 0) == CR_SUCCESS)
            hardwareIds.emplace_back(instance);
        const std::wstring primaryId = hardwareIds.empty() ? std::wstring() : hardwareIds.front();
        std::wstring description = DevNodeString(node, DEVPKEY_Device_BusReportedDeviceDesc);
        if (description.empty()) description = DevNodeString(node, DEVPKEY_Device_DeviceDesc);
        identity.chipModel = ChipModelFromHardwareId(description, primaryId);

        const std::wstring revision = HardwareIdField(primaryId, L"REV_", 16);
        const std::wstring subsystem = HardwareIdField(primaryId, L"SUBSYS_", 16);
        if (!revision.empty()) identity.hardwareVersion = L"REV_" + revision;
        if (!subsystem.empty())
        {
            if (!identity.hardwareVersion.empty()) identity.hardwareVersion += L" / ";
            identity.hardwareVersion += L"SUBSYS_" + subsystem;
        }
        if (identity.hardwareVersion.empty()) identity.hardwareVersion = L"驱动未公开硬件修订";
        identity.driverProvider = DevNodeString(node, DEVPKEY_Device_DriverProvider);
        identity.driverVersion = DevNodeString(node, DEVPKEY_Device_DriverVersion);
        identity.available = !identity.chipModel.empty();
        return identity;
    }

    void CollectTopologyParts(IPart* part, std::set<UINT>& visited,
        std::vector<ComPtr<IPart>>& parts, UINT depth)
    {
        if (!part || depth > 64) return;
        UINT localId = 0;
        if (FAILED(part->GetLocalId(&localId)) || !visited.insert(localId).second) return;

        ComPtr<IPart> retained = part;
        parts.push_back(std::move(retained));
        const auto collect = [&](IPartsList* connected)
        {
            if (!connected) return;
            UINT count = 0;
            if (FAILED(connected->GetCount(&count))) return;
            for (UINT index = 0; index < count; ++index)
            {
                ComPtr<IPart> child;
                if (SUCCEEDED(connected->GetPart(index, &child)))
                    CollectTopologyParts(child.Get(), visited, parts, depth + 1);
            }
        };
        ComPtr<IPartsList> incoming;
        if (SUCCEEDED(part->EnumPartsIncoming(&incoming))) collect(incoming.Get());
        ComPtr<IPartsList> outgoing;
        if (SUCCEEDED(part->EnumPartsOutgoing(&outgoing))) collect(outgoing.Get());
    }

    KSNODEPROPERTY_AUDIO_CHANNEL KsVolumeProperty(UINT nodeId,
        LONG channel, ULONG operation)
    {
        KSNODEPROPERTY_AUDIO_CHANNEL property{};
        property.NodeProperty.Property.Set = KSPROPSETID_Audio;
        property.NodeProperty.Property.Id = KSPROPERTY_AUDIO_VOLUMELEVEL;
        property.NodeProperty.Property.Flags = operation |
            KSPROPERTY_TYPE_TOPOLOGY;
        property.NodeProperty.NodeId = nodeId;
        property.Channel = channel;
        return property;
    }

    HRESULT KsHardwareGetLevelRange(IKsControl* control, UINT nodeId,
        UINT channel, float* minimumDb, float* maximumDb, float* steppingDb)
    {
        if (!control || !minimumDb || !maximumDb || !steppingDb)
            return E_INVALIDARG;
        auto property = KsVolumeProperty(nodeId, static_cast<LONG>(channel),
            KSPROPERTY_TYPE_BASICSUPPORT);
        std::array<BYTE, 1024> buffer{};
        ULONG returned = 0;
        HRESULT hr = control->KsProperty(
            reinterpret_cast<PKSPROPERTY>(&property), sizeof(property),
            buffer.data(), static_cast<ULONG>(buffer.size()), &returned);
        if (FAILED(hr) || returned < sizeof(KSPROPERTY_DESCRIPTION)) return hr;
        const auto* description = reinterpret_cast<const KSPROPERTY_DESCRIPTION*>(
            buffer.data());
        if ((description->AccessFlags & KSPROPERTY_TYPE_GET) == 0 ||
            (description->AccessFlags & KSPROPERTY_TYPE_SET) == 0)
            return E_ACCESSDENIED;
        const BYTE* cursor = buffer.data() + sizeof(KSPROPERTY_DESCRIPTION);
        const BYTE* end = buffer.data() + std::min<size_t>(returned,
            buffer.size());
        for (ULONG list = 0; list < description->MembersListCount; ++list)
        {
            if (cursor + sizeof(KSPROPERTY_MEMBERSHEADER) > end)
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            const auto* members = reinterpret_cast<
                const KSPROPERTY_MEMBERSHEADER*>(cursor);
            cursor += sizeof(KSPROPERTY_MEMBERSHEADER);
            const size_t bytes = static_cast<size_t>(members->MembersSize) *
                members->MembersCount;
            if (cursor + bytes > end)
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            if (members->MembersFlags == KSPROPERTY_MEMBER_STEPPEDRANGES &&
                members->MembersSize >= sizeof(KSPROPERTY_STEPPING_LONG) &&
                members->MembersCount != 0)
            {
                const auto* range = reinterpret_cast<
                    const KSPROPERTY_STEPPING_LONG*>(cursor);
                *minimumDb = range->Bounds.SignedMinimum / 65536.0f;
                *maximumDb = range->Bounds.SignedMaximum / 65536.0f;
                *steppingDb = range->SteppingDelta / 65536.0f;
                return S_OK;
            }
            if (members->MembersFlags == KSPROPERTY_MEMBER_RANGES &&
                members->MembersSize >= sizeof(KSPROPERTY_BOUNDS_LONG) &&
                members->MembersCount != 0)
            {
                const auto* range = reinterpret_cast<
                    const KSPROPERTY_BOUNDS_LONG*>(cursor);
                *minimumDb = range->SignedMinimum / 65536.0f;
                *maximumDb = range->SignedMaximum / 65536.0f;
                *steppingDb = 0.0f;
                return S_OK;
            }
            cursor += bytes;
        }
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    HRESULT HardwareGetLevelRange(const HardwareControl& control,
        UINT channel, float* minimumDb, float* maximumDb, float* steppingDb)
    {
        if (control.level)
            return control.level->GetLevelRange(channel, minimumDb,
                maximumDb, steppingDb);
        return KsHardwareGetLevelRange(control.ksControl.Get(),
            control.partLocalId, channel, minimumDb, maximumDb, steppingDb);
    }

    HRESULT HardwareGetLevel(const HardwareControl& control,
        UINT channel, float* levelDb)
    {
        if (!levelDb) return E_POINTER;
        if (control.level) return control.level->GetLevel(channel, levelDb);
        if (!control.ksControl) return E_NOINTERFACE;
        auto property = KsVolumeProperty(control.partLocalId,
            static_cast<LONG>(channel), KSPROPERTY_TYPE_GET);
        LONG level = 0;
        ULONG returned = 0;
        const HRESULT hr = control.ksControl->KsProperty(
            reinterpret_cast<PKSPROPERTY>(&property), sizeof(property),
            &level, sizeof(level), &returned);
        if (SUCCEEDED(hr) && returned >= sizeof(level))
            *levelDb = level / 65536.0f;
        return hr;
    }

    HRESULT HardwareSetLevel(const HardwareControl& control,
        UINT channel, float levelDb)
    {
        if (control.level)
            return control.level->SetLevel(channel, levelDb, nullptr);
        if (!control.ksControl) return E_NOINTERFACE;
        auto property = KsVolumeProperty(control.partLocalId,
            static_cast<LONG>(channel), KSPROPERTY_TYPE_SET);
        LONG level = static_cast<LONG>(std::llround(
            static_cast<double>(levelDb) * 65536.0));
        ULONG returned = 0;
        return control.ksControl->KsProperty(
            reinterpret_cast<PKSPROPERTY>(&property), sizeof(property),
            &level, sizeof(level), &returned);
    }

    bool TryAddKsAmplifierControl(IPart* part, UINT channels,
        UINT& ordinal, std::vector<HardwareControl>& controls)
    {
        if (!part || channels == 0) return false;
        ComPtr<IKsControl> ksControl;
        if (FAILED(part->Activate(CLSCTX_ALL, __uuidof(IKsControl),
            reinterpret_cast<void**>(ksControl.GetAddressOf())))) return false;
        UINT nodeId = 0;
        if (FAILED(part->GetLocalId(&nodeId))) return false;
        const UINT maximumChannels = std::min<UINT>(channels, 32);
        UINT supportedChannels = 0;
        float commonMinimum = -100000.0f;
        float commonMaximum = 100000.0f;
        for (UINT channel = 0; channel < maximumChannels; ++channel)
        {
            float minimum = 0.0f;
            float maximum = 0.0f;
            float stepping = 0.0f;
            float current = 0.0f;
            if (FAILED(KsHardwareGetLevelRange(ksControl.Get(), nodeId,
                channel, &minimum, &maximum, &stepping))) break;
            HardwareControl probe;
            probe.partLocalId = nodeId;
            probe.ksControl = ksControl;
            if (FAILED(HardwareGetLevel(probe, channel, &current))) break;
            commonMinimum = std::max(commonMinimum, minimum);
            commonMaximum = std::min(commonMaximum, maximum);
            ++supportedChannels;
        }
        if (supportedChannels == 0 || commonMaximum <= 0.001f ||
            commonMaximum < commonMinimum) return false;
        HardwareControl control;
        control.kind = HardwareControlKind::Amplifier;
        control.partLocalId = nodeId;
        control.ordinal = ordinal++;
        control.channels = supportedChannels;
        control.minimumDb = commonMinimum;
        control.maximumDb = commonMaximum;
        control.ksControl = std::move(ksControl);
        controls.push_back(std::move(control));
        return true;
    }

    template<typename T>
    bool TryAddHardwareControl(IPart* part, HardwareControlKind kind,
        UINT& ordinal, std::vector<HardwareControl>& controls)
    {
        ComPtr<T> typed;
        if (FAILED(part->Activate(CLSCTX_ALL, __uuidof(T), reinterpret_cast<void**>(typed.GetAddressOf())))) return false;
        ComPtr<IPerChannelDbLevel> level;
        if (FAILED(typed.As(&level))) return false;

        UINT channels = 0;
        if (FAILED(level->GetChannelCount(&channels)) || channels == 0) return false;
        float commonMinimum = -100000.0f;
        float commonMaximum = 100000.0f;
        for (UINT channel = 0; channel < channels; ++channel)
        {
            float minimum = 0.0f;
            float maximum = 0.0f;
            float stepping = 0.0f;
            if (FAILED(level->GetLevelRange(channel, &minimum, &maximum, &stepping))) return false;
            commonMinimum = std::max(commonMinimum, minimum);
            commonMaximum = std::min(commonMaximum, maximum);
        }
        if (commonMaximum < commonMinimum) return false;
        // A normal hardware attenuation node is not an extra amplifier. Only
        // advertise it when the physical driver reports real positive gain.
        if (kind == HardwareControlKind::Amplifier && commonMaximum <= 0.001f) return false;

        HardwareControl control;
        control.kind = kind;
        part->GetLocalId(&control.partLocalId);
        control.ordinal = ordinal++;
        control.channels = channels;
        control.minimumDb = commonMinimum;
        control.maximumDb = commonMaximum;
        control.level = std::move(level);
        controls.push_back(std::move(control));
        return true;
    }

    HRESULT EnumerateHardwareControls(IMMDeviceEnumerator* enumerator,
        const std::wstring& endpointId, std::vector<HardwareControl>& controls)
    {
        controls.clear();
        ComPtr<IMMDevice> endpoint;
        RETURN_IF_FAILED(enumerator->GetDevice(endpointId.c_str(), &endpoint));
        ComPtr<IDeviceTopology> topology;
        RETURN_IF_FAILED(endpoint->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, &topology));
        ComPtr<IConnector> endpointConnector;
        RETURN_IF_FAILED(topology->GetConnector(0, &endpointConnector));
        ComPtr<IConnector> adapterConnector;
        RETURN_IF_FAILED(endpointConnector->GetConnectedTo(&adapterConnector));
        ComPtr<IPart> root;
        RETURN_IF_FAILED(adapterConnector.As(&root));

        UINT endpointChannels = 2;
        ComPtr<IAudioClient> audioClient;
        WAVEFORMATEX* mixFormat = nullptr;
        if (SUCCEEDED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
            nullptr, &audioClient)) &&
            SUCCEEDED(audioClient->GetMixFormat(&mixFormat)) && mixFormat)
            endpointChannels = std::max<UINT>(1, mixFormat->nChannels);
        if (mixFormat) CoTaskMemFree(mixFormat);

        std::set<UINT> visited;
        std::vector<ComPtr<IPart>> parts;
        CollectTopologyParts(root.Get(), visited, parts, 0);
        UINT ordinals[4]{};
        for (auto& part : parts)
        {
            const bool standardAmplifier =
                TryAddHardwareControl<IAudioVolumeLevel>(part.Get(),
                    HardwareControlKind::Amplifier,
                    ordinals[static_cast<int>(HardwareControlKind::Amplifier)],
                    controls);
            if (!standardAmplifier)
                TryAddKsAmplifierControl(part.Get(), endpointChannels,
                    ordinals[static_cast<int>(HardwareControlKind::Amplifier)],
                    controls);
            TryAddHardwareControl<IAudioBass>(part.Get(), HardwareControlKind::Bass,
                ordinals[static_cast<int>(HardwareControlKind::Bass)], controls);
            TryAddHardwareControl<IAudioMidrange>(part.Get(), HardwareControlKind::Midrange,
                ordinals[static_cast<int>(HardwareControlKind::Midrange)], controls);
            TryAddHardwareControl<IAudioTreble>(part.Get(), HardwareControlKind::Treble,
                ordinals[static_cast<int>(HardwareControlKind::Treble)], controls);
        }
        return controls.empty() ? S_FALSE : S_OK;
    }

    std::vector<std::wstring> SplitTabs(const std::wstring& value)
    {
        std::vector<std::wstring> fields;
        size_t start = 0;
        while (start <= value.size())
        {
            const size_t separator = value.find(L'\t', start);
            fields.push_back(value.substr(start,
                separator == std::wstring::npos ? std::wstring::npos : separator - start));
            if (separator == std::wstring::npos) break;
            start = separator + 1;
        }
        return fields;
    }

    std::vector<HardwareOriginal> ReadHardwareOriginals()
    {
        std::vector<HardwareOriginal> result;
        for (const auto& encoded : ReadRegistryMultiString(L"HardwareOriginals"))
        {
            const auto fields = SplitTabs(encoded);
            if (fields.size() != 5) continue;
            HardwareOriginal item;
            item.deviceId = fields[0];
            item.kind = static_cast<HardwareControlKind>(_wtoi(fields[1].c_str()));
            item.ordinal = static_cast<UINT>(_wtoi(fields[2].c_str()));
            item.channel = static_cast<UINT>(_wtoi(fields[3].c_str()));
            wchar_t* end = nullptr;
            item.levelDb = wcstof(fields[4].c_str(), &end);
            if (item.deviceId.empty() || end == fields[4].c_str() ||
                static_cast<int>(item.kind) < 0 || static_cast<int>(item.kind) > 3) continue;
            result.push_back(std::move(item));
        }
        return result;
    }

    void WriteHardwareOriginals(const std::vector<HardwareOriginal>& originals)
    {
        if (originals.empty())
        {
            DeleteRegistryValue(L"HardwareOriginals");
            return;
        }
        std::vector<std::wstring> encoded;
        encoded.reserve(originals.size());
        for (const auto& item : originals)
        {
            encoded.push_back(item.deviceId + L"\t" +
                std::to_wstring(static_cast<int>(item.kind)) + L"\t" +
                std::to_wstring(item.ordinal) + L"\t" +
                std::to_wstring(item.channel) + L"\t" +
                std::to_wstring(item.levelDb));
        }
        WriteRegistryMultiString(L"HardwareOriginals", encoded);
    }

    bool HasHardwareOriginal(const std::vector<HardwareOriginal>& originals,
        const std::wstring& deviceId, HardwareControlKind kind, UINT ordinal, UINT channel)
    {
        return std::any_of(originals.begin(), originals.end(), [&](const auto& item)
        {
            return item.deviceId == deviceId && item.kind == kind &&
                item.ordinal == ordinal && item.channel == channel;
        });
    }

    float HardwareTargetDb(HardwareControlKind kind)
    {
        switch (kind)
        {
        case HardwareControlKind::Amplifier:
            return static_cast<float>(ReadRegistryDword(L"HardwareAmplifierTenths", 0)) / 10.0f;
        case HardwareControlKind::Bass:
            return (static_cast<int>(ReadRegistryDword(L"HardwareBassOffset", 120)) - 120) / 10.0f;
        case HardwareControlKind::Midrange:
            return (static_cast<int>(ReadRegistryDword(L"HardwareMidrangeOffset", 120)) - 120) / 10.0f;
        case HardwareControlKind::Treble:
            return (static_cast<int>(ReadRegistryDword(L"HardwareTrebleOffset", 120)) - 120) / 10.0f;
        }
        return 0.0f;
    }

    HRESULT ApplyHardwareSettings(const std::vector<std::wstring>& endpointIds)
    {
        if (ReadRegistryDword(L"HardwareEnabled", 0) == 0) return S_FALSE;
        ComPtr<IMMDeviceEnumerator> enumerator;
        RETURN_IF_FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator)));
        std::vector<HardwareOriginal> originals = ReadHardwareOriginals();
        UINT applied = 0;
        for (const auto& endpointId : endpointIds)
        {
            std::vector<HardwareControl> controls;
            if (FAILED(EnumerateHardwareControls(enumerator.Get(), endpointId, controls))) continue;
            for (auto& control : controls)
            {
                const float requested = HardwareTargetDb(control.kind);
                for (UINT channel = 0; channel < control.channels; ++channel)
                {
                    float minimum = 0.0f;
                    float maximum = 0.0f;
                    float stepping = 0.0f;
                    float current = 0.0f;
                    if (FAILED(HardwareGetLevelRange(control, channel,
                        &minimum, &maximum, &stepping)) ||
                        FAILED(HardwareGetLevel(control, channel, &current))) continue;
                    if (!HasHardwareOriginal(originals, endpointId, control.kind, control.ordinal, channel))
                    {
                        originals.push_back({ endpointId, control.kind, control.ordinal, channel, current });
                    }
                    float target = std::clamp(requested, minimum, maximum);
                    if (stepping > 0.000001f)
                    {
                        target = minimum + std::round((target - minimum) / stepping) * stepping;
                        target = std::clamp(target, minimum, maximum);
                    }
                    if (SUCCEEDED(HardwareSetLevel(control, channel, target))) ++applied;
                }
            }
        }
        WriteHardwareOriginals(originals);
        return applied > 0 ? S_OK : S_FALSE;
    }

    HRESULT RestoreHardwareOriginals(bool disableSettings)
    {
        std::vector<HardwareOriginal> remaining = ReadHardwareOriginals();
        if (remaining.empty())
        {
            if (disableSettings) WriteRegistryDword(L"HardwareEnabled", 0);
            return S_FALSE;
        }
        ComPtr<IMMDeviceEnumerator> enumerator;
        RETURN_IF_FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator)));
        std::set<std::wstring> deviceIds;
        for (const auto& item : remaining) deviceIds.insert(item.deviceId);
        for (const auto& deviceId : deviceIds)
        {
            std::vector<HardwareControl> controls;
            if (FAILED(EnumerateHardwareControls(enumerator.Get(), deviceId, controls))) continue;
            for (auto& control : controls)
            {
                for (auto it = remaining.begin(); it != remaining.end();)
                {
                    if (it->deviceId == deviceId && it->kind == control.kind &&
                        it->ordinal == control.ordinal && it->channel < control.channels &&
                        SUCCEEDED(HardwareSetLevel(control, it->channel,
                            it->levelDb)))
                    {
                        it = remaining.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
        }
        WriteHardwareOriginals(remaining);
        if (disableSettings) WriteRegistryDword(L"HardwareEnabled", 0);
        return remaining.empty() ? S_OK : S_FALSE;
    }

    HardwareCapabilitySummary ProbeHardwareCapabilities(const std::vector<std::wstring>& endpointIds)
    {
        HardwareCapabilitySummary summary;
        summary.devices = static_cast<UINT>(endpointIds.size());
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator)))) return summary;
        for (const auto& endpointId : endpointIds)
        {
            std::vector<HardwareControl> controls;
            if (FAILED(EnumerateHardwareControls(enumerator.Get(), endpointId, controls))) continue;
            bool amplifier = false;
            bool bass = false;
            bool midrange = false;
            bool treble = false;
            for (const auto& control : controls)
            {
                switch (control.kind)
                {
                case HardwareControlKind::Amplifier:
                    amplifier = true;
                    summary.maximumAmplifierDb = std::max(summary.maximumAmplifierDb, control.maximumDb);
                    break;
                case HardwareControlKind::Bass: bass = true; break;
                case HardwareControlKind::Midrange: midrange = true; break;
                case HardwareControlKind::Treble: treble = true; break;
                }
            }
            if (amplifier) ++summary.amplifierDevices;
            if (bass) ++summary.bassDevices;
            if (midrange) ++summary.midrangeDevices;
            if (treble) ++summary.trebleDevices;
        }
        return summary;
    }

    CpuTopology DetectCpuTopology()
    {
        CpuTopology topology;
        topology.physicalCores = 0;

        DWORD length = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && length > 0)
        {
            std::vector<BYTE> data(length);
            if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(data.data()), &length))
            {
                DWORD offset = 0;
                while (offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) <= length)
                {
                    const auto* item = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(data.data() + offset);
                    if (item->Size == 0 || offset + item->Size > length) break;
                    if (item->Relationship == RelationProcessorCore) ++topology.physicalCores;
                    offset += item->Size;
                }
            }
        }

        const WORD groupCount = GetActiveProcessorGroupCount();
        for (WORD group = 0; group < groupCount; ++group)
        {
            const DWORD count = GetActiveProcessorCount(group);
            for (DWORD number = 0; number < count && number <= UCHAR_MAX; ++number)
            {
                PROCESSOR_NUMBER processor{};
                processor.Group = group;
                processor.Number = static_cast<BYTE>(number);
                topology.logicalProcessors.push_back(processor);
            }
        }
        if (topology.logicalProcessors.empty())
        {
            PROCESSOR_NUMBER processor{};
            topology.logicalProcessors.push_back(processor);
        }
        if (topology.physicalCores == 0) topology.physicalCores = static_cast<DWORD>(topology.logicalProcessors.size());
        return topology;
    }

    void ConfigureRealtimeThread(size_t slot, const wchar_t* description)
    {
        if (!g_cpu.logicalProcessors.empty())
        {
            PROCESSOR_NUMBER processor = g_cpu.logicalProcessors[slot % g_cpu.logicalProcessors.size()];
            SetThreadIdealProcessorEx(GetCurrentThread(), &processor, nullptr);
        }
        SetThreadDescription(GetCurrentThread(), description);
    }

    struct MmcssRegistration
    {
        HANDLE handle = nullptr;
        MmcssRegistration()
        {
            DWORD taskIndex = 0;
            handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
            if (handle) AvSetMmThreadPriority(handle, AVRT_PRIORITY_HIGH);
        }
        ~MmcssRegistration()
        {
            if (handle) AvRevertMmThreadCharacteristics(handle);
        }
    };

    class ByteRing
    {
    public:
        ByteRing(size_t capacity, UINT32 blockAlign)
            : m_data(std::max<size_t>(capacity - (capacity % blockAlign), blockAlign)),
              m_blockAlign(blockAlign)
        {
            InitializeSRWLock(&m_lock);
        }

        void Clear()
        {
            AcquireSRWLockExclusive(&m_lock);
            m_read = m_write;
            m_available = 0;
            m_primed = false;
            ReleaseSRWLockExclusive(&m_lock);
        }

        void SetPrimeTarget(size_t byteCount)
        {
            byteCount -= byteCount % m_blockAlign;
            byteCount = std::clamp<size_t>(byteCount, m_blockAlign, m_data.size());
            AcquireSRWLockExclusive(&m_lock);
            m_primeTarget = byteCount;
            m_primed = m_available >= m_primeTarget;
            ReleaseSRWLockExclusive(&m_lock);
        }

        size_t AvailableBytes() const
        {
            AcquireSRWLockShared(&m_lock);
            const size_t available = m_available;
            ReleaseSRWLockShared(&m_lock);
            return available;
        }

        bool IsPrimed() const
        {
            AcquireSRWLockShared(&m_lock);
            const bool primed = m_primed;
            ReleaseSRWLockShared(&m_lock);
            return primed;
        }

        void Write(const BYTE* source, size_t byteCount)
        {
            byteCount -= byteCount % m_blockAlign;
            if (byteCount == 0) return;

            AcquireSRWLockExclusive(&m_lock);
            if (byteCount > m_data.size())
            {
                const size_t trim = byteCount - m_data.size();
                if (source) source += trim;
                byteCount = m_data.size();
            }
            if (m_available + byteCount > m_data.size())
            {
                const size_t discard = m_available + byteCount - m_data.size();
                m_read = (m_read + discard) % m_data.size();
                m_available -= discard;
            }

            const size_t first = std::min(byteCount, m_data.size() - m_write);
            if (source)
            {
                CopyMemory(m_data.data() + m_write, source, first);
                if (byteCount > first) CopyMemory(m_data.data(), source + first, byteCount - first);
            }
            else
            {
                ZeroMemory(m_data.data() + m_write, first);
                if (byteCount > first) ZeroMemory(m_data.data(), byteCount - first);
            }
            m_write = (m_write + byteCount) % m_data.size();
            m_available += byteCount;
            if (!m_primed && m_available >= m_primeTarget) m_primed = true;
            ReleaseSRWLockExclusive(&m_lock);
        }

        size_t Read(BYTE* destination, size_t byteCount)
        {
            byteCount -= byteCount % m_blockAlign;
            AcquireSRWLockExclusive(&m_lock);
            if (!m_primed)
            {
                ReleaseSRWLockExclusive(&m_lock);
                return 0;
            }
            // Once the renderer has completed its initial pre-roll, never
            // bounce back into an all-or-nothing priming cycle. Short system
            // sounds frequently put the two event callbacks a few frames out
            // of phase; copy the complete frames that exist and leave the
            // already-zeroed tail silent.
            const size_t copied = std::min(byteCount, m_available) -
                (std::min(byteCount, m_available) % m_blockAlign);
            if (copied == 0)
            {
                ReleaseSRWLockExclusive(&m_lock);
                return 0;
            }
            const size_t first = std::min(copied, m_data.size() - m_read);
            CopyMemory(destination, m_data.data() + m_read, first);
            if (copied > first) CopyMemory(destination + first, m_data.data(), copied - first);
            m_read = (m_read + copied) % m_data.size();
            m_available -= copied;
            ReleaseSRWLockExclusive(&m_lock);
            return copied;
        }

    private:
        mutable SRWLOCK m_lock{};
        std::vector<BYTE> m_data;
        size_t m_read = 0;
        size_t m_write = 0;
        size_t m_available = 0;
        size_t m_primeTarget = 1;
        bool m_primed = false;
        UINT32 m_blockAlign = 1;
    };

    UINT32 ChooseEnginePeriod(IAudioClient3* client, const WAVEFORMATEX* format, DWORD requestedMs)
    {
        UINT32 defaultFrames = 0;
        UINT32 fundamentalFrames = 0;
        UINT32 minimumFrames = 0;
        UINT32 maximumFrames = 0;
        if (FAILED(client->GetSharedModeEnginePeriod(format, &defaultFrames, &fundamentalFrames, &minimumFrames, &maximumFrames)) ||
            fundamentalFrames == 0 || minimumFrames == 0 || maximumFrames < minimumFrames)
        {
            return 0;
        }

        ULONGLONG requested = (static_cast<ULONGLONG>(format->nSamplesPerSec) * requestedMs + 999) / 1000;
        requested = std::max<ULONGLONG>(requested, minimumFrames);
        requested = std::min<ULONGLONG>(requested, maximumFrames);
        UINT32 frames = static_cast<UINT32>(requested);
        frames = ((frames + fundamentalFrames - 1) / fundamentalFrames) * fundamentalFrames;
        if (frames > maximumFrames)
        {
            frames = (maximumFrames / fundamentalFrames) * fundamentalFrames;
        }
        return std::max(frames, minimumFrames);
    }

    WORD EffectiveWaveFormatTag(const WAVEFORMATEX* format)
    {
        if (!format) return WAVE_FORMAT_UNKNOWN;
        if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
            format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        {
            return format->wFormatTag;
        }
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return static_cast<WORD>(extensible->SubFormat.Data1);
    }

    WAVEFORMATEXTENSIBLE BuildFloatBusFormat(const WAVEFORMATEX* source)
    {
        WAVEFORMATEXTENSIBLE result{};
        result.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        result.Format.nChannels = source->nChannels;
        result.Format.nSamplesPerSec = source->nSamplesPerSec;
        result.Format.wBitsPerSample = 32;
        result.Format.nBlockAlign = static_cast<WORD>(source->nChannels * sizeof(float));
        result.Format.nAvgBytesPerSec = result.Format.nSamplesPerSec * result.Format.nBlockAlign;
        result.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        result.Samples.wValidBitsPerSample = 32;
        if (source->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            source->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        {
            result.dwChannelMask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(source)->dwChannelMask;
        }
        else if (source->nChannels == 1)
        {
            result.dwChannelMask = 0x00000004; // SPEAKER_FRONT_CENTER
        }
        else if (source->nChannels == 2)
        {
            result.dwChannelMask = 0x00000003; // SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
        }
        result.SubFormat = { WAVE_FORMAT_IEEE_FLOAT, 0x0000, 0x0010,
            { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
        return result;
    }

    bool ConvertToFloatBus(const BYTE* source, UINT32 frames, const WAVEFORMATEX* format,
        std::vector<float>& destination)
    {
        const size_t sampleCount = static_cast<size_t>(frames) * format->nChannels;
        destination.resize(sampleCount);
        if (!source)
        {
            std::fill(destination.begin(), destination.end(), 0.0f);
            return true;
        }

        const WORD tag = EffectiveWaveFormatTag(format);
        if (tag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32)
        {
            CopyMemory(destination.data(), source, sampleCount * sizeof(float));
            return true;
        }
        if (tag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 64)
        {
            const auto* samples = reinterpret_cast<const double*>(source);
            for (size_t i = 0; i < sampleCount; ++i) destination[i] = static_cast<float>(samples[i]);
            return true;
        }
        if (tag != WAVE_FORMAT_PCM) return false;

        switch (format->wBitsPerSample)
        {
        case 8:
            for (size_t i = 0; i < sampleCount; ++i)
                destination[i] = (static_cast<int>(source[i]) - 128) / 128.0f;
            return true;
        case 16:
        {
            const auto* samples = reinterpret_cast<const int16_t*>(source);
            for (size_t i = 0; i < sampleCount; ++i) destination[i] = samples[i] / 32768.0f;
            return true;
        }
        case 24:
            for (size_t i = 0; i < sampleCount; ++i)
            {
                const BYTE* sample = source + i * 3;
                int32_t value = static_cast<int32_t>(sample[0]) |
                    (static_cast<int32_t>(sample[1]) << 8) |
                    (static_cast<int32_t>(sample[2]) << 16);
                if ((value & 0x00800000) != 0) value |= static_cast<int32_t>(0xFF000000);
                destination[i] = value / 8388608.0f;
            }
            return true;
        case 32:
        {
            const auto* samples = reinterpret_cast<const int32_t*>(source);
            for (size_t i = 0; i < sampleCount; ++i)
                destination[i] = static_cast<float>(samples[i] / 2147483648.0);
            return true;
        }
        default:
            return false;
        }
    }

    DWORD FloatPeakQ31(const std::vector<float>& samples)
    {
        double peak = 0.0;
        for (float sample : samples)
        {
            if (std::isfinite(sample)) peak = std::max(peak, std::abs(static_cast<double>(sample)));
        }
        if (peak >= 1.0) return 0x7fffffffu;
        return static_cast<DWORD>(peak * 2147483647.0);
    }

    DWORD FloatBytesPeakQ31(const BYTE* bytes, size_t byteCount)
    {
        if (!bytes || byteCount < sizeof(float)) return 0;
        const auto* samples = reinterpret_cast<const float*>(bytes);
        const size_t sampleCount = byteCount / sizeof(float);
        double peak = 0.0;
        for (size_t i = 0; i < sampleCount; ++i)
        {
            if (std::isfinite(samples[i]))
                peak = std::max(peak, std::abs(static_cast<double>(samples[i])));
        }
        if (peak >= 1.0) return 0x7fffffffu;
        return static_cast<DWORD>(peak * 2147483647.0);
    }

    struct FloatStatistics
    {
        DWORD rmsQ31 = 0;
        DWORD acRmsQ31 = 0;
        DWORD deltaRmsQ31 = 0;
        DWORD zeroCrossings = 0;
        DWORD nonFinite = 0;
        LONG meanQ31 = 0;
    };

    DWORD MagnitudeQ31(double value)
    {
        if (!std::isfinite(value)) return 0;
        return static_cast<DWORD>(std::clamp(std::abs(value), 0.0, 1.0) * 2147483647.0);
    }

    FloatStatistics MeasureFloatStatistics(const std::vector<float>& samples, UINT32 channels)
    {
        FloatStatistics result;
        if (samples.empty()) return result;
        const size_t channelStride = std::max<UINT32>(channels, 1);
        double sum = 0.0;
        double squareSum = 0.0;
        double deltaSquareSum = 0.0;
        size_t finiteCount = 0;
        size_t deltaCount = 0;
        for (size_t index = 0; index < samples.size(); ++index)
        {
            const double value = samples[index];
            if (!std::isfinite(value))
            {
                ++result.nonFinite;
                continue;
            }
            sum += value;
            squareSum += value * value;
            ++finiteCount;
            if (index >= channelStride && std::isfinite(samples[index - channelStride]))
            {
                const double previous = samples[index - channelStride];
                const double delta = value - previous;
                deltaSquareSum += delta * delta;
                ++deltaCount;
                if ((value < 0.0) != (previous < 0.0)) ++result.zeroCrossings;
            }
        }
        if (finiteCount == 0) return result;
        const double mean = sum / finiteCount;
        const double meanSquare = squareSum / finiteCount;
        result.rmsQ31 = MagnitudeQ31(std::sqrt(std::max(0.0, meanSquare)));
        result.acRmsQ31 = MagnitudeQ31(std::sqrt(std::max(0.0, meanSquare - mean * mean)));
        if (deltaCount != 0)
            result.deltaRmsQ31 = MagnitudeQ31(std::sqrt(deltaSquareSum / deltaCount));
        result.meanQ31 = static_cast<LONG>(std::clamp(mean, -1.0, 1.0) * 2147483647.0);
        return result;
    }

    enum class PeqFilterType
    {
        Peak,
        LowShelf,
        HighShelf,
        LowPass,
        HighPass,
        Notch
    };

    enum class VirtualBassMode
    {
        Harmonics234,
        Harmonics357,
        Custom
    };

    struct PeqBand
    {
        bool enabled = true;
        PeqFilterType type = PeqFilterType::Peak;
        double frequencyHz = 1000.0;
        double gainDb = 0.0;
        double q = 0.9;
    };

    struct DspSettings
    {
        bool virtualBassEnabled = false;
        double virtualBassAmount = 0.0;
        double virtualBassCutoffHz = 80.0;
        double virtualBassSubsonicHz = 18.0;
        VirtualBassMode virtualBassMode = VirtualBassMode::Harmonics234;
        std::array<double, 3> virtualBassMultipliers{ 2.0, 3.0, 4.0 };
        double preampDb = 0.0;
        std::vector<PeqBand> peqBands;
    };

    bool ValidVirtualBassMultiplier(double multiplier)
    {
        constexpr double pi = 3.14159265358979323846;
        return multiplier > 0.0 && std::isfinite(multiplier) &&
            std::isfinite(multiplier * pi);
    }

    std::array<double, 3> EffectiveVirtualBassMultipliers(
        const DspSettings& settings)
    {
        if (settings.virtualBassMode == VirtualBassMode::Harmonics357)
            return { 3.0, 5.0, 7.0 };
        if (settings.virtualBassMode == VirtualBassMode::Custom)
            return settings.virtualBassMultipliers;
        return { 2.0, 3.0, 4.0 };
    }

    std::vector<PeqBand> g_peqEditorBands;
    std::vector<DeviceInfo> g_peqEditorDevices;
    std::wstring g_peqEditorDeviceId;
    std::wstring g_peqEditorDeviceKey;
    bool g_peqEditorDirty = false;
    int g_peqSelectedBand = -1;
    bool g_peqDraggingBand = false;
    bool g_peqUpdatingFields = false;
    double g_peqViewMinHz = 10.0;
    double g_peqViewMaxHz = 24000.0;
    constexpr double kPeqDefaultViewDbRange =
        HXAudioDsp::kMaximumPeqGainDb;
    constexpr double kPeqMinimumViewDbRange = 1.0;
    double g_peqViewDbRange = kPeqDefaultViewDbRange;
    ULONGLONG g_peqLastLiveCommit = 0;
    bool g_virtualBassUpdating = false;
    bool g_virtualBassDirty = false;
    bool g_virtualBassDraggingCutoff = false;
    bool g_virtualBassDraggingSubsonic = false;
    ULONGLONG g_virtualBassLastLiveCommit = 0;

    int ReadOffsetTenths(const wchar_t* name, int midpoint, int minimum, int maximum)
    {
        const DWORD encoded = ReadRegistryDword(name, static_cast<DWORD>(midpoint));
        return std::clamp(static_cast<int>(encoded) - midpoint, minimum, maximum);
    }

    bool TryParseFiniteDouble(const std::wstring& text, double& value)
    {
        const wchar_t* cursor = text.c_str();
        while (*cursor && iswspace(*cursor)) ++cursor;
        if (!*cursor) return false;

        errno = 0;
        wchar_t* end = nullptr;
        const double parsed = wcstod(cursor, &end);
        if (end == cursor || !std::isfinite(parsed)) return false;
        while (*end && iswspace(*end)) ++end;
        if (*end) return false;
        value = parsed;
        return true;
    }

    std::wstring FormatParameter(double value)
    {
        std::wostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
        return stream.str();
    }

    const wchar_t* PeqTypeToken(PeqFilterType type)
    {
        switch (type)
        {
        case PeqFilterType::Peak: return L"PK";
        case PeqFilterType::LowShelf: return L"LS";
        case PeqFilterType::HighShelf: return L"HS";
        case PeqFilterType::LowPass: return L"LP";
        case PeqFilterType::HighPass: return L"HP";
        case PeqFilterType::Notch: return L"NO";
        }
        return L"PK";
    }

    const wchar_t* PeqTypeDisplayName(PeqFilterType type)
    {
        switch (type)
        {
        case PeqFilterType::Peak: return L"Peak";
        case PeqFilterType::LowShelf: return L"Low Shelf";
        case PeqFilterType::HighShelf: return L"High Shelf";
        case PeqFilterType::LowPass: return L"Low Pass";
        case PeqFilterType::HighPass: return L"High Pass";
        case PeqFilterType::Notch: return L"Notch";
        }
        return L"Peak";
    }

    bool TryParsePeqType(const std::wstring& text, PeqFilterType& type)
    {
        if (text == L"PK") type = PeqFilterType::Peak;
        else if (text == L"LS") type = PeqFilterType::LowShelf;
        else if (text == L"HS") type = PeqFilterType::HighShelf;
        else if (text == L"LP") type = PeqFilterType::LowPass;
        else if (text == L"HP") type = PeqFilterType::HighPass;
        else if (text == L"NO") type = PeqFilterType::Notch;
        else return false;
        return true;
    }

    std::vector<std::wstring> SplitPeqLine(const std::wstring& line)
    {
        std::vector<std::wstring> fields;
        size_t start = 0;
        while (start <= line.size())
        {
            const size_t separator = line.find(L'|', start);
            fields.push_back(line.substr(start,
                separator == std::wstring::npos ? std::wstring::npos : separator - start));
            if (separator == std::wstring::npos) break;
            start = separator + 1;
        }
        return fields;
    }

    bool TryParsePeqBand(const std::wstring& line, PeqBand& band)
    {
        const auto fields = SplitPeqLine(line);
        if (fields.size() != 5 || !TryParsePeqType(fields[1], band.type)) return false;
        if (fields[0] == L"1") band.enabled = true;
        else if (fields[0] == L"0") band.enabled = false;
        else return false;
        if (!TryParseFiniteDouble(fields[2], band.frequencyHz) || band.frequencyHz <= 0.0) return false;
        if (!TryParseFiniteDouble(fields[3], band.gainDb)) return false;
        band.gainDb = HXAudioDsp::ClampPeqGainDb(band.gainDb);
        if (!TryParseFiniteDouble(fields[4], band.q) || band.q <= 0.0) return false;
        return true;
    }

    std::wstring SerializePeqBand(const PeqBand& band)
    {
        return std::wstring(band.enabled ? L"1|" : L"0|") + PeqTypeToken(band.type) + L"|" +
            FormatParameter(band.frequencyHz) + L"|" +
            FormatParameter(HXAudioDsp::ClampPeqGainDb(band.gainDb)) + L"|" +
            FormatParameter(band.q);
    }

    bool TryParseLegacyPeqBand(const std::wstring& line, PeqBand& band)
    {
        const auto fields = SplitPeqLine(line);
        if (fields.size() != 5 || !TryParsePeqType(fields[1], band.type))
            return false;
        if (fields[0] == L"1") band.enabled = true;
        else if (fields[0] == L"0") band.enabled = false;
        else return false;
        return TryParseFiniteDouble(fields[2], band.frequencyHz) &&
            band.frequencyHz > 0.0 &&
            TryParseFiniteDouble(fields[3], band.gainDb) &&
            TryParseFiniteDouble(fields[4], band.q) && band.q > 0.0;
    }

    void NormalizeStoredPeqGainLimits()
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0,
            KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return;
        std::vector<std::wstring> valueNames;
        DWORD index = 0;
        std::vector<wchar_t> name(16384, L'\0');
        while (true)
        {
            DWORD length = static_cast<DWORD>(name.size());
            DWORD type = 0;
            const LONG result = RegEnumValueW(key, index++, name.data(),
                &length, nullptr, &type, nullptr, nullptr);
            if (result == ERROR_NO_MORE_ITEMS) break;
            if (result != ERROR_SUCCESS || type != REG_MULTI_SZ) continue;
            const std::wstring valueName(name.data(), length);
            if (valueName == L"DspPeqBands" ||
                valueName.rfind(L"DspPeqBands_", 0) == 0)
                valueNames.push_back(valueName);
        }
        RegCloseKey(key);

        for (const auto& valueName : valueNames)
        {
            const auto stored = ReadRegistryMultiString(valueName.c_str());
            std::vector<PeqBand> bands;
            bands.reserve(stored.size());
            double maximumMagnitude = 0.0;
            for (const auto& line : stored)
            {
                PeqBand band;
                if (!TryParseLegacyPeqBand(line, band)) continue;
                maximumMagnitude = std::max(maximumMagnitude,
                    std::abs(band.gainDb));
                bands.push_back(std::move(band));
            }
            const double scale = maximumMagnitude >
                HXAudioDsp::kMaximumPeqGainDb
                ? HXAudioDsp::kMaximumPeqGainDb / maximumMagnitude : 1.0;
            std::vector<std::wstring> normalized;
            normalized.reserve(bands.size());
            for (auto& band : bands)
            {
                band.gainDb = HXAudioDsp::ClampPeqGainDb(
                    band.gainDb * scale);
                normalized.push_back(SerializePeqBand(band));
            }
            if (normalized != stored)
                WriteRegistryMultiString(valueName.c_str(), normalized);
        }
        WriteRegistryDword(L"PeqGainLimitVersion", 40);
    }

    std::vector<PeqBand> LoadPeqBands()
    {
        std::vector<PeqBand> bands;
        if (RegistryValueIsType(L"DspPeqBands", REG_MULTI_SZ))
        {
            for (const auto& line : ReadRegistryMultiString(L"DspPeqBands"))
            {
                PeqBand band;
                if (TryParsePeqBand(line, band)) bands.push_back(band);
            }
            return bands;
        }

        // One-time compatibility view of the former fixed three-band EQ.  The
        // values become ordinary PEQ entries as soon as the new editor saves.
        bands.push_back({ true, PeqFilterType::LowShelf, 120.0,
            ReadOffsetTenths(L"DspBassOffset", 120, -120, 120) / 10.0, 0.7071067811865476 });
        bands.push_back({ true, PeqFilterType::Peak, 1000.0,
            ReadOffsetTenths(L"DspMidrangeOffset", 120, -120, 120) / 10.0, 0.9 });
        bands.push_back({ true, PeqFilterType::HighShelf, 8000.0,
            ReadOffsetTenths(L"DspTrebleOffset", 120, -120, 120) / 10.0, 0.7071067811865476 });
        return bands;
    }

    bool SavePeqBands(const std::vector<PeqBand>& bands)
    {
        std::vector<std::wstring> encoded;
        encoded.reserve(bands.size());
        for (const auto& band : bands) encoded.push_back(SerializePeqBand(band));
        return WriteRegistryMultiString(L"DspPeqBands", encoded);
    }

    std::wstring PeqDeviceRegistrySuffix(const std::wstring& deviceId)
    {
        uint64_t hash = 1469598103934665603ull;
        for (const wchar_t character : deviceId)
        {
            hash ^= static_cast<uint16_t>(character);
            hash *= 1099511628211ull;
        }
        std::wostringstream stream;
        stream << std::hex << std::uppercase << std::setw(16) << std::setfill(L'0') << hash;
        return stream.str();
    }

    std::vector<PeqBand> LoadPeqBandsForDevice(const std::wstring& profileKey,
        const std::wstring& legacyDeviceId = {})
    {
        if (profileKey.empty()) return LoadPeqBands();

        const std::wstring suffix = PeqDeviceRegistrySuffix(profileKey);
        const std::wstring idValue = L"DspPeqDeviceId_" + suffix;
        const std::wstring bandsValue = L"DspPeqBands_" + suffix;
        if (ReadRegistryString(idValue.c_str()) == profileKey &&
            RegistryValueIsType(bandsValue.c_str(), REG_MULTI_SZ))
        {
            std::vector<PeqBand> bands;
            for (const auto& line : ReadRegistryMultiString(bandsValue.c_str()))
            {
                PeqBand band;
                if (TryParsePeqBand(line, band)) bands.push_back(band);
            }
            return bands;
        }

        if (!legacyDeviceId.empty() && legacyDeviceId != profileKey)
        {
            const std::wstring legacySuffix = PeqDeviceRegistrySuffix(legacyDeviceId);
            const std::wstring legacyIdValue = L"DspPeqDeviceId_" + legacySuffix;
            const std::wstring legacyBandsValue = L"DspPeqBands_" + legacySuffix;
            if (ReadRegistryString(legacyIdValue.c_str()) == legacyDeviceId &&
                RegistryValueIsType(legacyBandsValue.c_str(), REG_MULTI_SZ))
            {
                std::vector<PeqBand> bands;
                for (const auto& line : ReadRegistryMultiString(legacyBandsValue.c_str()))
                {
                    PeqBand band;
                    if (TryParsePeqBand(line, band)) bands.push_back(band);
                }
                std::vector<std::wstring> encoded;
                encoded.reserve(bands.size());
                for (const auto& band : bands) encoded.push_back(SerializePeqBand(band));
                WriteRegistryMultiString(bandsValue.c_str(), encoded);
                WriteRegistryString(idValue.c_str(), profileKey);
                return bands;
            }
        }

        // Existing installations had one global curve. Until a device is
        // edited independently, it inherits that curve without changing it.
        return LoadPeqBands();
    }

    bool SavePeqBandsForDevice(const std::wstring& profileKey,
        const std::vector<PeqBand>& bands)
    {
        if (profileKey.empty()) return SavePeqBands(bands);

        std::vector<std::wstring> encoded;
        encoded.reserve(bands.size());
        for (const auto& band : bands) encoded.push_back(SerializePeqBand(band));

        const std::wstring suffix = PeqDeviceRegistrySuffix(profileKey);
        const std::wstring idValue = L"DspPeqDeviceId_" + suffix;
        const std::wstring bandsValue = L"DspPeqBands_" + suffix;
        return WriteRegistryMultiString(bandsValue.c_str(), encoded) &&
            WriteRegistryString(idValue.c_str(), profileKey);
    }

    DspSettings LoadDspSettings()
    {
        DspSettings settings;
        const DWORD legacyAmount = std::clamp<DWORD>(
            ReadRegistryDword(L"DspVirtualBassAmount", 0), 0, 200);
        settings.virtualBassEnabled = ReadRegistryDword(L"DspVirtualBassEnabled",
            legacyAmount != 0 ? 1 : 0) != 0;
        settings.virtualBassAmount = legacyAmount / 100.0;
        if (!TryParseFiniteDouble(ReadRegistryString(L"DspVirtualBassCutoffHzText"),
            settings.virtualBassCutoffHz) || settings.virtualBassCutoffHz <= 0.0)
        {
            settings.virtualBassCutoffHz = std::max<DWORD>(
                ReadRegistryDword(L"DspVirtualBassCutoffHz", 80), 1);
        }
        if (!TryParseFiniteDouble(ReadRegistryString(L"DspVirtualBassSubsonicHzText"),
            settings.virtualBassSubsonicHz) || settings.virtualBassSubsonicHz < 0.0)
        {
            settings.virtualBassSubsonicHz = ReadRegistryDword(L"DspVirtualBassSubsonicHz", 18);
        }
        switch (ReadRegistryDword(L"DspVirtualBassMode", 0))
        {
        case 1: settings.virtualBassMode = VirtualBassMode::Harmonics357; break;
        case 2: settings.virtualBassMode = VirtualBassMode::Custom; break;
        default: settings.virtualBassMode = VirtualBassMode::Harmonics234; break;
        }
        constexpr std::array<const wchar_t*, 3> multiplierNames{
            L"DspVirtualBassMultiplier1", L"DspVirtualBassMultiplier2",
            L"DspVirtualBassMultiplier3" };
        for (size_t index = 0; index < settings.virtualBassMultipliers.size(); ++index)
        {
            double multiplier = settings.virtualBassMultipliers[index];
            if (TryParseFiniteDouble(ReadRegistryString(multiplierNames[index]),
                multiplier) && ValidVirtualBassMultiplier(multiplier))
                settings.virtualBassMultipliers[index] = multiplier;
        }
        if (!TryParseFiniteDouble(ReadRegistryString(L"DspPreampDb"), settings.preampDb))
            settings.preampDb = ReadOffsetTenths(L"DspPreampOffset", 240, -240, 240) / 10.0;
        settings.preampDb = HXAudioDsp::ClampGainDb(settings.preampDb);
        settings.peqBands = LoadPeqBands();
        return settings;
    }

    struct BiquadCoefficients
    {
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
    };

    BiquadCoefficients NormalizeBiquad(
        double b0, double b1, double b2, double a0, double a1, double a2)
    {
        if (!std::isfinite(b0) || !std::isfinite(b1) || !std::isfinite(b2) ||
            !std::isfinite(a0) || !std::isfinite(a1) || !std::isfinite(a2) ||
            std::abs(a0) < std::numeric_limits<double>::min()) return {};
        BiquadCoefficients result{ b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
        if (!std::isfinite(result.b0) || !std::isfinite(result.b1) ||
            !std::isfinite(result.b2) || !std::isfinite(result.a1) ||
            !std::isfinite(result.a2)) return {};
        return result;
    }

    bool PeqFrequencyIsRepresentable(double sampleRate, double frequency, double q)
    {
        // There is no fixed UI or storage ceiling.  A sampled biquad can only
        // represent frequencies strictly below the current stream's Nyquist
        // frequency, so an out-of-band entry is preserved but is identity at
        // that sample rate rather than being silently moved to another point.
        return std::isfinite(sampleRate) && sampleRate > 0.0 &&
            std::isfinite(frequency) && frequency > 0.0 && frequency < sampleRate * 0.5 &&
            std::isfinite(q) && q > 0.0;
    }

    BiquadCoefficients MakePeaking(double sampleRate, double frequency, double q, double gainDb)
    {
        gainDb = HXAudioDsp::ClampPeqGainDb(gainDb);
        if (!PeqFrequencyIsRepresentable(sampleRate, frequency, q) ||
            std::abs(gainDb) < 0.00001) return {};
        constexpr double pi = 3.14159265358979323846;
        const double omega = 2.0 * pi * frequency / sampleRate;
        const double cosine = std::cos(omega);
        const double alpha = std::sin(omega) / (2.0 * q);
        const double amplitude = std::pow(10.0, gainDb / 40.0);
        return NormalizeBiquad(
            1.0 + alpha * amplitude,
            -2.0 * cosine,
            1.0 - alpha * amplitude,
            1.0 + alpha / amplitude,
            -2.0 * cosine,
            1.0 - alpha / amplitude);
    }

    BiquadCoefficients MakeLowPass(double sampleRate, double frequency, double q)
    {
        if (!PeqFrequencyIsRepresentable(sampleRate, frequency, q)) return {};
        constexpr double pi = 3.14159265358979323846;
        const double omega = 2.0 * pi * frequency / sampleRate;
        const double cosine = std::cos(omega);
        const double alpha = std::sin(omega) / (2.0 * q);
        return NormalizeBiquad(
            (1.0 - cosine) / 2.0,
            1.0 - cosine,
            (1.0 - cosine) / 2.0,
            1.0 + alpha,
            -2.0 * cosine,
            1.0 - alpha);
    }

    BiquadCoefficients MakeHighPass(double sampleRate, double frequency, double q)
    {
        if (!PeqFrequencyIsRepresentable(sampleRate, frequency, q)) return {};
        constexpr double pi = 3.14159265358979323846;
        const double omega = 2.0 * pi * frequency / sampleRate;
        const double cosine = std::cos(omega);
        const double alpha = std::sin(omega) / (2.0 * q);
        return NormalizeBiquad(
            (1.0 + cosine) / 2.0,
            -(1.0 + cosine),
            (1.0 + cosine) / 2.0,
            1.0 + alpha,
            -2.0 * cosine,
            1.0 - alpha);
    }

    BiquadCoefficients MakeLowShelf(double sampleRate, double frequency, double gainDb, double q)
    {
        gainDb = HXAudioDsp::ClampPeqGainDb(gainDb);
        if (!PeqFrequencyIsRepresentable(sampleRate, frequency, q) ||
            std::abs(gainDb) < 0.00001) return {};
        constexpr double pi = 3.14159265358979323846;
        const double amplitude = std::pow(10.0, gainDb / 40.0);
        if (!std::isfinite(amplitude) || amplitude <= 0.0) return {};
        const double omega = 2.0 * pi * frequency / sampleRate;
        const double cosine = std::cos(omega);
        const double alpha = std::sin(omega) / (2.0 * q);
        const double beta = 2.0 * std::sqrt(amplitude) * alpha;
        return NormalizeBiquad(
            amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine + beta),
            2.0 * amplitude * ((amplitude - 1.0) - (amplitude + 1.0) * cosine),
            amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine - beta),
            (amplitude + 1.0) + (amplitude - 1.0) * cosine + beta,
            -2.0 * ((amplitude - 1.0) + (amplitude + 1.0) * cosine),
            (amplitude + 1.0) + (amplitude - 1.0) * cosine - beta);
    }

    BiquadCoefficients MakeHighShelf(double sampleRate, double frequency, double gainDb, double q)
    {
        gainDb = HXAudioDsp::ClampPeqGainDb(gainDb);
        if (!PeqFrequencyIsRepresentable(sampleRate, frequency, q) ||
            std::abs(gainDb) < 0.00001) return {};
        constexpr double pi = 3.14159265358979323846;
        const double amplitude = std::pow(10.0, gainDb / 40.0);
        if (!std::isfinite(amplitude) || amplitude <= 0.0) return {};
        const double omega = 2.0 * pi * frequency / sampleRate;
        const double cosine = std::cos(omega);
        const double alpha = std::sin(omega) / (2.0 * q);
        const double beta = 2.0 * std::sqrt(amplitude) * alpha;
        return NormalizeBiquad(
            amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine + beta),
            -2.0 * amplitude * ((amplitude - 1.0) + (amplitude + 1.0) * cosine),
            amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine - beta),
            (amplitude + 1.0) - (amplitude - 1.0) * cosine + beta,
            2.0 * ((amplitude - 1.0) - (amplitude + 1.0) * cosine),
            (amplitude + 1.0) - (amplitude - 1.0) * cosine - beta);
    }

    BiquadCoefficients MakeNotch(double sampleRate, double frequency, double q)
    {
        if (!PeqFrequencyIsRepresentable(sampleRate, frequency, q)) return {};
        constexpr double pi = 3.14159265358979323846;
        const double omega = 2.0 * pi * frequency / sampleRate;
        const double cosine = std::cos(omega);
        const double alpha = std::sin(omega) / (2.0 * q);
        return NormalizeBiquad(
            1.0,
            -2.0 * cosine,
            1.0,
            1.0 + alpha,
            -2.0 * cosine,
            1.0 - alpha);
    }

    BiquadCoefficients MakePeqBand(double sampleRate, const PeqBand& band)
    {
        switch (band.type)
        {
        case PeqFilterType::Peak:
            return MakePeaking(sampleRate, band.frequencyHz, band.q, band.gainDb);
        case PeqFilterType::LowShelf:
            return MakeLowShelf(sampleRate, band.frequencyHz, band.gainDb, band.q);
        case PeqFilterType::HighShelf:
            return MakeHighShelf(sampleRate, band.frequencyHz, band.gainDb, band.q);
        case PeqFilterType::LowPass:
            return MakeLowPass(sampleRate, band.frequencyHz, band.q);
        case PeqFilterType::HighPass:
            return MakeHighPass(sampleRate, band.frequencyHz, band.q);
        case PeqFilterType::Notch:
            return MakeNotch(sampleRate, band.frequencyHz, band.q);
        }
        return {};
    }

    class Biquad
    {
    public:
        void Configure(const BiquadCoefficients& coefficients, UINT32 channels)
        {
            m_coefficients = coefficients;
            m_z1.assign(channels, 0.0);
            m_z2.assign(channels, 0.0);
        }

        float Process(float input, UINT32 channel)
        {
            const double output = m_coefficients.b0 * input + m_z1[channel];
            m_z1[channel] = m_coefficients.b1 * input - m_coefficients.a1 * output + m_z2[channel];
            m_z2[channel] = m_coefficients.b2 * input - m_coefficients.a2 * output;
            return static_cast<float>(output);
        }

    private:
        BiquadCoefficients m_coefficients;
        std::vector<double> m_z1;
        std::vector<double> m_z2;
    };

    class VirtualBassProcessor
    {
    public:
        // Called only by the owning audio thread; does not reset or allocate filters.
        void SetRealtimeAmount(double amount)
        {
            m_amount = std::isfinite(amount) ? std::clamp(amount, 0.0, 2.0) : 0.0;
        }

        void Configure(double sampleRate, UINT32 channels, const DspSettings& settings)
        {
            constexpr double butterworthQ = 0.7071067811865476;
            m_enabled = settings.virtualBassEnabled;
            m_amount = std::isfinite(settings.virtualBassAmount)
                ? std::clamp(settings.virtualBassAmount, 0.0, 2.0) : 0.0;
            m_mode = settings.virtualBassMode;
            m_multipliers = EffectiveVirtualBassMultipliers(settings);
            if (m_mode == VirtualBassMode::Custom)
            {
                constexpr double pi = 3.14159265358979323846;
                for (size_t harmonic = 0; harmonic < m_multipliers.size(); ++harmonic)
                {
                    const double multiplier = m_multipliers[harmonic];
                    const double atSilence = std::cos(multiplier * pi * 0.5);
                    for (size_t step = 0; step < kCustomTransferSize; ++step)
                    {
                        const double x = -1.0 + 2.0 * step /
                            static_cast<double>(kCustomTransferSize - 1);
                        m_customTransfer[harmonic][step] = static_cast<float>(
                            std::cos(multiplier * std::acos(x)) - atSilence);
                    }
                }
            }
            const double nyquistSafe = sampleRate * 0.45;
            const double cutoff = std::clamp(settings.virtualBassCutoffHz, 20.0, nyquistSafe);
            const double subsonic = std::clamp(settings.virtualBassSubsonicHz, 0.0,
                std::max(0.0, cutoff * 0.95));
            m_subsonicEnabled = subsonic > 0.0;

            // Two cascaded Butterworth sections make fourth-order Linkwitz-
            // Riley-style splits. The generator itself has no level detector,
            // compressor, attack or release state. Bridge applies its peak guard
            // after virtual bass, preamp and PEQ.
            m_subsonic1.Configure(m_subsonicEnabled
                ? MakeHighPass(sampleRate, subsonic, butterworthQ) : BiquadCoefficients{}, channels);
            m_subsonic2.Configure(m_subsonicEnabled
                ? MakeHighPass(sampleRate, subsonic, butterworthQ) : BiquadCoefficients{}, channels);
            m_lowPass1.Configure(MakeLowPass(sampleRate, cutoff, butterworthQ), channels);
            m_lowPass2.Configure(MakeLowPass(sampleRate, cutoff, butterworthQ), channels);
            m_dryHighPass1.Configure(MakeHighPass(sampleRate, cutoff, butterworthQ), channels);
            m_dryHighPass2.Configure(MakeHighPass(sampleRate, cutoff, butterworthQ), channels);
            m_dryHighPass3.Configure(MakeHighPass(sampleRate, cutoff, butterworthQ), channels);
            m_dryHighPass4.Configure(MakeHighPass(sampleRate, cutoff, butterworthQ), channels);
            m_harmonicHighPass1.Configure(MakeHighPass(sampleRate, cutoff, butterworthQ), channels);
            m_harmonicHighPass2.Configure(MakeHighPass(sampleRate, cutoff, butterworthQ), channels);
            m_harmonicHighPass3.Configure(MakeHighPass(sampleRate, cutoff, butterworthQ), channels);
            m_harmonicHighPass4.Configure(MakeHighPass(sampleRate, cutoff, butterworthQ), channels);
        }

        float Process(float input, UINT32 channel)
        {
            if (!m_enabled) return input;
            // Reject invalid source samples before they poison recursive filters.
            if (!std::isfinite(input)) input = 0.0f;

            float cleaned = m_subsonic1.Process(input, channel);
            cleaned = m_subsonic2.Process(cleaned, channel);
            if (m_amount <= 0.0) return cleaned;

            float low = cleaned;
            low = m_lowPass1.Process(low, channel);
            low = m_lowPass2.Process(low, channel);

            float high = m_dryHighPass1.Process(cleaned, channel);
            high = m_dryHighPass2.Process(high, channel);
            high = m_dryHighPass3.Process(high, channel);
            high = m_dryHighPass4.Process(high, channel);

            const double drive = 1.0 + 7.5 * m_amount;
            const double driven = static_cast<double>(low) * drive;
            // Smooth bounded excitation: Chebyshev polynomials are bounded only
            // on [-1, 1]. tanh is continuous with a continuous derivative and
            // retains harmonic generation without hard-clipping the waveform.
            const double x = std::tanh(driven);
            const double x2 = x * x;
            const double x3 = x2 * x;
            const double x4 = x2 * x2;
            double generated = 0.0;
            if (m_mode == VirtualBassMode::Harmonics357)
            {
                const double x5 = x4 * x;
                const double x7 = x5 * x2;
                const double h3 = 4.0 * x3 - 3.0 * x;
                const double h5 = 16.0 * x5 - 20.0 * x3 + 5.0 * x;
                const double h7 = 64.0 * x7 - 112.0 * x5 + 56.0 * x3 - 7.0 * x;
                generated = (0.62 * h3 + 0.28 * h5 + 0.10 * h7) / drive;
            }
            else if (m_mode == VirtualBassMode::Harmonics234)
            {
                const double h2 = 2.0 * x2;
                const double h3 = 4.0 * x3 - 3.0 * x;
                const double h4 = 8.0 * x4 - 8.0 * x2;
                generated = (0.58 * h2 + 0.29 * h3 + 0.13 * h4) / drive;
            }
            else
            {
                constexpr std::array<double, 3> weights{ 0.58, 0.29, 0.13 };
                for (size_t index = 0; index < m_multipliers.size(); ++index)
                {
                    constexpr double pi = 3.14159265358979323846;
                    const double multiplier = m_multipliers[index];
                    const double shaped = std::cos(multiplier *
                        (x + pi * 0.5)) -
                        std::cos(multiplier * pi * 0.5);
                    generated += weights[index] * shaped;
                }
                generated /= drive;
            }

            float harmonics = static_cast<float>(generated * m_amount * 1.25);
            harmonics = m_harmonicHighPass1.Process(harmonics, channel);
            harmonics = m_harmonicHighPass2.Process(harmonics, channel);
            harmonics = m_harmonicHighPass3.Process(harmonics, channel);
            harmonics = m_harmonicHighPass4.Process(harmonics, channel);
            const double replacementMix = std::min(m_amount, 1.0);
            const double dry = (1.0 - replacementMix) * cleaned + replacementMix * high;
            return static_cast<float>(dry + harmonics);
        }

    private:
        static constexpr size_t kCustomTransferSize = 2049;
        bool m_enabled = false;
        bool m_subsonicEnabled = false;
        double m_amount = 0.0;
        VirtualBassMode m_mode = VirtualBassMode::Harmonics234;
        std::array<double, 3> m_multipliers{ 2.0, 3.0, 4.0 };
        std::array<std::array<float, kCustomTransferSize>, 3> m_customTransfer{};
        Biquad m_subsonic1;
        Biquad m_subsonic2;
        Biquad m_lowPass1;
        Biquad m_lowPass2;
        Biquad m_dryHighPass1;
        Biquad m_dryHighPass2;
        Biquad m_dryHighPass3;
        Biquad m_dryHighPass4;
        Biquad m_harmonicHighPass1;
        Biquad m_harmonicHighPass2;
        Biquad m_harmonicHighPass3;
        Biquad m_harmonicHighPass4;
    };

    class DspProcessor
    {
    public:
        void Configure(UINT32 sampleRate, UINT32 channels, const DspSettings& settings)
        {
            m_channels = std::max<UINT32>(channels, 1);
            const double rate = std::max<double>(sampleRate, 8000.0);
            m_virtualBass.Configure(rate, m_channels, settings);

            m_peqFilters.clear();
            m_peqFilters.reserve(settings.peqBands.size());
            for (const auto& band : settings.peqBands)
            {
                if (!band.enabled) continue;
                const BiquadCoefficients configured = MakePeqBand(rate, band);
                Biquad filter;
                filter.Configure(configured, m_channels);
                m_peqFilters.push_back(std::move(filter));
            }
            m_staticHeadroomDb = 0.0;
            const double preampGain = std::pow(10.0,
                HXAudioDsp::ClampGainDb(settings.preampDb) / 20.0);
            m_preampGain = std::isfinite(preampGain) ? preampGain : 0.0;
        }

        void Process(float* samples, UINT32 frames)
        {
            if (!samples || m_channels == 0) return;
            for (UINT32 frame = 0; frame < frames; ++frame)
            {
                for (UINT32 channel = 0; channel < m_channels; ++channel)
                {
                    const size_t index = static_cast<size_t>(frame) * m_channels + channel;
                    float value = m_virtualBass.Process(samples[index], channel);
                    value = static_cast<float>(value * m_preampGain);
                    for (auto& filter : m_peqFilters) value = filter.Process(value, channel);
                    samples[index] = value;
                }
            }
        }

        double StaticHeadroomDb() const { return m_staticHeadroomDb; }

    private:
        UINT32 m_channels = 0;
        double m_preampGain = 1.0;
        double m_staticHeadroomDb = 0.0;
        VirtualBassProcessor m_virtualBass;
        std::vector<Biquad> m_peqFilters;
    };

    struct StaticGainOriginal
    {
        std::wstring profileKey;
        std::wstring endpointId;
        UINT partLocalId = 0;
        UINT ordinal = 0;
        UINT channel = 0;
        float levelDb = 0.0f;
    };

    struct StaticGainEndpoint
    {
        std::wstring endpointId;
        std::wstring profileKey;
    };

    struct StaticGainEndpointCandidate
    {
        std::wstring endpointId;
        std::wstring profileKey;
    };

    std::vector<StaticGainOriginal> ReadStaticGainOriginals()
    {
        std::vector<StaticGainOriginal> result;
        for (const auto& encoded : ReadRegistryMultiString(L"StaticGainOriginals"))
        {
            const auto fields = SplitTabs(encoded);
            if (fields.size() != 6) continue;
            StaticGainOriginal item;
            item.profileKey = fields[0];
            item.endpointId = fields[1];
            item.partLocalId = static_cast<UINT>(wcstoul(
                fields[2].c_str(), nullptr, 10));
            item.ordinal = static_cast<UINT>(wcstoul(
                fields[3].c_str(), nullptr, 10));
            item.channel = static_cast<UINT>(wcstoul(
                fields[4].c_str(), nullptr, 10));
            wchar_t* end = nullptr;
            item.levelDb = wcstof(fields[5].c_str(), &end);
            if (item.profileKey.empty() || item.endpointId.empty() ||
                end == fields[5].c_str() || !std::isfinite(item.levelDb)) continue;
            result.push_back(std::move(item));
        }
        return result;
    }

    void WriteStaticGainOriginals(
        const std::vector<StaticGainOriginal>& originals)
    {
        if (originals.empty())
        {
            DeleteRegistryValue(L"StaticGainOriginals");
            return;
        }
        std::vector<std::wstring> encoded;
        encoded.reserve(originals.size());
        for (const auto& item : originals)
        {
            encoded.push_back(item.profileKey + L"\t" + item.endpointId + L"\t" +
                std::to_wstring(item.partLocalId) + L"\t" +
                std::to_wstring(item.ordinal) + L"\t" +
                std::to_wstring(item.channel) + L"\t" +
                FormatParameter(item.levelDb));
        }
        WriteRegistryMultiString(L"StaticGainOriginals", encoded);
    }

    size_t FindStaticGainOriginal(
        const std::vector<StaticGainOriginal>& originals,
        const std::wstring& profileKey, const HardwareControl& control,
        UINT channel)
    {
        const auto exact = std::find_if(originals.begin(), originals.end(),
            [&](const StaticGainOriginal& item)
            {
                return item.profileKey == profileKey &&
                    item.partLocalId == control.partLocalId &&
                    item.ordinal == control.ordinal && item.channel == channel;
            });
        if (exact != originals.end())
            return static_cast<size_t>(exact - originals.begin());
        const auto ordinal = std::find_if(originals.begin(), originals.end(),
            [&](const StaticGainOriginal& item)
            {
                return item.profileKey == profileKey &&
                    item.ordinal == control.ordinal && item.channel == channel;
            });
        return ordinal == originals.end() ? std::numeric_limits<size_t>::max() :
            static_cast<size_t>(ordinal - originals.begin());
    }

    HardwareControl* FindStaticGainControl(
        std::vector<HardwareControl>& controls,
        const StaticGainOriginal& original)
    {
        auto exact = std::find_if(controls.begin(), controls.end(),
            [&](const HardwareControl& control)
            {
                return control.kind == HardwareControlKind::Amplifier &&
                    control.partLocalId == original.partLocalId &&
                    control.ordinal == original.ordinal;
            });
        if (exact != controls.end()) return &*exact;
        auto ordinal = std::find_if(controls.begin(), controls.end(),
            [&](const HardwareControl& control)
            {
                return control.kind == HardwareControlKind::Amplifier &&
                    control.ordinal == original.ordinal;
            });
        return ordinal == controls.end() ? nullptr : &*ordinal;
    }

    std::vector<StaticGainEndpointCandidate> EnumerateStaticGainCandidates(
        IMMDeviceEnumerator* enumerator, DWORD states)
    {
        std::vector<StaticGainEndpointCandidate> candidates;
        if (!enumerator) return candidates;
        ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator->EnumAudioEndpoints(eRender, states, &collection)))
            return candidates;
        UINT count = 0;
        if (FAILED(collection->GetCount(&count))) return candidates;
        for (UINT index = 0; index < count; ++index)
        {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(index, &device)) ||
                IsVirtualName(DeviceName(device.Get()))) continue;
            const std::wstring id = DeviceId(device.Get());
            if (id.empty()) continue;
            candidates.push_back({ id, DeviceProfileKey(device.Get()) });
        }
        return candidates;
    }

    std::wstring ResolveStaticGainEndpoint(IMMDeviceEnumerator* enumerator,
        const std::wstring& preferredId, const std::wstring& profileKey,
        DWORD states)
    {
        ComPtr<IMMDevice> preferred;
        if (!preferredId.empty() && enumerator &&
            SUCCEEDED(enumerator->GetDevice(preferredId.c_str(), &preferred)))
        {
            DWORD state = 0;
            if (SUCCEEDED(preferred->GetState(&state)) && (state & states) != 0 &&
                !IsVirtualName(DeviceName(preferred.Get())) &&
                (profileKey.empty() || DeviceProfileKey(preferred.Get()) == profileKey))
                return preferredId;
        }
        for (const auto& candidate : EnumerateStaticGainCandidates(enumerator, states))
        {
            if (!profileKey.empty() && candidate.profileKey == profileKey)
                return candidate.endpointId;
        }
        return {};
    }

    std::vector<StaticGainEndpoint> ResolveConfiguredStaticGainEndpoints(
        IMMDeviceEnumerator* enumerator, std::set<std::wstring>& desiredProfiles)
    {
        std::vector<StaticGainEndpoint> result;
        if (!enumerator) return result;
        const auto active = EnumerateStaticGainCandidates(
            enumerator, DEVICE_STATE_ACTIVE);

        const auto appendActive = [&](const std::wstring& id,
            const std::wstring& storedProfile)
        {
            std::wstring profile = storedProfile;
            auto candidate = std::find_if(active.begin(), active.end(),
                [&](const StaticGainEndpointCandidate& item)
                { return item.endpointId == id; });
            if (candidate == active.end() && !profile.empty())
            {
                candidate = std::find_if(active.begin(), active.end(),
                    [&](const StaticGainEndpointCandidate& item)
                    { return item.profileKey == profile; });
            }
            if (profile.empty() && candidate != active.end())
                profile = candidate->profileKey;
            if (profile.empty() && !id.empty()) profile = L"ENDPOINT|" + id;
            if (!profile.empty()) desiredProfiles.insert(profile);
            if (candidate == active.end() || profile.empty()) return;
            if (std::none_of(result.begin(), result.end(),
                [&](const StaticGainEndpoint& item)
                { return item.profileKey == profile; }))
                result.push_back({ candidate->endpointId, profile });
        };

        const bool targetsConfigured =
            RegistryValueIsType(L"TargetEndpoints", REG_MULTI_SZ) ||
            RegistryValueIsType(L"TargetEndpointProfiles", REG_MULTI_SZ);
        if (!targetsConfigured)
        {
            for (const auto& candidate : active)
            {
                if (candidate.profileKey.empty()) continue;
                desiredProfiles.insert(candidate.profileKey);
                result.push_back({ candidate.endpointId, candidate.profileKey });
            }
        }
        else
        {
            const auto ids = ReadRegistryMultiString(L"TargetEndpoints");
            const auto profiles = ReadRegistryMultiString(
                L"TargetEndpointProfiles");
            const size_t count = std::max(ids.size(), profiles.size());
            for (size_t index = 0; index < count; ++index)
                appendActive(index < ids.size() ? ids[index] : std::wstring(),
                    index < profiles.size() ? profiles[index] : std::wstring());
        }

        const auto playbackIds = ReadRegistryMultiString(L"PlaybackEndpoints");
        const auto playbackProfiles = ReadRegistryMultiString(
            L"PlaybackEndpointProfiles");
        const size_t playbackCount = std::max(playbackIds.size(),
            playbackProfiles.size());
        for (size_t index = 0; index < playbackCount; ++index)
            appendActive(index < playbackIds.size() ? playbackIds[index] :
                std::wstring(), index < playbackProfiles.size() ?
                playbackProfiles[index] : std::wstring());
        return result;
    }

    bool RestoreStaticGainEntries(IMMDeviceEnumerator* enumerator,
        std::vector<StaticGainOriginal>& originals,
        const std::set<std::wstring>* profilesToKeep)
    {
        bool changed = false;
        std::set<std::wstring> processedProfiles;
        const std::vector<StaticGainOriginal> snapshots = originals;
        for (const auto& snapshot : snapshots)
        {
            if (profilesToKeep && profilesToKeep->find(snapshot.profileKey) !=
                profilesToKeep->end()) continue;
            if (!processedProfiles.insert(snapshot.profileKey).second) continue;
            const std::wstring endpointId = ResolveStaticGainEndpoint(enumerator,
                snapshot.endpointId, snapshot.profileKey, DEVICE_STATEMASK_ALL);
            if (endpointId.empty()) continue;
            std::vector<HardwareControl> controls;
            if (FAILED(EnumerateHardwareControls(enumerator, endpointId, controls)))
                continue;
            for (auto it = originals.begin(); it != originals.end();)
            {
                if (it->profileKey != snapshot.profileKey)
                {
                    ++it;
                    continue;
                }
                HardwareControl* control = FindStaticGainControl(controls, *it);
                if (control && it->channel < control->channels &&
                    SUCCEEDED(control->level->SetLevel(
                        it->channel, it->levelDb, nullptr)))
                {
                    it = originals.erase(it);
                    changed = true;
                }
                else ++it;
            }
        }
        return changed;
    }

    void WriteStaticGainDiagnostic(const std::wstring& profileKey,
        double requestedDb, double appliedDb, HRESULT result)
    {
        const std::wstring suffix = PeqDeviceRegistrySuffix(profileKey);
        WriteRegistryString((L"StaticGainProfileId_" + suffix).c_str(),
            profileKey);
        const auto milliDb = [](double value)
        {
            const double rounded = static_cast<double>(std::llround(
                std::max(0.0, value) * 1000.0));
            return static_cast<DWORD>(std::clamp(
                rounded, 0.0, static_cast<double>(MAXDWORD)));
        };
        WriteRegistryDword((L"StaticGainRequestedMilliDb_" + suffix).c_str(),
            milliDb(requestedDb));
        WriteRegistryDword((L"StaticGainAppliedMilliDb_" + suffix).c_str(),
            milliDb(appliedDb));
        WriteRegistryDword((L"StaticGainUncompensatedMilliDb_" + suffix).c_str(),
            milliDb(std::max(0.0, requestedDb - appliedDb)));
        WriteRegistryDword((L"StaticGainLastError_" + suffix).c_str(),
            static_cast<DWORD>(result));
    }

    void EndpointMixFormat(IMMDeviceEnumerator* enumerator,
        const std::wstring& endpointId, UINT32& sampleRate, UINT32& channels)
    {
        sampleRate = 48000;
        channels = 2;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        WAVEFORMATEX* format = nullptr;
        if (enumerator && SUCCEEDED(enumerator->GetDevice(
            endpointId.c_str(), &device)) &&
            SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                nullptr, &client)) && SUCCEEDED(client->GetMixFormat(&format)) &&
            format)
        {
            sampleRate = std::max<UINT32>(format->nSamplesPerSec, 8000);
            channels = std::max<UINT32>(format->nChannels, 1);
        }
        if (format) CoTaskMemFree(format);
    }

    double CommonSteppedGain(double requested,
        const std::vector<float>& stepping, double available)
    {
        double gain = std::clamp(requested, 0.0, std::max(0.0, available));
        for (int pass = 0; pass < 32; ++pass)
        {
            const double previous = gain;
            for (const float step : stepping)
            {
                if (step > 0.000001f)
                    gain = std::floor((gain + 0.000001) / step) * step;
            }
            gain = std::clamp(gain, 0.0, std::max(0.0, available));
            if (std::abs(gain - previous) < 0.000001) break;
        }
        return gain;
    }

    HRESULT ApplyStaticHardwareCompensation(StaticGainApplyMode mode)
    {
        if (!ApoModeIsInstalled()) return S_FALSE;
        HANDLE mutex = CreateMutexW(nullptr, FALSE, kStaticGainMutex);
        if (!mutex) return HRESULT_FROM_WIN32(GetLastError());
        const DWORD wait = WaitForSingleObject(mutex, 5000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
        {
            CloseHandle(mutex);
            return HRESULT_FROM_WIN32(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT :
                GetLastError());
        }

        HRESULT overall = S_FALSE;
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        std::vector<StaticGainOriginal> originals = ReadStaticGainOriginals();
        if (SUCCEEDED(hr))
        {
            std::set<std::wstring> desiredProfiles;
            const auto endpoints = ResolveConfiguredStaticGainEndpoints(
                enumerator.Get(), desiredProfiles);
            RestoreStaticGainEntries(enumerator.Get(), originals,
                &desiredProfiles);

            std::vector<std::wstring> activeProfiles;
            for (const auto& endpoint : endpoints)
            {
                DspSettings settings = LoadDspSettings();
                settings.peqBands = LoadPeqBandsForDevice(
                    endpoint.profileKey, endpoint.endpointId);
                UINT32 sampleRate = 48000;
                UINT32 channels = 2;
                EndpointMixFormat(enumerator.Get(), endpoint.endpointId,
                    sampleRate, channels);
                DspProcessor processor;
                processor.Configure(sampleRate, channels, settings);
                const double requestedDb = std::max(0.0,
                    processor.StaticHeadroomDb());
                const std::wstring suffix = PeqDeviceRegistrySuffix(
                    endpoint.profileKey);
                const double previousAppliedDb = ReadRegistryDword(
                    (L"StaticGainAppliedMilliDb_" + suffix).c_str(), 0) /
                    1000.0;
                const bool hasOriginal = std::any_of(originals.begin(),
                    originals.end(), [&](const StaticGainOriginal& item)
                    { return item.profileKey == endpoint.profileKey; });
                activeProfiles.push_back(endpoint.profileKey);
                if (mode == StaticGainApplyMode::ReductionsOnly &&
                    requestedDb + 0.0005 >= previousAppliedDb &&
                    !(requestedDb <= 0.0005 && hasOriginal))
                    continue;

                if (requestedDb <= 0.0005)
                {
                    const std::set<std::wstring> keepOthers = [&]()
                    {
                        std::set<std::wstring> value = desiredProfiles;
                        value.erase(endpoint.profileKey);
                        return value;
                    }();
                    RestoreStaticGainEntries(enumerator.Get(), originals,
                        &keepOthers);
                    WriteStaticGainDiagnostic(endpoint.profileKey, 0.0, 0.0,
                        S_OK);
                    overall = S_OK;
                    continue;
                }

                std::vector<HardwareControl> controls;
                hr = EnumerateHardwareControls(enumerator.Get(),
                    endpoint.endpointId, controls);
                double appliedDb = 0.0;
                HRESULT endpointResult = hr;
                if (SUCCEEDED(hr))
                {
                    endpointResult = S_FALSE;
                    for (auto& control : controls)
                    {
                        if (control.kind != HardwareControlKind::Amplifier)
                            continue;
                        struct ChannelState
                        {
                            float minimum = 0.0f;
                            float maximum = 0.0f;
                            float stepping = 0.0f;
                            float original = 0.0f;
                        };
                        std::vector<ChannelState> states(control.channels);
                        std::vector<size_t> originalIndices(control.channels,
                            std::numeric_limits<size_t>::max());
                        bool complete = true;
                        double available = std::numeric_limits<double>::infinity();
                        std::vector<float> stepping;
                        stepping.reserve(control.channels);
                        for (UINT channel = 0; channel < control.channels; ++channel)
                        {
                            float current = 0.0f;
                            auto& state = states[channel];
                            if (FAILED(HardwareGetLevelRange(control, channel,
                                &state.minimum, &state.maximum, &state.stepping)))
                            {
                                complete = false;
                                break;
                            }
                            size_t originalIndex = FindStaticGainOriginal(originals,
                                endpoint.profileKey, control, channel);
                            if (originalIndex == std::numeric_limits<size_t>::max())
                            {
                                if (FAILED(HardwareGetLevel(control, channel,
                                    &current)))
                                {
                                    complete = false;
                                    break;
                                }
                                originals.push_back({ endpoint.profileKey,
                                    endpoint.endpointId, control.partLocalId,
                                    control.ordinal, channel, current });
                                originalIndex = originals.size() - 1;
                            }
                            else originals[originalIndex].endpointId =
                                endpoint.endpointId;
                            originalIndices[channel] = originalIndex;
                            state.original = originals[originalIndex].levelDb;
                            available = std::min(available,
                                static_cast<double>(state.maximum - state.original));
                            stepping.push_back(state.stepping);
                        }
                        if (!complete || !std::isfinite(available)) continue;

                        const double nodeGain = CommonSteppedGain(
                            requestedDb - appliedDb, stepping, available);
                        bool wrote = true;
                        for (UINT channel = 0; channel < control.channels; ++channel)
                        {
                            const float target = static_cast<float>(
                                states[channel].original + nodeGain);
                            if (FAILED(HardwareSetLevel(control, channel,
                                std::clamp(target, states[channel].minimum,
                                    states[channel].maximum))))
                                wrote = false;
                        }

                        double commonActual = std::numeric_limits<double>::infinity();
                        for (UINT channel = 0; channel < control.channels; ++channel)
                        {
                            float current = states[channel].original;
                            if (FAILED(HardwareGetLevel(control, channel,
                                &current)))
                            {
                                wrote = false;
                                commonActual = 0.0;
                                break;
                            }
                            commonActual = std::min(commonActual,
                                std::max(0.0, static_cast<double>(current -
                                    states[channel].original)));
                        }
                        if (!std::isfinite(commonActual)) commonActual = 0.0;
                        commonActual = CommonSteppedGain(commonActual,
                            stepping, available);
                        for (UINT channel = 0; channel < control.channels; ++channel)
                        {
                            const float target = static_cast<float>(
                                states[channel].original + commonActual);
                            if (FAILED(HardwareSetLevel(control, channel,
                                std::clamp(target, states[channel].minimum,
                                    states[channel].maximum))))
                                wrote = false;
                        }
                        appliedDb += commonActual;
                        if (wrote) endpointResult = S_OK;
                    }
                }
                WriteStaticGainDiagnostic(endpoint.profileKey, requestedDb,
                    appliedDb, endpointResult);
                if (SUCCEEDED(endpointResult)) overall = S_OK;
                else if (overall == S_FALSE) overall = endpointResult;
            }
            if (mode == StaticGainApplyMode::All)
                WriteRegistryMultiString(L"StaticGainActiveProfiles", activeProfiles);
            WriteStaticGainOriginals(originals);
        }
        else overall = hr;

        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return overall;
    }

    HRESULT RestoreStaticHardwareCompensation()
    {
        HANDLE mutex = CreateMutexW(nullptr, FALSE, kStaticGainMutex);
        if (!mutex) return HRESULT_FROM_WIN32(GetLastError());
        const DWORD wait = WaitForSingleObject(mutex, 5000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
        {
            CloseHandle(mutex);
            return HRESULT_FROM_WIN32(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT :
                GetLastError());
        }
        std::vector<StaticGainOriginal> originals = ReadStaticGainOriginals();
        HRESULT result = originals.empty() ? S_FALSE : S_OK;
        if (!originals.empty())
        {
            ComPtr<IMMDeviceEnumerator> enumerator;
            result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
            if (SUCCEEDED(result))
            {
                RestoreStaticGainEntries(enumerator.Get(), originals, nullptr);
                result = originals.empty() ? S_OK : S_FALSE;
            }
        }
        WriteStaticGainOriginals(originals);
        if (originals.empty())
            WriteRegistryMultiString(L"StaticGainActiveProfiles", {});
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return result;
    }

    bool ProcessIdIsAlive(DWORD processId)
    {
        if (processId == 0) return false;
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
        if (!process) return false;
        const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        CloseHandle(process);
        return alive;
    }

    void WaitForApoSettingsGeneration(DWORD generation, DWORD timeoutMs)
    {
        if (generation == 0 || timeoutMs == 0) return;
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return;
        std::set<std::wstring> desiredProfiles;
        const auto endpoints = ResolveConfiguredStaticGainEndpoints(
            enumerator.Get(), desiredProfiles);
        std::vector<std::wstring> liveProfiles;
        for (const auto& endpoint : endpoints)
        {
            const std::wstring suffix = PeqDeviceRegistrySuffix(
                endpoint.profileKey);
            const DWORD processId = ReadApoRuntimeDword(
                L"ApoSettingsProcessId_" + suffix, 0);
            if (ProcessIdIsAlive(processId))
                liveProfiles.push_back(endpoint.profileKey);
        }
        if (liveProfiles.empty()) return;

        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (true)
        {
            bool complete = true;
            for (const auto& profile : liveProfiles)
            {
                const std::wstring suffix = PeqDeviceRegistrySuffix(profile);
                if (ReadApoRuntimeDword(L"ApoSettingsGeneration_" + suffix,
                    0) != generation)
                {
                    complete = false;
                    break;
                }
            }
            if (complete || GetTickCount64() >= deadline) return;
            Sleep(2);
        }
    }

    int RunDspSelfTest()
    {
        {
            const PeqBand original{ false, PeqFilterType::HighShelf, 123456789.125,
                -14.25, 0.1234567890123456 };
            PeqBand decoded;
            if (!TryParsePeqBand(SerializePeqBand(original), decoded) ||
                decoded.enabled != original.enabled || decoded.type != original.type ||
                decoded.frequencyHz != original.frequencyHz || decoded.gainDb != original.gainDb ||
                decoded.q != original.q) return 9;
            if (!TryParsePeqBand(L"1|PK|1000|80|1", decoded) ||
                decoded.gainDb != HXAudioDsp::kMaximumPeqGainDb) return 25;
        }

        {
            DspSettings settings;
            DspProcessor processor;
            processor.Configure(48000, 2, settings);
            float samples[] = { -0.75f, 0.5f, 0.25f, -0.125f };
            const float original[] = { -0.75f, 0.5f, 0.25f, -0.125f };
            processor.Process(samples, 2);
            for (size_t i = 0; i < ARRAYSIZE(samples); ++i)
                if (samples[i] != original[i]) return 10;
        }

        {
            DspSettings settings;
            settings.preampDb = 20.0;
            DspProcessor processor;
            processor.Configure(48000, 1, settings);
            float sample = 0.5f;
            processor.Process(&sample, 1);
            if (!(sample > 4.99f && sample < 5.01f)) return 11;
        }

        {
            DspSettings settings;
            settings.peqBands.push_back({ true, PeqFilterType::Peak, 1000.0, 80.0, 1.0 });
            DspProcessor processor;
            processor.Configure(48000, 1, settings);
            std::vector<float> samples(4800);
            constexpr double pi = 3.14159265358979323846;
            for (size_t i = 0; i < samples.size(); ++i)
                samples[i] = static_cast<float>(0.98 * std::sin(2.0 * pi * 1000.0 * i / 48000.0));
            processor.Process(samples.data(), static_cast<UINT32>(samples.size()));
            double peak = 0.0;
            for (float sample : samples)
            {
                if (!std::isfinite(sample)) return 12;
                peak = std::max(peak, std::abs(static_cast<double>(sample)));
            }
            if (!(peak > 90.0 && peak < 105.0 && std::isfinite(peak))) return 13;
        }

        {
            DspSettings settings;
            for (int i = 0; i < 512; ++i)
                settings.peqBands.push_back({ true, PeqFilterType::Peak,
                    20.0 + i * 40.0, 0.0, 0.7 + i * 0.001 });
            settings.peqBands.push_back({ true, PeqFilterType::Peak, 1.0e12, 30.0, 1.0 });
            DspProcessor processor;
            processor.Configure(48000, 1, settings);
            float sample = 0.25f;
            processor.Process(&sample, 1);
            if (!std::isfinite(sample) || std::abs(sample - 0.25f) > 0.00001f) return 14;
        }

        {
            DspSettings settings;
            settings.peqBands = {
                { true, PeqFilterType::LowShelf, 90.0, 6.0, 0.707 },
                { true, PeqFilterType::Peak, 900.0, -4.0, 1.2 },
                { true, PeqFilterType::HighShelf, 7000.0, 3.0, 0.8 },
                { true, PeqFilterType::HighPass, 25.0, 0.0, 0.707 },
                { true, PeqFilterType::LowPass, 18000.0, 0.0, 0.707 },
                { true, PeqFilterType::Notch, 12000.0, 0.0, 3.0 }
            };
            DspProcessor processor;
            processor.Configure(48000, 2, settings);
            std::vector<float> impulse(4096 * 2, 0.0f);
            impulse[0] = 0.25f;
            impulse[1] = -0.25f;
            processor.Process(impulse.data(), 4096);
            double energy = 0.0;
            for (float sample : impulse)
            {
                if (!std::isfinite(sample)) return 15;
                energy += static_cast<double>(sample) * sample;
            }
            if (energy <= 0.0) return 16;
        }

        const auto toneAmplitude = [](const std::vector<float>& samples,
            size_t start, double frequency, double sampleRate)
        {
            constexpr double pi = 3.14159265358979323846;
            double real = 0.0;
            double imaginary = 0.0;
            const size_t count = samples.size() - start;
            for (size_t index = 0; index < count; ++index)
            {
                const double phase = 2.0 * pi * frequency * index / sampleRate;
                real += samples[start + index] * std::cos(phase);
                imaginary -= samples[start + index] * std::sin(phase);
            }
            return count == 0 ? 0.0 : 2.0 * std::hypot(real, imaginary) / count;
        };

        const auto processBassTone = [](VirtualBassMode mode, double inputFrequency,
            double amount, double replacementHz, double subsonicHz,
            std::array<double, 3> multipliers = { 2.0, 3.0, 4.0 },
            double inputAmplitude = 0.08)
        {
            constexpr double sampleRate = 48000.0;
            constexpr double pi = 3.14159265358979323846;
            DspSettings settings;
            settings.virtualBassEnabled = true;
            settings.virtualBassMode = mode;
            settings.virtualBassAmount = amount;
            settings.virtualBassCutoffHz = replacementHz;
            settings.virtualBassSubsonicHz = subsonicHz;
            settings.virtualBassMultipliers = multipliers;
            DspProcessor processor;
            processor.Configure(static_cast<UINT32>(sampleRate), 1, settings);
            std::vector<float> samples(96000);
            for (size_t index = 0; index < samples.size(); ++index)
                samples[index] = static_cast<float>(inputAmplitude *
                    std::sin(2.0 * pi * inputFrequency * index / sampleRate));
            processor.Process(samples.data(), static_cast<UINT32>(samples.size()));
            return samples;
        };

        {
            const auto samples = processBassTone(VirtualBassMode::Harmonics234,
                50.0, 1.0, 80.0, 15.0);
            const size_t start = samples.size() / 2;
            const double fundamental = toneAmplitude(samples, start, 50.0, 48000.0);
            const double second = toneAmplitude(samples, start, 100.0, 48000.0);
            const double third = toneAmplitude(samples, start, 150.0, 48000.0);
            const double fourth = toneAmplitude(samples, start, 200.0, 48000.0);
            if (!(second > fundamental * 1.5 && second > 0.002 &&
                third > 0.0002 && fourth > 0.0001)) return 17;
        }

        {
            const auto samples = processBassTone(VirtualBassMode::Harmonics357,
                50.0, 1.0, 80.0, 15.0);
            const size_t start = samples.size() / 2;
            const double fundamental = toneAmplitude(samples, start, 50.0, 48000.0);
            const double third = toneAmplitude(samples, start, 150.0, 48000.0);
            const double fifth = toneAmplitude(samples, start, 250.0, 48000.0);
            const double seventh = toneAmplitude(samples, start, 350.0, 48000.0);
            if (!(third > fundamental && third > 0.001 &&
                fifth > 0.0001 && seventh > 0.00001)) return 18;
        }

        {
            const auto samples = processBassTone(VirtualBassMode::Harmonics234,
                10.0, 0.0, 80.0, 25.0);
            const double remaining = toneAmplitude(samples, samples.size() / 2, 10.0, 48000.0);
            if (remaining >= 0.008) return 19;
        }

        {
            const auto samples = processBassTone(VirtualBassMode::Custom,
                50.0, 1.25, 80.0, 15.0, { 2.5, 3.75, 6.5 });
            double energy = 0.0;
            for (const float sample : samples)
            {
                if (!std::isfinite(sample)) return 21;
                energy += static_cast<double>(sample) * sample;
            }
            if (!(energy > 0.0)) return 22;
        }

        {
            const auto samples = processBassTone(VirtualBassMode::Harmonics357,
                50.0, 2.0, 80.0, 15.0, { 3.0, 5.0, 7.0 }, 0.98);
            double peak = 0.0;
            for (const float sample : samples)
            {
                if (!std::isfinite(sample)) return 23;
                peak = std::max(peak, std::abs(static_cast<double>(sample)));
            }
            if (!(peak > 0.0 && std::isfinite(peak))) return 24;
        }

        {
            constexpr double pi = 3.14159265358979323846;
            std::vector<float> boosted(48000);
            for (size_t index = 0; index < boosted.size(); ++index)
                boosted[index] = static_cast<float>(0.05 *
                    std::sin(2.0 * pi * 1000.0 * index / 48000.0));
            std::vector<float> reduced = boosted;

            DspSettings boostSettings;
            boostSettings.peqBands.push_back(
                { true, PeqFilterType::Peak, 1000.0, 12.0, 1.0 });
            DspSettings reduceSettings;
            reduceSettings.peqBands.push_back(
                { true, PeqFilterType::Peak, 1000.0, -12.0, 1.0 });
            DspProcessor boostProcessor;
            DspProcessor reduceProcessor;
            boostProcessor.Configure(48000, 1, boostSettings);
            reduceProcessor.Configure(48000, 1, reduceSettings);
            boostProcessor.Process(boosted.data(), static_cast<UINT32>(boosted.size()));
            reduceProcessor.Process(reduced.data(), static_cast<UINT32>(reduced.size()));

            const double boostedTone = toneAmplitude(boosted, boosted.size() / 2, 1000.0, 48000.0);
            const double reducedTone = toneAmplitude(reduced, reduced.size() / 2, 1000.0, 48000.0);
            if (!(boostedTone > reducedTone * 12.0 &&
                boostedTone > 0.18 && boostedTone < 0.22)) return 20;
        }
        return 0;
    }

    HRESULT ActivateSharedClient(IMMDevice* device, const WAVEFORMATEX* format, DWORD streamFlags,
        DWORD requestedMs, ComPtr<IAudioClient>& client, bool& eventDriven, UINT32& actualPeriodFrames)
    {
        eventDriven = false;
        actualPeriodFrames = 0;
        const DWORD eventFlags = streamFlags | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

        ComPtr<IAudioClient3> modern;
        HRESULT hr = device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &modern);
        if (SUCCEEDED(hr))
        {
            const UINT32 period = ChooseEnginePeriod(modern.Get(), format, requestedMs);
            if (period != 0)
            {
                hr = modern->InitializeSharedAudioStream(eventFlags, period, format, nullptr);
                if (SUCCEEDED(hr))
                {
                    RETURN_IF_FAILED(modern.As(&client));
                    eventDriven = true;
                    actualPeriodFrames = period;
                    return S_OK;
                }
            }
        }

        client.Reset();
        RETURN_IF_FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client));
        const REFERENCE_TIME duration = static_cast<REFERENCE_TIME>(std::clamp<DWORD>(requestedMs, 3, 100)) * 10000;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, eventFlags, duration, 0, format, nullptr);
        if (SUCCEEDED(hr))
        {
            eventDriven = true;
            return S_OK;
        }

        // A small number of older vendor endpoints reject event callbacks even
        // in shared mode.  Keep a timer-driven fallback for those devices.
        client.Reset();
        RETURN_IF_FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client));
        RETURN_IF_FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, duration, 0, format, nullptr));
        return S_OK;
    }

    HRESULT ConfigureRawSharedClient(IAudioClient2* client)
    {
        if (!client) return E_INVALIDARG;
        AudioClientProperties properties{};
        properties.cbSize = sizeof(properties);
        properties.bIsOffload = FALSE;
        properties.eCategory = AudioCategory_Media;
        properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
        return client->SetClientProperties(&properties);
    }

    HRESULT ActivateRawSharedClient(IMMDevice* device, const WAVEFORMATEX* format,
        DWORD requestedMs, ComPtr<IAudioClient>& client, bool& eventDriven,
        UINT32& actualPeriodFrames)
    {
        eventDriven = false;
        actualPeriodFrames = 0;

        ComPtr<IAudioClient3> modern;
        HRESULT hr = device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &modern);
        if (SUCCEEDED(hr)) hr = ConfigureRawSharedClient(modern.Get());
        if (SUCCEEDED(hr))
        {
            const UINT32 period = ChooseEnginePeriod(modern.Get(), format, requestedMs);
            if (period != 0)
            {
                hr = modern->InitializeSharedAudioStream(
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, format, nullptr);
                if (SUCCEEDED(hr))
                {
                    RETURN_IF_FAILED(modern.As(&client));
                    eventDriven = true;
                    actualPeriodFrames = period;
                    return S_OK;
                }
            }
        }

        ComPtr<IAudioClient2> raw;
        hr = device->Activate(__uuidof(IAudioClient2), CLSCTX_ALL, nullptr, &raw);
        if (SUCCEEDED(hr)) hr = ConfigureRawSharedClient(raw.Get());
        if (SUCCEEDED(hr)) hr = raw.As(&client);
        const REFERENCE_TIME duration = static_cast<REFERENCE_TIME>(
            std::clamp<DWORD>(requestedMs, 3, 100)) * 10000;
        if (SUCCEEDED(hr))
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration, 0, format, nullptr);
        if (SUCCEEDED(hr))
        {
            eventDriven = true;
            return S_OK;
        }

        client.Reset();
        raw.Reset();
        hr = device->Activate(__uuidof(IAudioClient2), CLSCTX_ALL, nullptr, &raw);
        if (SUCCEEDED(hr)) hr = ConfigureRawSharedClient(raw.Get());
        if (SUCCEEDED(hr)) hr = raw.As(&client);
        if (SUCCEEDED(hr))
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                duration, 0, format, nullptr);
        if (SUCCEEDED(hr)) return S_OK;

        // RAW is an endpoint capability, not a routing requirement. Bluetooth,
        // USB and a number of vendor endpoints legitimately return
        // AUDCLNT_E_RAW_MODE_UNSUPPORTED. A failed RAW negotiation must never
        // remove that endpoint (or the entire multi-output graph) from service.
        client.Reset();
        raw.Reset();
        return ActivateSharedClient(device, format,
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            requestedMs,
            client, eventDriven, actualPeriodFrames);
    }

    class TargetWorker
    {
    public:
        TargetWorker(std::wstring id, std::wstring name, std::wstring profileKey,
            const WAVEFORMATEX* format, DWORD bufferMs, HANDLE stopEvent,
            HANDLE failureEvent, size_t cpuSlot, bool applyPeq = true,
            bool allowProfileFallback = true, bool bridgePeakGuard = false)
            : m_id(std::move(id)), m_name(std::move(name)), m_bufferMs(bufferMs),
              m_stopEvent(stopEvent), m_failureEvent(failureEvent), m_cpuSlot(cpuSlot),
              m_profileKey(std::move(profileKey)),
              m_applyPeq(applyPeq), m_allowProfileFallback(allowProfileFallback),
              m_bridgePeakGuard(bridgePeakGuard),
              m_format(sizeof(WAVEFORMATEX) + format->cbSize),
              m_ring(std::max<size_t>(
                  static_cast<size_t>(format->nAvgBytesPerSec) * std::max<DWORD>(bufferMs * 8, 120) / 1000,
                  format->nBlockAlign * 4), format->nBlockAlign)
        {
            CopyMemory(m_format.data(), format, m_format.size());
            m_readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (m_profileKey.empty()) m_profileKey = L"ENDPOINT|" + m_id;
            if (m_applyPeq)
                ConfigurePeq(LoadPeqBandsForDevice(m_profileKey, m_id));
            else
                m_peqBypass = true;
        }

        ~TargetWorker()
        {
            if (m_thread) CloseHandle(m_thread);
            if (m_readyEvent) CloseHandle(m_readyEvent);
        }

        bool Start()
        {
            m_thread = CreateThread(nullptr, 0, ThreadEntry, this, 0, nullptr);
            return m_thread != nullptr;
        }

        void RestartIfNeeded()
        {
            if (!m_thread || WaitForSingleObject(m_thread, 0) != WAIT_OBJECT_0) return;
            CloseHandle(m_thread);
            m_thread = nullptr;
            ResetEvent(m_readyEvent);
            m_ring.Clear();
            m_sourceCache.clear();
            m_sourceStartFrame = 0;
            m_sourcePhase = 0.0;
            const std::vector<PeqBand> retainedBands = m_peqBands;
            ConfigurePeq(retainedBands);
            Start();
        }

        bool WaitUntilReady(DWORD timeoutMs)
        {
            return m_readyEvent && WaitForSingleObject(m_readyEvent, timeoutMs) == WAIT_OBJECT_0;
        }

        void Join()
        {
            if (m_thread) WaitForSingleObject(m_thread, INFINITE);
        }

        bool IsActive() const { return m_active.load(); }
        HRESULT Result() const { return m_result.load(); }
        DWORD LastPeakQ31() const { return m_lastPeakQ31.load(); }
        DWORD LastCopiedBytes() const { return m_lastCopiedBytes.load(); }
        unsigned long long ResamplerProtected() const { return m_resamplerProtected.load(); }
        unsigned long long UnderrunCallbacks() const { return m_underrunCallbacks.load(); }
        unsigned long long MissingFrames() const { return m_missingFrames.load(); }
        DWORD LastPaddingFrames() const { return m_lastPaddingFrames.load(); }
        DWORD RingBytes() const { return static_cast<DWORD>(std::min<size_t>(m_ring.AvailableBytes(), MAXDWORD)); }
        DWORD EndpointVolumeQ31() const { return m_endpointVolumeQ31.load(); }
        DWORD EndpointMuted() const { return m_endpointMuted.load(); }
        DWORD SessionVolumeQ31() const { return m_sessionVolumeQ31.load(); }
        DWORD SessionMuted() const { return m_sessionMuted.load(); }
        DWORD EndpointMeterQ31() const { return m_endpointMeterQ31.load(); }
        const std::wstring& Id() const { return m_id; }
        const std::wstring& ProfileKey() const { return m_profileKey; }
        void Clear() { m_ring.Clear(); }
        void ConfigurePeq(const std::vector<PeqBand>& bands)
        {
            if (!m_applyPeq)
            {
                m_peqBands.clear();
                m_peqBypass = true;
                return;
            }
            const auto* format = reinterpret_cast<const WAVEFORMATEX*>(m_format.data());
            if (!format) return;
            m_peqBands = bands;
            DspSettings settings;
            settings.peqBands = bands;
            m_peq.Configure(format->nSamplesPerSec, format->nChannels, settings);
            m_peqBypass = std::none_of(bands.begin(), bands.end(),
                [](const PeqBand& band) { return band.enabled; });
        }
        void Push(const float* source, UINT32 frames)
        {
            if (!m_active.load() || !source || frames == 0) return;
            const auto* format = reinterpret_cast<const WAVEFORMATEX*>(m_format.data());
            if (!format || format->nChannels == 0) return;
            const size_t samples = static_cast<size_t>(frames) * format->nChannels;
            const size_t bytes = static_cast<size_t>(frames) * format->nBlockAlign;
            if (m_peqBypass)
            {
                m_ring.Write(reinterpret_cast<const BYTE*>(source), bytes);
                return;
            }
            m_peqBuffer.assign(source, source + samples);
            m_peq.Process(m_peqBuffer.data(), frames);
            m_ring.Write(reinterpret_cast<const BYTE*>(m_peqBuffer.data()), bytes);
        }

    private:
        static DWORD WINAPI ThreadEntry(void* context)
        {
            return static_cast<TargetWorker*>(context)->Run();
        }

        size_t CachedFrames() const
        {
            const auto* format = reinterpret_cast<const WAVEFORMATEX*>(m_format.data());
            if (!format || format->nChannels == 0) return 0;
            const size_t totalFrames = m_sourceCache.size() / format->nChannels;
            return totalFrames > m_sourceStartFrame ? totalFrames - m_sourceStartFrame : 0;
        }

        void CompactSourceCache()
        {
            const auto* format = reinterpret_cast<const WAVEFORMATEX*>(m_format.data());
            if (!format || m_sourceStartFrame == 0) return;
            const size_t channels = format->nChannels;
            const size_t remainingFrames = CachedFrames();
            if (remainingFrames != 0)
            {
                MoveMemory(m_sourceCache.data(),
                    m_sourceCache.data() + m_sourceStartFrame * channels,
                    remainingFrames * channels * sizeof(float));
            }
            m_sourceCache.resize(remainingFrames * channels);
            m_sourceStartFrame = 0;
        }

        size_t FillSourceCache(size_t requiredFrames)
        {
            const auto* format = reinterpret_cast<const WAVEFORMATEX*>(m_format.data());
            if (!format || format->nChannels == 0) return 0;
            if (CachedFrames() >= requiredFrames) return CachedFrames();
            CompactSourceCache();

            const size_t channels = format->nChannels;
            while (CachedFrames() < requiredFrames)
            {
                const size_t oldFrames = CachedFrames();
                const size_t requestedFrames = requiredFrames - oldFrames;
                m_sourceCache.resize((oldFrames + requestedFrames) * channels);
                const size_t copiedBytes = m_ring.Read(
                    reinterpret_cast<BYTE*>(m_sourceCache.data() + oldFrames * channels),
                    requestedFrames * format->nBlockAlign);
                const size_t copiedFrames = copiedBytes / format->nBlockAlign;
                m_sourceCache.resize((oldFrames + copiedFrames) * channels);
                if (copiedFrames == 0) break;
            }
            return CachedFrames();
        }

        UINT32 RenderResampled(float* destination, UINT32 outputFrames,
            double sourceFramesPerOutput)
        {
            const auto* format = reinterpret_cast<const WAVEFORMATEX*>(m_format.data());
            if (!destination || !format || format->nChannels == 0 || outputFrames == 0) return 0;
            const size_t channels = format->nChannels;
            const double finalPhase = m_sourcePhase +
                static_cast<double>(outputFrames - 1) * sourceFramesPerOutput;
            const size_t requiredFrames = static_cast<size_t>(std::floor(finalPhase)) + 3;
            const size_t availableFrames = FillSourceCache(requiredFrames);
            const float* source = m_sourceCache.data() + m_sourceStartFrame * channels;

            UINT32 produced = 0;
            for (; produced < outputFrames; ++produced)
            {
                const size_t center = static_cast<size_t>(std::floor(m_sourcePhase));
                if (center + 2 >= availableFrames) break;
                const size_t previous = center == 0 ? 0 : center - 1;
                const double fraction = m_sourcePhase - static_cast<double>(center);
                for (size_t channel = 0; channel < channels; ++channel)
                {
                    const double y0 = source[previous * channels + channel];
                    const double y1 = source[center * channels + channel];
                    const double y2 = source[(center + 1) * channels + channel];
                    const double y3 = source[(center + 2) * channels + channel];
                    const double a = -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3;
                    const double b = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
                    const double c = -0.5 * y0 + 0.5 * y2;
                    destination[static_cast<size_t>(produced) * channels + channel] =
                        static_cast<float>(((a * fraction + b) * fraction + c) * fraction + y1);
                }
                m_sourcePhase += sourceFramesPerOutput;
            }

            const size_t wholeFrames = static_cast<size_t>(std::floor(m_sourcePhase));
            size_t consumedFrames = wholeFrames > 1 ? wholeFrames - 1 : 0;
            const size_t maximumConsumption = availableFrames > 3 ? availableFrames - 3 : 0;
            consumedFrames = std::min(consumedFrames, maximumConsumption);
            m_sourceStartFrame += consumedFrames;
            m_sourcePhase -= static_cast<double>(consumedFrames);
            return produced;
        }

        DWORD Run()
        {
            const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ConfigureRealtimeThread(m_cpuSlot, L"HXAudio output renderer");
            MmcssRegistration mmcss;

            HRESULT hr = S_OK;
            ComPtr<IMMDeviceEnumerator> enumerator;
            ComPtr<IMMDevice> device;
            ComPtr<IAudioClient> client;
            ComPtr<IAudioRenderClient> render;
            ComPtr<IAudioEndpointVolume> endpointVolume;
            ComPtr<IAudioMeterInformation> endpointMeter;
            ComPtr<ISimpleAudioVolume> sessionVolume;
            HANDLE audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            bool eventDriven = false;
            UINT32 periodFrames = 0;
            UINT32 bufferFrames = 0;
            const auto* format = reinterpret_cast<const WAVEFORMATEX*>(m_format.data());

            if (!audioEvent) hr = HRESULT_FROM_WIN32(GetLastError());
            if (SUCCEEDED(hr)) hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
            if (SUCCEEDED(hr))
            {
                hr = enumerator->GetDevice(m_id.c_str(), &device);
                DWORD state = 0;
                if (SUCCEEDED(hr) && (FAILED(device->GetState(&state)) ||
                    (state & DEVICE_STATE_ACTIVE) == 0))
                    hr = HRESULT_FROM_WIN32(ERROR_NOT_READY);
                if (FAILED(hr) && m_allowProfileFallback)
                {
                    device.Reset();
                    ComPtr<IMMDeviceCollection> collection;
                    if (SUCCEEDED(enumerator->EnumAudioEndpoints(
                        eRender, DEVICE_STATE_ACTIVE, &collection)))
                    {
                        UINT count = 0;
                        collection->GetCount(&count);
                        for (UINT index = 0; index < count; ++index)
                        {
                            ComPtr<IMMDevice> candidate;
                            if (SUCCEEDED(collection->Item(index, &candidate)) &&
                                DeviceProfileKey(candidate.Get()) == m_profileKey)
                            {
                                device = candidate;
                                hr = S_OK;
                                break;
                            }
                        }
                    }
                }
            }
            if (SUCCEEDED(hr)) hr = ActivateRawSharedClient(device.Get(), format,
                m_bufferMs, client, eventDriven, periodFrames);
            if (SUCCEEDED(hr) && eventDriven) hr = client->SetEventHandle(audioEvent);
            if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufferFrames);
            if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&render));
            if (SUCCEEDED(hr)) client->GetService(IID_PPV_ARGS(&sessionVolume));
            if (SUCCEEDED(hr)) device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &endpointVolume);
            if (SUCCEEDED(hr)) device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, &endpointMeter);
            UINT32 effectivePeriodFrames = periodFrames;
            if (effectivePeriodFrames == 0) effectivePeriodFrames = std::max<UINT32>(1, bufferFrames / 2);
            const UINT32 requestedFrames = std::max<UINT32>(1,
                static_cast<UINT32>((static_cast<ULONGLONG>(format->nSamplesPerSec) * m_bufferMs + 999) / 1000));
            const UINT32 desiredFrames = std::max(requestedFrames, effectivePeriodFrames * 2);
            const size_t desiredBytes = static_cast<size_t>(desiredFrames) * format->nBlockAlign;
            const size_t primeBytes = static_cast<size_t>(desiredFrames + bufferFrames) * format->nBlockAlign;
            m_ring.SetPrimeTarget(primeBytes);
            m_sourceCache.reserve(static_cast<size_t>(desiredFrames + bufferFrames * 2) *
                format->nChannels);
            if (SUCCEEDED(hr)) hr = client->Start();

            m_result.store(hr);
            m_active.store(SUCCEEDED(hr));
            SetEvent(m_readyEvent);

            ULONGLONG lastRateControl = GetTickCount64();
            double sourceFramesPerOutput = 1.0;
            while (SUCCEEDED(hr))
            {
                DWORD waitResult = WAIT_TIMEOUT;
                if (eventDriven)
                {
                    HANDLE events[] = { m_stopEvent, audioEvent };
                    waitResult = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE);
                    if (waitResult == WAIT_OBJECT_0) break;
                    if (waitResult != WAIT_OBJECT_0 + 1) { hr = E_FAIL; break; }
                }
                else
                {
                    waitResult = WaitForSingleObject(m_stopEvent, std::max<DWORD>(1, m_bufferMs / 2));
                    if (waitResult == WAIT_OBJECT_0) break;
                }

                UINT32 padding = 0;
                hr = client->GetCurrentPadding(&padding);
                if (FAILED(hr)) break;
                const UINT32 availableFrames = bufferFrames > padding ? bufferFrames - padding : 0;
                if (availableFrames == 0) continue;

                BYTE* destination = nullptr;
                hr = render->GetBuffer(availableFrames, &destination);
                if (FAILED(hr)) break;
                const size_t requestedBytes = static_cast<size_t>(availableFrames) * format->nBlockAlign;
                ZeroMemory(destination, requestedBytes);
                const UINT32 renderedFrames = RenderResampled(
                    reinterpret_cast<float*>(destination), availableFrames, sourceFramesPerOutput);
                // Cubic interpolation can overshoot even a bounded input. Bridge-only
                // final guard protects the exact float samples sent to WASAPI.
                if (m_bridgePeakGuard)
                {
                    HXAudioDsp::BridgePeakStats stats;
                    auto* values = reinterpret_cast<float*>(destination);
                    for (size_t i = 0; i < static_cast<size_t>(renderedFrames) * format->nChannels; ++i)
                        values[i] = HXAudioDsp::BridgePeakGuard(values[i], stats);
                    m_resamplerProtected.fetch_add(stats.protectedSamples);
                }
                const size_t copied = static_cast<size_t>(renderedFrames) * format->nBlockAlign;
                // Counts software render starvation after the ring has primed. This
                // does not claim to measure every hardware/driver WASAPI glitch.
                if (m_ring.IsPrimed() && renderedFrames < availableFrames)
                {
                    m_underrunCallbacks.fetch_add(1);
                    m_missingFrames.fetch_add(availableFrames - renderedFrames);
                }
                m_lastPaddingFrames.store(padding);
                m_lastCopiedBytes.store(static_cast<DWORD>(std::min<size_t>(copied, MAXDWORD)));
                m_lastPeakQ31.store(FloatBytesPeakQ31(destination, copied));
                hr = render->ReleaseBuffer(availableFrames, copied == 0 ? AUDCLNT_BUFFERFLAGS_SILENT : 0);

                if (SUCCEEDED(hr))
                {
                    float scalar = 0.0f;
                    BOOL muted = FALSE;
                    if (endpointVolume)
                    {
                        if (SUCCEEDED(endpointVolume->GetMasterVolumeLevelScalar(&scalar)))
                            m_endpointVolumeQ31.store(static_cast<DWORD>(std::clamp<double>(scalar, 0.0, 1.0) * 2147483647.0));
                        if (SUCCEEDED(endpointVolume->GetMute(&muted))) m_endpointMuted.store(muted ? 1 : 0);
                    }
                    scalar = 0.0f;
                    muted = FALSE;
                    if (sessionVolume)
                    {
                        if (SUCCEEDED(sessionVolume->GetMasterVolume(&scalar)))
                            m_sessionVolumeQ31.store(static_cast<DWORD>(std::clamp<double>(scalar, 0.0, 1.0) * 2147483647.0));
                        if (SUCCEEDED(sessionVolume->GetMute(&muted))) m_sessionMuted.store(muted ? 1 : 0);
                    }
                    scalar = 0.0f;
                    if (endpointMeter && SUCCEEDED(endpointMeter->GetPeakValue(&scalar)))
                        m_endpointMeterQ31.store(static_cast<DWORD>(std::clamp<double>(scalar, 0.0, 1.0) * 2147483647.0));
                }

                const ULONGLONG now = GetTickCount64();
                if (SUCCEEDED(hr) && now - lastRateControl >= 250)
                {
                    const size_t bufferedBytes = m_ring.AvailableBytes() +
                        CachedFrames() * format->nBlockAlign;
                    const double error = desiredBytes == 0 ? 0.0 :
                        (static_cast<double>(bufferedBytes) - static_cast<double>(desiredBytes)) /
                        static_cast<double>(desiredBytes);
                    const double requestedRatio = 1.0 + std::clamp(error * 0.001, -0.003, 0.003);
                    sourceFramesPerOutput = sourceFramesPerOutput * 0.85 + requestedRatio * 0.15;
                    lastRateControl = now;
                }
            }

            m_result.store(hr);
            m_active.store(false);
            if (client) client->Stop();
            if (audioEvent) CloseHandle(audioEvent);
            if (m_failureEvent && WaitForSingleObject(m_stopEvent, 0) != WAIT_OBJECT_0)
            {
                SetEvent(m_failureEvent);
            }
            if (SUCCEEDED(co)) CoUninitialize();
            return static_cast<DWORD>(hr);
        }

        std::wstring m_id;
        std::wstring m_name;
        std::wstring m_profileKey;
        DWORD m_bufferMs = 5;
        HANDLE m_stopEvent = nullptr;
        HANDLE m_failureEvent = nullptr;
        size_t m_cpuSlot = 0;
        bool m_applyPeq = true;
        bool m_allowProfileFallback = true;
        bool m_bridgePeakGuard = false;
        std::atomic<unsigned long long> m_resamplerProtected{ 0 };
        std::vector<BYTE> m_format;
        ByteRing m_ring;
        DspProcessor m_peq;
        std::vector<PeqBand> m_peqBands;
        bool m_peqBypass = true;
        std::vector<float> m_peqBuffer;
        std::vector<float> m_sourceCache;
        size_t m_sourceStartFrame = 0;
        double m_sourcePhase = 0.0;
        HANDLE m_readyEvent = nullptr;
        HANDLE m_thread = nullptr;
        std::atomic<bool> m_active{ false };
        std::atomic<HRESULT> m_result{ E_PENDING };
        std::atomic<DWORD> m_lastPeakQ31{ 0 };
        std::atomic<DWORD> m_lastCopiedBytes{ 0 };
        std::atomic<unsigned long long> m_underrunCallbacks{ 0 }, m_missingFrames{ 0 };
        std::atomic<DWORD> m_lastPaddingFrames{ 0 };
        std::atomic<DWORD> m_endpointVolumeQ31{ 0 };
        std::atomic<DWORD> m_endpointMuted{ 0 };
        std::atomic<DWORD> m_sessionVolumeQ31{ 0 };
        std::atomic<DWORD> m_sessionMuted{ 0 };
        std::atomic<DWORD> m_endpointMeterQ31{ 0 };
    };

    // IPolicyConfig is the shell interface used by Windows' own sound UI.
    // Routing continues to work if it is unavailable; only automatic default
    // endpoint selection is skipped.
    struct __declspec(uuid("F8679F50-850A-41CF-9C72-430F290290C8")) IPolicyConfig : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
        virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, INT64*, INT64*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, INT64*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, const PROPVARIANT*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
    };

    HRESULT CreatePolicyConfig(ComPtr<IPolicyConfig>& policy)
    {
        static const CLSID clsid = { 0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };
        return CoCreateInstance(clsid, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), &policy);
    }

    HRESULT SetDefaultEndpoint(const std::wstring& id)
    {
        ComPtr<IPolicyConfig> policy;
        HRESULT hr = CreatePolicyConfig(policy);
        if (FAILED(hr)) return hr;
        for (ERole role : { eConsole, eMultimedia, eCommunications })
        {
            hr = policy->SetDefaultEndpoint(id.c_str(), role);
            if (FAILED(hr)) return hr;
        }
        return S_OK;
    }

    struct SavedApplicationRoutingPolicy
    {
        std::array<std::wstring, 3> packedEndpoints;
        bool valid = false;
    };

    class ApplicationRoutingPolicy
    {
    public:
        ~ApplicationRoutingPolicy()
        {
            if (m_factory) m_factory->Release();
            if (m_roInitialized) RoUninitialize();
        }

        HRESULT Initialize()
        {
            if (m_factory && m_getEndpoint && m_setEndpoint) return S_OK;
            const HRESULT roResult = RoInitialize(RO_INIT_MULTITHREADED);
            if (SUCCEEDED(roResult)) m_roInitialized = true;
            else if (roResult != RPC_E_CHANGED_MODE) return roResult;
            static constexpr wchar_t className[] =
                L"Windows.Media.Internal.AudioPolicyConfig";
            HSTRING runtimeClass = nullptr;
            HRESULT hr = WindowsCreateString(className,
                static_cast<UINT32>(wcslen(className)), &runtimeClass);
            if (FAILED(hr)) return hr;

            // Windows 11/late Windows 10 and down-level Windows 10 expose the
            // same factory layout under different interface IDs.
            static const IID modernIid = { 0xab3d4648, 0xe242, 0x459f,
                { 0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24 } };
            static const IID downlevelIid = { 0x2a59116d, 0x6c4f, 0x45e0,
                { 0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58 } };
            hr = RoGetActivationFactory(runtimeClass, modernIid,
                reinterpret_cast<void**>(&m_factory));
            if (FAILED(hr))
                hr = RoGetActivationFactory(runtimeClass, downlevelIid,
                    reinterpret_cast<void**>(&m_factory));
            WindowsDeleteString(runtimeClass);
            if (FAILED(hr) || !m_factory) return FAILED(hr) ? hr : E_NOINTERFACE;

            void** table = *reinterpret_cast<void***>(m_factory);
            m_getEndpoint = reinterpret_cast<GetEndpointFunction>(table[24]);
            m_setEndpoint = reinterpret_cast<SetEndpointFunction>(table[25]);
            return m_getEndpoint && m_setEndpoint ? S_OK : E_NOINTERFACE;
        }

        HRESULT CaptureProcessEndpoints(DWORD processId,
            SavedApplicationRoutingPolicy& saved)
        {
            RETURN_IF_FAILED(Initialize());
            SavedApplicationRoutingPolicy captured;
            for (size_t index = 0; index < kRoles.size(); ++index)
            {
                HSTRING endpoint = nullptr;
                const HRESULT result = m_getEndpoint(m_factory, processId,
                    eRender, kRoles[index], &endpoint);
                if (FAILED(result))
                {
                    if (endpoint) WindowsDeleteString(endpoint);
                    return result;
                }
                if (endpoint)
                {
                    UINT32 length = 0;
                    const wchar_t* value = WindowsGetStringRawBuffer(endpoint, &length);
                    if (value && length != 0)
                        captured.packedEndpoints[index].assign(value, length);
                    WindowsDeleteString(endpoint);
                }
            }
            captured.valid = true;
            saved = std::move(captured);
            return S_OK;
        }

        HRESULT SetProcessEndpoint(DWORD processId, const std::wstring& endpointId)
        {
            RETURN_IF_FAILED(Initialize());
            HSTRING device = nullptr;
            if (!endpointId.empty())
            {
                const std::wstring packed = L"\\\\?\\SWD#MMDEVAPI#" + endpointId +
                    L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
                RETURN_IF_FAILED(WindowsCreateString(packed.c_str(),
                    static_cast<UINT32>(packed.size()), &device));
            }
            HRESULT firstFailure = S_OK;
            for (ERole role : kRoles)
            {
                const HRESULT result = m_setEndpoint(m_factory, processId,
                    eRender, role, device);
                if (FAILED(result) && SUCCEEDED(firstFailure)) firstFailure = result;
            }
            if (device) WindowsDeleteString(device);
            return firstFailure;
        }

        HRESULT RestoreProcessEndpoints(DWORD processId,
            const SavedApplicationRoutingPolicy& saved)
        {
            if (!saved.valid) return E_INVALIDARG;
            RETURN_IF_FAILED(Initialize());
            HRESULT firstFailure = S_OK;
            for (size_t index = 0; index < kRoles.size(); ++index)
            {
                HSTRING endpoint = nullptr;
                const std::wstring& packed = saved.packedEndpoints[index];
                if (!packed.empty())
                {
                    const HRESULT stringResult = WindowsCreateString(packed.c_str(),
                        static_cast<UINT32>(packed.size()), &endpoint);
                    if (FAILED(stringResult))
                    {
                        if (SUCCEEDED(firstFailure)) firstFailure = stringResult;
                        continue;
                    }
                }
                const HRESULT result = m_setEndpoint(m_factory, processId,
                    eRender, kRoles[index], endpoint);
                if (endpoint) WindowsDeleteString(endpoint);
                if (FAILED(result) && SUCCEEDED(firstFailure)) firstFailure = result;
            }
            return firstFailure;
        }

    private:
        using GetEndpointFunction = HRESULT(STDMETHODCALLTYPE*)(
            IInspectable*, UINT32, EDataFlow, ERole, HSTRING*);
        using SetEndpointFunction = HRESULT(STDMETHODCALLTYPE*)(
            IInspectable*, UINT32, EDataFlow, ERole, HSTRING);
        static constexpr std::array<ERole, 3> kRoles{
            eConsole, eMultimedia, eCommunications };
        IInspectable* m_factory = nullptr;
        GetEndpointFunction m_getEndpoint = nullptr;
        SetEndpointFunction m_setEndpoint = nullptr;
        bool m_roInitialized = false;
    };

    bool IsHxAudioProcess(DWORD processId)
    {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE, processId);
        if (!process) return false;
        wchar_t path[32768]{};
        DWORD length = ARRAYSIZE(path);
        const bool queried = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
        CloseHandle(process);
        if (!queried || length == 0) return false;
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        return _wcsicmp(name, L"HXAudioControl.exe") == 0;
    }

    struct EscapedApplicationSession
    {
        DWORD processId = 0;
        std::wstring identifier;
        std::wstring instanceIdentifier;
        ComPtr<IAudioSessionControl> control;
        ComPtr<ISimpleAudioVolume> volume;
        float originalVolume = 1.0f;
        BOOL originallyMuted = FALSE;
    };

    std::vector<EscapedApplicationSession> EscapedApplicationSessions(
        IMMDeviceEnumerator* enumerator,
        const std::wstring& destinationId,
        bool includeAllPhysicalEndpoints = false)
    {
        std::vector<EscapedApplicationSession> escaped;
        if (!enumerator || destinationId.empty()) return escaped;
        const std::vector<std::wstring> targetIds = ReadRegistryMultiString(L"TargetEndpoints");
        const std::vector<std::wstring> targetProfiles =
            ReadRegistryMultiString(L"TargetEndpointProfiles");
        if (!includeAllPhysicalEndpoints && targetIds.empty()) return escaped;

        ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator->EnumAudioEndpoints(
            eRender, DEVICE_STATE_ACTIVE, &collection))) return escaped;
        UINT count = 0;
        if (FAILED(collection->GetCount(&count))) return escaped;
        const DWORD ownProcess = GetCurrentProcessId();
        for (UINT index = 0; index < count; ++index)
        {
            ComPtr<IMMDevice> endpoint;
            if (FAILED(collection->Item(index, &endpoint))) continue;
            const std::wstring id = DeviceId(endpoint.Get());
            if (id.empty() || id == destinationId ||
                IsVirtualName(DeviceName(endpoint.Get())))
                continue;
            const bool selectedById = includeAllPhysicalEndpoints ||
                std::find(targetIds.begin(), targetIds.end(), id) != targetIds.end();
            const std::wstring profile = DeviceProfileKey(endpoint.Get());
            const bool selectedByProfile = includeAllPhysicalEndpoints ||
                (!profile.empty() &&
                std::find(targetProfiles.begin(), targetProfiles.end(), profile) !=
                    targetProfiles.end());
            if (!selectedById && !selectedByProfile) continue;

            ComPtr<IAudioSessionManager2> manager;
            if (FAILED(endpoint->Activate(__uuidof(IAudioSessionManager2),
                CLSCTX_ALL, nullptr, &manager))) continue;
            ComPtr<IAudioSessionEnumerator> sessions;
            if (FAILED(manager->GetSessionEnumerator(&sessions))) continue;
            int sessionCount = 0;
            if (FAILED(sessions->GetCount(&sessionCount))) continue;
            for (int sessionIndex = 0; sessionIndex < sessionCount; ++sessionIndex)
            {
                ComPtr<IAudioSessionControl> control;
                if (FAILED(sessions->GetSession(sessionIndex, &control))) continue;
                AudioSessionState state = AudioSessionStateInactive;
                if (FAILED(control->GetState(&state)) || state != AudioSessionStateActive)
                    continue;
                ComPtr<IAudioSessionControl2> control2;
                DWORD processId = 0;
                if (FAILED(control.As(&control2)) ||
                    FAILED(control2->GetProcessId(&processId)) ||
                    processId == ownProcess ||
                    (processId != 0 && IsHxAudioProcess(processId)))
                    continue;
                ComPtr<ISimpleAudioVolume> volume;
                if (FAILED(control.As(&volume))) continue;
                EscapedApplicationSession session;
                session.processId = processId;
                session.control = control;
                session.volume = volume;
                volume->GetMasterVolume(&session.originalVolume);
                volume->GetMute(&session.originallyMuted);
                LPWSTR identifier = nullptr;
                if (SUCCEEDED(control2->GetSessionIdentifier(&identifier)) && identifier)
                {
                    session.identifier = identifier;
                    CoTaskMemFree(identifier);
                }
                LPWSTR instanceIdentifier = nullptr;
                if (SUCCEEDED(control2->GetSessionInstanceIdentifier(
                    &instanceIdentifier)) && instanceIdentifier)
                {
                    session.instanceIdentifier = instanceIdentifier;
                    CoTaskMemFree(instanceIdentifier);
                }
                escaped.push_back(std::move(session));
            }
        }
        return escaped;
    }

    std::vector<EscapedApplicationSession> ActiveApplicationSessions(
        IMMDeviceEnumerator* enumerator, const std::wstring& endpointId,
        const std::set<DWORD>& processIds)
    {
        std::vector<EscapedApplicationSession> sessions;
        if (!enumerator || endpointId.empty()) return sessions;
        ComPtr<IMMDevice> endpoint;
        if (FAILED(enumerator->GetDevice(endpointId.c_str(), &endpoint))) return sessions;
        ComPtr<IAudioSessionManager2> manager;
        if (FAILED(endpoint->Activate(__uuidof(IAudioSessionManager2),
            CLSCTX_ALL, nullptr, &manager))) return sessions;
        ComPtr<IAudioSessionEnumerator> enumerated;
        if (FAILED(manager->GetSessionEnumerator(&enumerated))) return sessions;
        int count = 0;
        if (FAILED(enumerated->GetCount(&count))) return sessions;
        for (int index = 0; index < count; ++index)
        {
            ComPtr<IAudioSessionControl> control;
            if (FAILED(enumerated->GetSession(index, &control))) continue;
            AudioSessionState state = AudioSessionStateInactive;
            if (FAILED(control->GetState(&state)) || state != AudioSessionStateActive)
                continue;
            ComPtr<IAudioSessionControl2> control2;
            DWORD processId = 0;
            if (FAILED(control.As(&control2)) ||
                FAILED(control2->GetProcessId(&processId)) ||
                (!processIds.empty() && processIds.find(processId) == processIds.end())) continue;
            ComPtr<ISimpleAudioVolume> volume;
            if (FAILED(control.As(&volume))) continue;
            EscapedApplicationSession session;
            session.processId = processId;
            session.control = control;
            session.volume = volume;
            LPWSTR identifier = nullptr;
            if (SUCCEEDED(control2->GetSessionIdentifier(&identifier)) && identifier)
            {
                session.identifier = identifier;
                CoTaskMemFree(identifier);
            }
            LPWSTR instanceIdentifier = nullptr;
            if (SUCCEEDED(control2->GetSessionInstanceIdentifier(
                &instanceIdentifier)) && instanceIdentifier)
            {
                session.instanceIdentifier = instanceIdentifier;
                CoTaskMemFree(instanceIdentifier);
            }
            sessions.push_back(std::move(session));
        }
        return sessions;
    }

    std::wstring SessionInstanceKey(const EscapedApplicationSession& session)
    {
        if (!session.instanceIdentifier.empty()) return session.instanceIdentifier;
        if (!session.identifier.empty()) return session.identifier;
        return L"PID:" + std::to_wstring(session.processId);
    }

    ULONGLONG ProcessCreationToken(DWORD processId, bool& running)
    {
        if (processId == 0)
        {
            running = true;
            return 1;
        }
        running = false;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
            FALSE, processId);
        if (!process) return 0;
        running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        FILETIME created{}, exited{}, kernel{}, user{};
        ULONGLONG token = 0;
        if (GetProcessTimes(process, &created, &exited, &kernel, &user))
            token = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) |
                created.dwLowDateTime;
        CloseHandle(process);
        return token;
    }

    struct OwnedApplicationRoute
    {
        DWORD processId = 0;
        ULONGLONG processCreationToken = 0;
        std::wstring destinationEndpointId;
        SavedApplicationRoutingPolicy originalPolicy;
        std::vector<EscapedApplicationSession> mutedDirectSessions;
        std::vector<std::wstring> initializedVirtualSessions;
    };

    class ApplicationRouteOwnership
    {
    public:
        DWORD Reconcile(IMMDeviceEnumerator* enumerator,
            const std::wstring& destinationId,
            bool includeAllPhysicalEndpoints = false)
        {
            if (!enumerator || destinationId.empty()) return 0;
            PruneExpiredDirectSessions();
            for (auto& owner : m_owned)
            {
                bool running = false;
                const ULONGLONG token = ProcessCreationToken(
                    owner.processId, running);
                if (!running || token != owner.processCreationToken ||
                    owner.destinationEndpointId == destinationId) continue;
                const HRESULT result = m_policy.SetProcessEndpoint(
                    owner.processId, destinationId);
                if (FAILED(result)) RecordFailure(result);
                else owner.destinationEndpointId = destinationId;
            }
            std::vector<EscapedApplicationSession> escaped =
                EscapedApplicationSessions(enumerator, destinationId,
                    includeAllPhysicalEndpoints);
            DWORD acquired = 0;

            for (auto& session : escaped)
            {
                bool running = false;
                const ULONGLONG creationToken =
                    ProcessCreationToken(session.processId, running);
                if (!running) continue;
                OwnedApplicationRoute* owner = FindOwner(
                    session.processId, creationToken);
                const std::wstring sessionKey = SessionInstanceKey(session);
                if (owner && OwnsDirectSession(*owner, sessionKey)) continue;

                if (!owner)
                {
                    OwnedApplicationRoute candidate;
                    candidate.processId = session.processId;
                    candidate.processCreationToken = creationToken;
                    candidate.destinationEndpointId = destinationId;
                    HRESULT result = m_policy.CaptureProcessEndpoints(
                        session.processId, candidate.originalPolicy);
                    if (FAILED(result))
                    {
                        RecordFailure(result);
                        continue;
                    }
                    // Take ownership before changing the process policy. This is
                    // a fail-closed hand-off: the old physical stream can never
                    // overlap the processed stream and create a loud burst.
                    session.volume->SetMute(TRUE, nullptr);
                    candidate.mutedDirectSessions.push_back(std::move(session));
                    result = m_policy.SetProcessEndpoint(candidate.processId,
                        destinationId);
                    if (FAILED(result))
                    {
                        RestoreDirectSessions(candidate);
                        RecordFailure(result);
                        continue;
                    }
                    m_owned.push_back(std::move(candidate));
                    ++acquired;
                    continue;
                }

                // A process that deliberately creates several session
                // instances is still one routing owner. Mutate each new
                // instance once, never once per guardian tick.
                session.volume->SetMute(TRUE, nullptr);
                owner->mutedDirectSessions.push_back(std::move(session));
                const HRESULT result = m_policy.SetProcessEndpoint(
                    owner->processId, destinationId);
                if (FAILED(result)) RecordFailure(result);
                else
                {
                    owner->destinationEndpointId = destinationId;
                    ++acquired;
                }
            }

            std::set<DWORD> ownedProcesses;
            for (const auto& owner : m_owned) ownedProcesses.insert(owner.processId);
            const std::vector<EscapedApplicationSession> virtualSessions =
                ActiveApplicationSessions(enumerator, destinationId,
                    ownedProcesses);
            for (const auto& destination : virtualSessions)
            {
                bool running = false;
                const ULONGLONG creationToken =
                    ProcessCreationToken(destination.processId, running);
                OwnedApplicationRoute* owner = running
                    ? FindOwner(destination.processId, creationToken) : nullptr;
                if (!owner) continue;
                const std::wstring destinationKey = SessionInstanceKey(destination);
                if (std::find(owner->initializedVirtualSessions.begin(),
                    owner->initializedVirtualSessions.end(), destinationKey) !=
                    owner->initializedVirtualSessions.end()) continue;
                auto original = std::find_if(owner->mutedDirectSessions.begin(),
                    owner->mutedDirectSessions.end(),
                    [&](const EscapedApplicationSession& candidate)
                    {
                        return destination.identifier.empty() || candidate.identifier.empty() ||
                            candidate.identifier == destination.identifier;
                    });
                if (original == owner->mutedDirectSessions.end() &&
                    !owner->mutedDirectSessions.empty())
                    original = owner->mutedDirectSessions.begin();
                if (original != owner->mutedDirectSessions.end())
                {
                    destination.volume->SetMasterVolume(
                        original->originalVolume, nullptr);
                    destination.volume->SetMute(original->originallyMuted, nullptr);
                }
                owner->initializedVirtualSessions.push_back(destinationKey);
            }
            WriteRegistryDword(L"RouterOwnedApplicationCount",
                static_cast<DWORD>(std::min<size_t>(m_owned.size(), MAXDWORD)));
            return acquired;
        }

        void ReleaseAll()
        {
            for (auto& owner : m_owned)
            {
                const HRESULT result = m_policy.RestoreProcessEndpoints(
                    owner.processId, owner.originalPolicy);
                if (FAILED(result)) RecordFailure(result);
                RestoreDirectSessions(owner);
            }
            m_owned.clear();
            WriteRegistryDword(L"RouterOwnedApplicationCount", 0);
        }

        size_t Count() const { return m_owned.size(); }

    private:
        OwnedApplicationRoute* FindOwner(DWORD processId, ULONGLONG creationToken)
        {
            auto found = std::find_if(m_owned.begin(), m_owned.end(),
                [&](const OwnedApplicationRoute& candidate)
                {
                    return candidate.processId == processId &&
                        candidate.processCreationToken == creationToken;
                });
            return found != m_owned.end() ? &*found : nullptr;
        }

        static bool OwnsDirectSession(const OwnedApplicationRoute& owner,
            const std::wstring& key)
        {
            return std::any_of(owner.mutedDirectSessions.begin(),
                owner.mutedDirectSessions.end(),
                [&](const EscapedApplicationSession& candidate)
                { return SessionInstanceKey(candidate) == key; });
        }

        static void RestoreDirectSessions(OwnedApplicationRoute& owner)
        {
            for (auto& session : owner.mutedDirectSessions)
            {
                session.volume->SetMasterVolume(session.originalVolume, nullptr);
                session.volume->SetMute(session.originallyMuted, nullptr);
            }
            owner.mutedDirectSessions.clear();
        }

        void PruneExpiredDirectSessions()
        {
            for (auto& owner : m_owned)
            {
                owner.mutedDirectSessions.erase(std::remove_if(
                    owner.mutedDirectSessions.begin(), owner.mutedDirectSessions.end(),
                    [](EscapedApplicationSession& session)
                    {
                        AudioSessionState state = AudioSessionStateExpired;
                        if (session.control && SUCCEEDED(session.control->GetState(&state)) &&
                            state == AudioSessionStateActive) return false;
                        // Inactive/expired sessions cannot leak audio; restore
                        // their original mixer state before releasing the COM
                        // object so Windows never inherits a stale mute.
                        session.volume->SetMasterVolume(session.originalVolume, nullptr);
                        session.volume->SetMute(session.originallyMuted, nullptr);
                        return true;
                    }), owner.mutedDirectSessions.end());
            }
        }

        static void RecordFailure(HRESULT result)
        {
            WriteRegistryDword(L"RouterSessionRepairLastError",
                static_cast<DWORD>(result));
        }

        ApplicationRoutingPolicy m_policy;
        std::vector<OwnedApplicationRoute> m_owned;
    };

    HRESULT ResetEndpointFormat(const std::wstring& id)
    {
        ComPtr<IPolicyConfig> policy;
        RETURN_IF_FAILED(CreatePolicyConfig(policy));
        return policy->ResetDeviceFormat(id.c_str());
    }

    HRESULT SetEndpointVisible(const std::wstring& id, bool visible)
    {
        ComPtr<IPolicyConfig> policy;
        RETURN_IF_FAILED(CreatePolicyConfig(policy));
        return policy->SetEndpointVisibility(id.c_str(), visible ? TRUE : FALSE);
    }

    HRESULT EnsureVirtualEndpointActive(IMMDeviceEnumerator* enumerator,
        std::wstring& virtualId, DWORD timeoutMs)
    {
        if (!enumerator) return E_POINTER;
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        HRESULT lastResult = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        do
        {
            ComPtr<IMMDevice> virtualDevice;
            lastResult = FindVirtualDevice(enumerator, virtualDevice);
            if (SUCCEEDED(lastResult))
            {
                virtualId = DeviceId(virtualDevice.Get());
                if (!virtualId.empty())
                {
                    lastResult = SetEndpointVisible(virtualId, true);
                    DWORD state = 0;
                    if (SUCCEEDED(lastResult) &&
                        SUCCEEDED(virtualDevice->GetState(&state)) &&
                        (state & DEVICE_STATE_ACTIVE) != 0)
                    {
                        return S_OK;
                    }
                }
            }
            if (GetTickCount64() >= deadline) break;
            Sleep(50);
        } while (true);
        return FAILED(lastResult) ? lastResult : HRESULT_FROM_WIN32(ERROR_NOT_READY);
    }

    bool IsActiveEndpoint(IMMDeviceEnumerator* enumerator, const std::wstring& id)
    {
        if (id.empty()) return false;
        ComPtr<IMMDevice> device;
        DWORD state = 0;
        return SUCCEEDED(enumerator->GetDevice(id.c_str(), &device)) &&
            SUCCEEDED(device->GetState(&state)) && (state & DEVICE_STATE_ACTIVE) != 0;
    }

    bool AllDefaultRolesMatch(IMMDeviceEnumerator* enumerator, const std::wstring& id)
    {
        for (ERole role : { eConsole, eMultimedia, eCommunications })
        {
            ComPtr<IMMDevice> current;
            if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, role, &current)) || DeviceId(current.Get()) != id)
            {
                return false;
            }
        }
        return true;
    }

    std::wstring ResolveRestoreEndpoint(IMMDeviceEnumerator* enumerator)
    {
        const std::wstring original = ReadRegistryString(L"OriginalDefaultEndpoint");
        if (IsActiveEndpoint(enumerator, original)) return original;

        // If routing is already stopped or failed before taking ownership of
        // the default endpoint, keep Windows' current working non-HXAudio
        // output instead of replacing it with stale HXAudio target history.
        for (ERole role : { eMultimedia, eConsole, eCommunications })
        {
            ComPtr<IMMDevice> current;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, role, &current)) &&
                !IsVirtualName(DeviceName(current.Get())))
            {
                const std::wstring id = DeviceId(current.Get());
                if (IsActiveEndpoint(enumerator, id)) return id;
            }
        }

        for (const auto& id : ReadRegistryMultiString(L"TargetEndpoints"))
        {
            if (IsActiveEndpoint(enumerator, id)) return id;
        }
        const std::wstring legacy = ReadRegistryString(L"TargetEndpoint");
        if (IsActiveEndpoint(enumerator, legacy)) return legacy;

        // Last-resort protection against silence: if the HXAudio endpoint is
        // still the default but its saved physical target was lost, select an
        // active non-HXAudio render endpoint before removing the driver.
        ComPtr<IMMDeviceCollection> collection;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender,
            DEVICE_STATE_ACTIVE, &collection)))
        {
            UINT count = 0;
            collection->GetCount(&count);
            for (UINT index = 0; index < count; ++index)
            {
                ComPtr<IMMDevice> candidate;
                if (SUCCEEDED(collection->Item(index, &candidate)) &&
                    !IsVirtualName(DeviceName(candidate.Get())))
                {
                    const std::wstring id = DeviceId(candidate.Get());
                    if (!id.empty()) return id;
                }
            }
        }
        return {};
    }

    bool StartBackgroundProcess()
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        std::wstring command = L"\"" + std::wstring(path) + L"\" --background";
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION process{};
        std::vector<wchar_t> writable(command.begin(), command.end());
        writable.push_back(L'\0');
        const BOOL ok = CreateProcessW(path, writable.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        if (ok)
        {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        return ok != FALSE;
    }

    void StopBackground()
    {
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEvent);
        if (event)
        {
            SetEvent(event);
            CloseHandle(event);
        }
    }

    bool SignalRoutingSettingsChanged()
    {
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kRoutingSettingsEvent);
        if (!event) return false;
        SetEvent(event);
        CloseHandle(event);
        return true;
    }

    void ResetRouterReadySignal()
    {
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kRouterReadyEvent);
        if (event)
        {
            ResetEvent(event);
            CloseHandle(event);
        }
    }

    bool RouterProcessIsRunning()
    {
        HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kRouterMutex);
        if (!mutex) return false;
        CloseHandle(mutex);
        return true;
    }

    bool RouterIsReady()
    {
        HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, kRouterReadyEvent);
        if (!event) return false;
        const bool ready = WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
        CloseHandle(event);
        return ready;
    }

    bool WaitForRouterReady(DWORD timeoutMs)
    {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        do
        {
            HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, kRouterReadyEvent);
            if (event)
            {
                const DWORD remaining = static_cast<DWORD>(std::min<ULONGLONG>(
                    deadline > GetTickCount64() ? deadline - GetTickCount64() : 0, 100));
                const bool ready = WaitForSingleObject(event, remaining) == WAIT_OBJECT_0;
                CloseHandle(event);
                if (ready) return true;
            }
            else
            {
                Sleep(50);
            }
        } while (GetTickCount64() < deadline);
        return RouterIsReady();
    }

    void WaitForRouterStopped(DWORD timeoutMs)
    {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (RouterProcessIsRunning() && GetTickCount64() < deadline)
        {
            Sleep(50);
        }
    }

    class RouterDeviceNotifications final : public IMMNotificationClient
    {
    public:
        RouterDeviceNotifications(HANDLE changedEvent, HANDLE defaultChangedEvent,
            std::wstring sourceId, bool rebuildOnAnyDevice = false)
            : m_changedEvent(changedEvent),
              m_defaultChangedEvent(defaultChangedEvent),
              m_sourceId(std::move(sourceId)),
              m_rebuildOnAnyDevice(rebuildOnAnyDevice)
        {
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
        {
            if (!object) return E_POINTER;
            if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient))
            {
                *object = static_cast<IMMNotificationClient*>(this);
                AddRef();
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return ++m_references;
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            return --m_references;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR deviceId, DWORD) override
        {
            if (m_rebuildOnAnyDevice && m_changedEvent) SetEvent(m_changedEvent);
            else SignalSourceIfNeeded(deviceId);
            SignalDefaultGuard();
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override
        {
            // Target workers recover independently. Rebuilding the virtual
            // capture graph for every Bluetooth/HDMI endpoint arrival creates
            // an audible race while applications open their next stream.
            if (m_rebuildOnAnyDevice && m_changedEvent) SetEvent(m_changedEvent);
            SignalDefaultGuard();
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR deviceId) override
        {
            if (m_rebuildOnAnyDevice && m_changedEvent) SetEvent(m_changedEvent);
            else SignalSourceIfNeeded(deviceId);
            SignalDefaultGuard();
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole, LPCWSTR) override
        {
            // A dedicated policy event lets APO mode move application sessions
            // without rebuilding the playback graph or fighting the system
            // default-device UI. Virtual mode uses the same event to restore
            // its explicit default-endpoint invariant.
            if (flow == eRender && m_defaultChangedEvent)
                SetEvent(m_defaultChangedEvent);
            else if (flow == eRender && m_rebuildOnAnyDevice && m_changedEvent)
                SetEvent(m_changedEvent);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
        {
            return S_OK;
        }

    private:
        void SignalDefaultGuard()
        {
            if (m_defaultChangedEvent) SetEvent(m_defaultChangedEvent);
        }

        void SignalSourceIfNeeded(LPCWSTR deviceId)
        {
            if (deviceId && !m_sourceId.empty() &&
                _wcsicmp(deviceId, m_sourceId.c_str()) == 0 && m_changedEvent)
                SetEvent(m_changedEvent);
        }

        std::atomic<ULONG> m_references{ 1 };
        HANDLE m_changedEvent = nullptr;
        HANDLE m_defaultChangedEvent = nullptr;
        std::wstring m_sourceId;
        bool m_rebuildOnAnyDevice = false;
    };

    struct DefaultEndpointGuardContext
    {
        HANDLE stopEvent = nullptr;
        HANDLE changedEvent = nullptr;
        HANDLE routeStoppedEvent = nullptr;
    };

    DWORD WINAPI DefaultEndpointGuardThread(void* parameter)
    {
        const auto* context = static_cast<const DefaultEndpointGuardContext*>(parameter);
        if (!context || !context->stopEvent || !context->changedEvent ||
            !context->routeStoppedEvent) return ERROR_INVALID_PARAMETER;
        // Endpoint hand-off races happen while an application is opening its
        // next WASAPI stream. Keep the tiny guardian responsive without
        // putting the actual audio/render threads at risk.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT createResult = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        ULONGLONG repairCount = ReadRegistryDword(L"RouterDefaultRepairCount", 0);
        ULONGLONG sessionRepairCount =
            ReadRegistryDword(L"RouterEscapedSessionRepairCount", 0);
        ApplicationRouteOwnership routeOwnership;
        HANDLE events[] = { context->stopEvent, context->changedEvent };

        while (SUCCEEDED(createResult))
        {
            const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, 250);
            if (wait == WAIT_OBJECT_0) break;
            if (wait != WAIT_OBJECT_0 + 1 && wait != WAIT_TIMEOUT) break;
            if (ReadRegistryDword(L"RoutingEnabled", 0) == 0) continue;

            ComPtr<IMMDevice> virtualDevice;
            if (FAILED(FindVirtualDevice(enumerator.Get(), virtualDevice))) continue;
            const std::wstring virtualId = DeviceId(virtualDevice.Get());
            if (virtualId.empty()) continue;

            bool defaultsOwned = AllDefaultRolesMatch(enumerator.Get(), virtualId);
            if (!defaultsOwned && SUCCEEDED(SetDefaultEndpoint(virtualId)))
            {
                defaultsOwned = true;
                ++repairCount;
                WriteRegistryDword(L"RouterDefaultRepairCount",
                    static_cast<DWORD>(std::min<ULONGLONG>(repairCount, MAXDWORD)));
                WriteRegistryDword(L"RouterLastDefaultRepairTick",
                    static_cast<DWORD>(GetTickCount64() & 0xffffffffu));
            }
            if (defaultsOwned)
            {
                const DWORD repaired = routeOwnership.Reconcile(
                    enumerator.Get(), virtualId);
                if (repaired != 0)
                {
                    sessionRepairCount += repaired;
                    WriteRegistryDword(L"RouterEscapedSessionRepairCount",
                        static_cast<DWORD>(std::min<ULONGLONG>(
                            sessionRepairCount, MAXDWORD)));
                    WriteRegistryDword(L"RouterLastSessionRepairTick",
                        static_cast<DWORD>(GetTickCount64() & 0xffffffffu));
                }
            }
        }

        // The renderer and loopback capture observe the same stop event. Do not
        // unmute direct sessions or restore application policies until the
        // audio graph has fully drained, otherwise shutdown itself can leak a
        // short unprocessed/louder overlap to a physical endpoint.
        WaitForSingleObject(context->routeStoppedEvent, 10000);
        routeOwnership.ReleaseAll();

        if (SUCCEEDED(co)) CoUninitialize();
        return SUCCEEDED(createResult) ? ERROR_SUCCESS : static_cast<DWORD>(createResult);
    }

    int RunRouterGuardSelfTest()
    {
        HANDLE routeChanged = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE defaultChanged = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!routeChanged || !defaultChanged)
        {
            if (defaultChanged) CloseHandle(defaultChanged);
            if (routeChanged) CloseHandle(routeChanged);
            return 31;
        }
        RouterDeviceNotifications notifications(routeChanged, defaultChanged, L"virtual");
        notifications.OnDefaultDeviceChanged(eCapture, eConsole, L"capture");
        const bool captureIgnored = WaitForSingleObject(defaultChanged, 0) == WAIT_TIMEOUT;
        notifications.OnDefaultDeviceChanged(eRender, eMultimedia, L"physical");
        const bool renderSignaled = WaitForSingleObject(defaultChanged, 0) == WAIT_OBJECT_0;
        notifications.OnDeviceStateChanged(L"physical", DEVICE_STATE_ACTIVE);
        const bool physicalDidNotRebuild =
            WaitForSingleObject(routeChanged, 0) == WAIT_TIMEOUT;
        const bool deviceAlsoGuardsDefault =
            WaitForSingleObject(defaultChanged, 0) == WAIT_OBJECT_0;
        notifications.OnDeviceStateChanged(L"virtual", DEVICE_STATE_ACTIVE);
        const bool sourceRebuilds =
            WaitForSingleObject(routeChanged, 0) == WAIT_OBJECT_0;
        const bool sourceAlsoGuardsDefault =
            WaitForSingleObject(defaultChanged, 0) == WAIT_OBJECT_0;
        CloseHandle(defaultChanged);
        CloseHandle(routeChanged);
        if (!(captureIgnored && renderSignaled && physicalDidNotRebuild &&
            deviceAlsoGuardsDefault && sourceRebuilds && sourceAlsoGuardsDefault))
            return 32;

        EscapedApplicationSession first;
        first.processId = 100;
        first.identifier = L"group";
        first.instanceIdentifier = L"instance-a";
        EscapedApplicationSession duplicate = first;
        EscapedApplicationSession next = first;
        next.instanceIdentifier = L"instance-b";
        if (SessionInstanceKey(first) != SessionInstanceKey(duplicate) ||
            SessionInstanceKey(first) == SessionInstanceKey(next))
            return 33;

        // Capture is read-only. It verifies both Win10/Win11 policy-factory
        // vtable layouts before the router is allowed to own app policies.
        ApplicationRoutingPolicy policy;
        SavedApplicationRoutingPolicy saved;
        if (FAILED(policy.CaptureProcessEndpoints(GetCurrentProcessId(), saved)) ||
            !saved.valid)
            return 34;
        return 0;
    }

    std::vector<std::wstring> LoadTargetIds()
    {
        std::vector<std::wstring> targets = ReadRegistryMultiString(L"TargetEndpoints");
        if (targets.empty())
        {
            const std::wstring legacy = ReadRegistryString(L"TargetEndpoint");
            if (!legacy.empty()) targets.push_back(legacy);
        }
        return targets;
    }

    std::vector<std::wstring> LoadTargetProfileKeys(const std::vector<std::wstring>& targetIds)
    {
        std::vector<std::wstring> keys = ReadRegistryMultiString(L"TargetEndpointProfiles");
        if (keys.size() != targetIds.size()) keys.clear();
        if (!keys.empty()) return keys;

        ComPtr<IMMDeviceEnumerator> enumerator;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        for (const auto& id : targetIds)
        {
            ComPtr<IMMDevice> device;
            std::wstring key;
            if (enumerator && SUCCEEDED(enumerator->GetDevice(id.c_str(), &device)))
                key = DeviceProfileKey(device.Get());
            keys.push_back(key.empty() ? L"ENDPOINT|" + id : key);
        }
        return keys;
    }

    HRESULT EnsureInitialTarget(IMMDeviceEnumerator* enumerator,
        std::vector<std::wstring>& targets)
    {
        if (!enumerator) return E_POINTER;
        RETURN_IF_FAILED(EnumeratePhysicalDevices(enumerator));

        const size_t configuredCount = targets.size();
        const bool hadConfiguredTargets = configuredCount != 0;
        const std::vector<std::wstring> savedProfiles =
            ReadRegistryMultiString(L"TargetEndpointProfiles");
        std::vector<std::wstring> resolved;
        const auto addUnique = [&resolved](const std::wstring& id)
        {
            if (!id.empty() && std::find(resolved.begin(), resolved.end(), id) == resolved.end())
                resolved.push_back(id);
        };

        // MMDevice endpoint IDs can change after a driver update, sleep/resume,
        // or jack re-enumeration. Resolve every saved endpoint through its
        // stable hardware profile instead of retrying a stale ID forever.
        for (size_t i = 0; i < targets.size(); ++i)
        {
            const auto byId = std::find_if(g_devices.begin(), g_devices.end(),
                [&](const DeviceInfo& item) { return item.id == targets[i]; });
            if (byId != g_devices.end())
            {
                addUnique(byId->id);
                continue;
            }
            if (i < savedProfiles.size())
            {
                const auto byProfile = std::find_if(g_devices.begin(), g_devices.end(),
                    [&](const DeviceInfo& item) { return item.profileKey == savedProfiles[i]; });
                if (byProfile != g_devices.end()) addUnique(byProfile->id);
            }
        }
        targets = std::move(resolved);

        if (hadConfiguredTargets)
        {
            if (targets.empty())
            {
                // A temporarily unplugged selection is still the user's
                // selection. Keep it in the registry and retry when MMDevice
                // publishes that hardware again; never jump to a random sink.
                return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            }
            if (targets.size() == configuredCount)
            {
                WriteRegistryMultiString(L"TargetEndpoints", targets);
                WriteRegistryMultiString(L"TargetEndpointProfiles",
                    savedProfiles.size() == configuredCount
                        ? savedProfiles : LoadTargetProfileKeys(targets));
            }
            return S_OK;
        }

        const std::wstring original = ReadRegistryString(L"OriginalDefaultEndpoint");
        if (targets.empty() && IsActiveEndpoint(enumerator, original))
            targets.push_back(original);

        ComPtr<IMMDevice> currentDefault;
        if (targets.empty() &&
            SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &currentDefault)) &&
            !IsVirtualName(DeviceName(currentDefault.Get())))
        {
            const std::wstring id = DeviceId(currentDefault.Get());
            if (!id.empty())
            {
                targets.push_back(id);
                WriteRegistryString(L"OriginalDefaultEndpoint", id);
            }
        }

        if (targets.empty())
        {
            const std::wstring restore = ResolveRestoreEndpoint(enumerator);
            if (!restore.empty()) targets.push_back(restore);
        }

        if (targets.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

        WriteRegistryMultiString(L"TargetEndpoints", targets);
        WriteRegistryMultiString(L"TargetEndpointProfiles", LoadTargetProfileKeys(targets));
        return S_OK;
    }

    HRESULT ResumeSavedRouting(bool force)
    {
        const bool requested = ReadRegistryDword(L"ResumeAfterUpgrade", 0) != 0;
        if (!force && !requested) return S_FALSE;

        ComPtr<IMMDeviceEnumerator> enumerator;
        RETURN_IF_FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));

        std::vector<std::wstring> targets = LoadTargetIds();
        RETURN_IF_FAILED(EnsureInitialTarget(enumerator.Get(), targets));

        // A driver rebind can replace the endpoint ID before AudioSrv has
        // published the replacement through MMDevice/IPolicyConfig. Re-discover
        // a fresh endpoint object until Windows reports it active.
        std::wstring virtualId;
        HRESULT hr = EnsureVirtualEndpointActive(enumerator.Get(), virtualId, 20000);
        if (FAILED(hr)) return hr;

        // Previous builds advertised 24/32-bit and high-rate formats.  Reset
        // the persisted endpoint format so Windows reopens this driver using
        // the canonical 48 kHz / 16-bit transport declared by the new graph.
        ResetEndpointFormat(virtualId);

        StopBackground();
        WaitForRouterStopped(10000);
        if (RouterProcessIsRunning()) return HRESULT_FROM_WIN32(ERROR_BUSY);
        WriteRegistryDword(L"RoutingEnabled", 1);
        SetAutoStart(true);
        if (!StartBackgroundProcess())
        {
            WriteRegistryDword(L"RoutingEnabled", 0);
            SetAutoStart(false);
            return HRESULT_FROM_WIN32(GetLastError());
        }
        if (!WaitForRouterReady(20000))
        {
            StopBackground();
            WaitForRouterStopped(10000);
            WriteRegistryDword(L"RoutingEnabled", 0);
            SetAutoStart(false);
            return HRESULT_FROM_WIN32(ERROR_NOT_READY);
        }

        // The physical render clients and HXAudio loopback capture are already
        // running at this point.  Only now is it safe to hand Windows' default
        // output to the virtual endpoint.
        hr = SetDefaultEndpoint(virtualId);
        if (FAILED(hr)) return hr;

        DeleteRegistryValue(L"ResumeAfterUpgrade");
        return S_OK;
    }

    std::vector<std::wstring> LoadPlaybackIds()
    {
        return ReadRegistryMultiString(L"PlaybackEndpoints");
    }

    std::vector<std::wstring> LoadPlaybackProfileKeys()
    {
        return ReadRegistryMultiString(L"PlaybackEndpointProfiles");
    }

    HRESULT ResolveActivePlaybackEndpoints(IMMDeviceEnumerator* enumerator,
        std::vector<std::wstring>& activeIds,
        std::vector<std::wstring>& activeProfiles)
    {
        if (!enumerator) return E_POINTER;
        RETURN_IF_FAILED(EnumeratePhysicalDevices(enumerator));
        std::vector<std::wstring> storedIds = LoadPlaybackIds();
        std::vector<std::wstring> storedProfiles = LoadPlaybackProfileKeys();

        if (storedIds.empty())
        {
            ComPtr<IMMDevice> current;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(
                eRender, eMultimedia, &current)) &&
                !IsVirtualName(DeviceName(current.Get())))
            {
                const std::wstring id = DeviceId(current.Get());
                if (!id.empty())
                {
                    storedIds.push_back(id);
                    storedProfiles.push_back(DeviceProfileKey(current.Get()));
                    WriteRegistryMultiString(L"PlaybackEndpoints", storedIds);
                    WriteRegistryMultiString(L"PlaybackEndpointProfiles",
                        storedProfiles);
                }
            }
        }
        if (storedProfiles.size() < storedIds.size())
            storedProfiles.resize(storedIds.size());

        bool changed = false;
        activeIds.clear();
        activeProfiles.clear();
        for (size_t index = 0; index < storedIds.size(); ++index)
        {
            auto device = std::find_if(g_devices.begin(), g_devices.end(),
                [&](const DeviceInfo& item)
                { return item.id == storedIds[index]; });
            if (device == g_devices.end() && !storedProfiles[index].empty())
            {
                device = std::find_if(g_devices.begin(), g_devices.end(),
                    [&](const DeviceInfo& item)
                    { return item.profileKey == storedProfiles[index]; });
            }
            if (device == g_devices.end()) continue;
            if (storedIds[index] != device->id)
            {
                storedIds[index] = device->id;
                changed = true;
            }
            if (storedProfiles[index] != device->profileKey)
            {
                storedProfiles[index] = device->profileKey;
                changed = true;
            }
            if (std::find(activeIds.begin(), activeIds.end(), device->id) ==
                activeIds.end())
            {
                activeIds.push_back(device->id);
                activeProfiles.push_back(device->profileKey);
            }
        }
        if (changed)
        {
            WriteRegistryMultiString(L"PlaybackEndpoints", storedIds);
            WriteRegistryMultiString(L"PlaybackEndpointProfiles",
                storedProfiles);
        }
        return activeIds.empty() ? HRESULT_FROM_WIN32(ERROR_NOT_FOUND) : S_OK;
    }

    HRESULT RunApoPlaybackSession(HANDLE stopEvent, HANDLE readyEvent,
        HANDLE changedEvent, HANDLE dspSettingsEvent,
        HANDLE defaultChangedEvent, ApplicationRouteOwnership& routeOwnership,
        const std::vector<std::wstring>& endpointIds,
        const std::vector<std::wstring>& profileKeys)
    {
        if (endpointIds.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        ApplyStaticHardwareCompensation(StaticGainApplyMode::All);
        ComPtr<IMMDeviceEnumerator> enumerator;
        RETURN_IF_FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));

        ComPtr<IMMDevice> source;
        RETURN_IF_FAILED(enumerator->GetDevice(endpointIds.front().c_str(), &source));
        const std::wstring sourceId = endpointIds.front();
        ResetEvent(changedEvent);

        if (endpointIds.size() == 1)
        {
            routeOwnership.Reconcile(enumerator.Get(), sourceId, true);
            WriteRegistryDword(L"ApoPlaybackLastError", ERROR_SUCCESS);
            WriteRegistryDword(L"ApoPlaybackActiveEndpoints", 1);
            SetEvent(readyEvent);
            HANDLE events[] = { stopEvent, changedEvent, dspSettingsEvent,
                defaultChangedEvent };
            while (true)
            {
                const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(events),
                    events, FALSE, 100);
                if (wait == WAIT_OBJECT_0 + 2)
                {
                    ApplyStaticHardwareCompensation(
                        StaticGainApplyMode::All);
                    continue;
                }
                if (wait == WAIT_OBJECT_0 + 3 || wait == WAIT_TIMEOUT)
                {
                    routeOwnership.Reconcile(enumerator.Get(), sourceId, true);
                    continue;
                }
                break;
            }
            ResetEvent(readyEvent);
            WriteRegistryDword(L"ApoPlaybackActiveEndpoints", 0);
            return S_OK;
        }

        HANDLE sessionStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!sessionStopEvent || !captureEvent)
        {
            const HRESULT error = HRESULT_FROM_WIN32(GetLastError());
            if (captureEvent) CloseHandle(captureEvent);
            if (sessionStopEvent) CloseHandle(sessionStopEvent);
            return error;
        }

        HRESULT hr = S_OK;
        ComPtr<IAudioClient> captureClient;
        ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX* captureFormat = nullptr;
        bool eventDriven = false;
        UINT32 periodFrames = 0;
        const DWORD bufferMs = std::clamp<DWORD>(
            ReadRegistryDword(L"ApoPlaybackBufferMs", 10), 3, 100);
        hr = source->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
            nullptr, &captureClient);
        if (SUCCEEDED(hr)) hr = captureClient->GetMixFormat(&captureFormat);
        WAVEFORMATEXTENSIBLE floatBusFormat{};
        if (SUCCEEDED(hr)) floatBusFormat = BuildFloatBusFormat(captureFormat);

        std::vector<std::unique_ptr<TargetWorker>> workers;
        if (SUCCEEDED(hr))
        {
            for (size_t index = 1; index < endpointIds.size(); ++index)
            {
                ComPtr<IMMDevice> target;
                enumerator->GetDevice(endpointIds[index].c_str(), &target);
                const std::wstring name = target
                    ? DeviceName(target.Get()) : endpointIds[index];
                const std::wstring profile = index < profileKeys.size()
                    ? profileKeys[index] : L"ENDPOINT|" + endpointIds[index];
                auto worker = std::make_unique<TargetWorker>(endpointIds[index],
                    name, profile, &floatBusFormat.Format, bufferMs,
                    sessionStopEvent, nullptr, index + 1, false);
                if (worker->Start()) workers.push_back(std::move(worker));
            }
            if (workers.empty()) hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (SUCCEEDED(hr))
        {
            size_t readyWorkers = 0;
            for (auto& worker : workers)
                if (worker->WaitUntilReady(10000) && worker->IsActive())
                    ++readyWorkers;
            if (readyWorkers == 0) hr = workers.front()->Result();
        }
        if (SUCCEEDED(hr))
        {
            captureClient.Reset();
            hr = ActivateSharedClient(source.Get(), captureFormat,
                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                bufferMs, captureClient, eventDriven, periodFrames);
        }
        if (SUCCEEDED(hr) && eventDriven)
            hr = captureClient->SetEventHandle(captureEvent);
        if (SUCCEEDED(hr)) hr = captureClient->GetService(IID_PPV_ARGS(&capture));
        if (SUCCEEDED(hr)) hr = captureClient->Start();
        if (SUCCEEDED(hr))
        {
            WriteRegistryDword(L"ApoPlaybackLastError", ERROR_SUCCESS);
            WriteRegistryDword(L"ApoPlaybackActiveEndpoints",
                static_cast<DWORD>(endpointIds.size()));
            routeOwnership.Reconcile(enumerator.Get(), sourceId, true);
            SetEvent(readyEvent);
        }

        std::vector<float> floatBus;
        ULONGLONG nextRecovery = GetTickCount64() + 1000;
        ULONGLONG nextRouteReconcile = 0;
        while (SUCCEEDED(hr))
        {
            const ULONGLONG now = GetTickCount64();
            if (now >= nextRouteReconcile)
            {
                routeOwnership.Reconcile(enumerator.Get(), sourceId, true);
                nextRouteReconcile = now + 100;
            }
            if (now >= nextRecovery)
            {
                for (auto& worker : workers) worker->RestartIfNeeded();
                nextRecovery = now + 1000;
            }

            DWORD wait = WAIT_TIMEOUT;
            if (eventDriven)
            {
                HANDLE events[] = { stopEvent, captureEvent, changedEvent,
                    dspSettingsEvent, defaultChangedEvent };
                wait = WaitForMultipleObjects(ARRAYSIZE(events), events,
                    FALSE, 100);
                if (wait == WAIT_OBJECT_0) break;
                if (wait == WAIT_OBJECT_0 + 2)
                {
                    hr = AUDCLNT_E_DEVICE_INVALIDATED;
                    break;
                }
                if (wait == WAIT_OBJECT_0 + 3)
                {
                    ApplyStaticHardwareCompensation(
                        StaticGainApplyMode::All);
                    continue;
                }
                if (wait == WAIT_OBJECT_0 + 4)
                {
                    routeOwnership.Reconcile(enumerator.Get(), sourceId, true);
                    continue;
                }
                if (wait != WAIT_OBJECT_0 + 1 && wait != WAIT_TIMEOUT)
                {
                    hr = E_FAIL;
                    break;
                }
            }
            else
            {
                HANDLE events[] = { stopEvent, changedEvent,
                    dspSettingsEvent, defaultChangedEvent };
                wait = WaitForMultipleObjects(ARRAYSIZE(events), events,
                    FALSE, std::max<DWORD>(1, bufferMs / 2));
                if (wait == WAIT_OBJECT_0) break;
                if (wait == WAIT_OBJECT_0 + 1)
                {
                    hr = AUDCLNT_E_DEVICE_INVALIDATED;
                    break;
                }
                if (wait == WAIT_OBJECT_0 + 2)
                {
                    ApplyStaticHardwareCompensation(
                        StaticGainApplyMode::All);
                    continue;
                }
                if (wait == WAIT_OBJECT_0 + 3)
                {
                    routeOwnership.Reconcile(enumerator.Get(), sourceId, true);
                    continue;
                }
                if (wait != WAIT_TIMEOUT)
                {
                    hr = E_FAIL;
                    break;
                }
            }

            UINT32 packetFrames = 0;
            hr = capture->GetNextPacketSize(&packetFrames);
            while (SUCCEEDED(hr) && packetFrames != 0)
            {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;
                const BYTE* packet =
                    (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 ? nullptr : data;
                const bool converted = ConvertToFloatBus(
                    packet, frames, captureFormat, floatBus);
                if (converted)
                    for (auto& worker : workers)
                        worker->Push(floatBus.data(), frames);
                const HRESULT released = capture->ReleaseBuffer(frames);
                if (FAILED(released)) hr = released;
                else if (!converted) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
                if (FAILED(hr)) break;
                hr = capture->GetNextPacketSize(&packetFrames);
            }
        }

        ResetEvent(readyEvent);
        WriteRegistryDword(L"ApoPlaybackActiveEndpoints", 0);
        if (captureClient) captureClient->Stop();
        SetEvent(sessionStopEvent);
        for (auto& worker : workers) worker->Join();
        if (captureFormat) CoTaskMemFree(captureFormat);
        CloseHandle(captureEvent);
        CloseHandle(sessionStopEvent);
        return hr;
    }

    HRESULT RunApoPlayback()
    {
        HANDLE mutex = CreateMutexW(nullptr, FALSE, kRouterMutex);
        if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            if (mutex) CloseHandle(mutex);
            return S_FALSE;
        }
        HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, kStopEvent);
        HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, kRouterReadyEvent);
        HANDLE changedEvent = CreateEventW(nullptr, FALSE, FALSE,
            kRoutingSettingsEvent);
        HANDLE dspSettingsEvent = CreateEventW(nullptr, FALSE, FALSE,
            kDspSettingsEvent);
        HANDLE defaultChangedEvent = CreateEventW(nullptr, FALSE, FALSE,
            nullptr);
        if (!stopEvent || !readyEvent || !changedEvent || !dspSettingsEvent ||
            !defaultChangedEvent)
        {
            const HRESULT error = HRESULT_FROM_WIN32(GetLastError());
            if (defaultChangedEvent) CloseHandle(defaultChangedEvent);
            if (dspSettingsEvent) CloseHandle(dspSettingsEvent);
            if (changedEvent) CloseHandle(changedEvent);
            if (readyEvent) CloseHandle(readyEvent);
            if (stopEvent) CloseHandle(stopEvent);
            CloseHandle(mutex);
            return error;
        }
        ResetEvent(stopEvent);
        ResetEvent(readyEvent);
        ConfigureRealtimeThread(0, L"HXAudio APO playback supervisor");
        MmcssRegistration mmcss;

        ComPtr<IMMDeviceEnumerator> notificationEnumerator;
        HRESULT notificationResult = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&notificationEnumerator));
        RouterDeviceNotifications notifications(changedEvent,
            defaultChangedEvent,
            std::wstring(), true);
        if (SUCCEEDED(notificationResult))
            notificationResult = notificationEnumerator->
                RegisterEndpointNotificationCallback(&notifications);

        HRESULT lastResult = S_OK;
        ApplicationRouteOwnership routeOwnership;
        while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0)
        {
            ResetEvent(changedEvent);
            ApplyStaticHardwareCompensation(StaticGainApplyMode::All);
            if (ReadRegistryDword(L"ApoPlaybackEnabled", 0) == 0)
            {
                routeOwnership.ReleaseAll();
                WriteRegistryDword(L"ApoPlaybackActiveEndpoints", 0);
                SetEvent(readyEvent);
                HANDLE idleEvents[] = { stopEvent, changedEvent,
                    dspSettingsEvent, defaultChangedEvent };
                const DWORD idleWait = WaitForMultipleObjects(
                    ARRAYSIZE(idleEvents), idleEvents, FALSE, INFINITE);
                ResetEvent(readyEvent);
                if (idleWait == WAIT_OBJECT_0) break;
                continue;
            }
            std::vector<std::wstring> endpointIds;
            std::vector<std::wstring> profiles;
            ComPtr<IMMDeviceEnumerator> enumerator;
            lastResult = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
            if (SUCCEEDED(lastResult))
                lastResult = ResolveActivePlaybackEndpoints(
                    enumerator.Get(), endpointIds, profiles);
            if (SUCCEEDED(lastResult))
                lastResult = RunApoPlaybackSession(stopEvent, readyEvent,
                    changedEvent, dspSettingsEvent, defaultChangedEvent,
                    routeOwnership, endpointIds, profiles);
            WriteRegistryDword(L"ApoPlaybackLastError",
                static_cast<DWORD>(lastResult));
            if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
            if (FAILED(lastResult))
                WaitForSingleObject(changedEvent, 500);
        }

        ResetEvent(readyEvent);
        routeOwnership.ReleaseAll();
        WriteRegistryDword(L"ApoPlaybackActiveEndpoints", 0);
        if (SUCCEEDED(notificationResult) && notificationEnumerator)
            notificationEnumerator->UnregisterEndpointNotificationCallback(
                &notifications);
        CloseHandle(changedEvent);
        CloseHandle(dspSettingsEvent);
        CloseHandle(defaultChangedEvent);
        CloseHandle(readyEvent);
        CloseHandle(stopEvent);
        CloseHandle(mutex);
        return S_OK;
    }

    HRESULT RunRouterSession(HANDLE routerStopEvent, HANDLE readyEvent, HANDLE sourceChangedEvent)
    {
        HANDLE sessionStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE dspSettingsEvent = CreateEventW(nullptr, FALSE, FALSE, kDspSettingsEvent);
        if (!sessionStopEvent || !captureEvent || !dspSettingsEvent)
        {
            const HRESULT error = HRESULT_FROM_WIN32(GetLastError());
            if (dspSettingsEvent) CloseHandle(dspSettingsEvent);
            if (captureEvent) CloseHandle(captureEvent);
            if (sessionStopEvent) CloseHandle(sessionStopEvent);
            return error;
        }

        HRESULT hr = S_OK;
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> source;
        ComPtr<IAudioClient> captureClient;
        ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX* captureFormat = nullptr;
        bool eventDriven = false;
        UINT32 periodFrames = 0;
        const DWORD bufferMs = std::clamp<DWORD>(ReadRegistryDword(L"BufferMs", 5), 3, 100);
        std::vector<std::wstring> targetIds = LoadTargetIds();
        std::vector<std::wstring> targetProfileKeys;

        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(hr)) hr = EnsureInitialTarget(enumerator.Get(), targetIds);
        if (SUCCEEDED(hr)) targetProfileKeys = LoadTargetProfileKeys(targetIds);
        if (SUCCEEDED(hr)) RestoreStaticHardwareCompensation();
        if (SUCCEEDED(hr)) RestoreHardwareOriginals(true);
        if (SUCCEEDED(hr)) hr = FindVirtualDevice(enumerator.Get(), source);
        if (SUCCEEDED(hr)) hr = source->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
            nullptr, &captureClient);
        if (SUCCEEDED(hr)) hr = captureClient->GetMixFormat(&captureFormat);

        WAVEFORMATEXTENSIBLE floatBusFormat{};
        if (SUCCEEDED(hr)) floatBusFormat = BuildFloatBusFormat(captureFormat);
        DspProcessor dsp;
        if (SUCCEEDED(hr))
        {
            DspSettings commonSettings = LoadDspSettings();
            commonSettings.peqBands.clear();
            dsp.Configure(floatBusFormat.Format.nSamplesPerSec,
                floatBusFormat.Format.nChannels, commonSettings);
        }

        std::vector<std::unique_ptr<TargetWorker>> workers;
        if (SUCCEEDED(hr))
        {
            for (size_t i = 0; i < targetIds.size(); ++i)
            {
                ComPtr<IMMDevice> target;
                enumerator->GetDevice(targetIds[i].c_str(), &target);
                const std::wstring targetName = target ? DeviceName(target.Get()) : targetIds[i];
                auto worker = std::make_unique<TargetWorker>(targetIds[i], targetName,
                    targetProfileKeys[i],
                    &floatBusFormat.Format, bufferMs, sessionStopEvent, nullptr, i + 1);
                if (worker->Start()) workers.push_back(std::move(worker));
            }
            if (workers.empty()) hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }

        if (SUCCEEDED(hr))
        {
            size_t activeWorkers = 0;
            for (auto& worker : workers)
            {
                if (worker->WaitUntilReady(10000) && worker->IsActive()) ++activeWorkers;
            }
            if (activeWorkers == 0) hr = workers.front()->Result();
        }

        if (SUCCEEDED(hr))
        {
            captureClient.Reset();
            hr = ActivateSharedClient(source.Get(), captureFormat,
                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                bufferMs, captureClient, eventDriven, periodFrames);
        }
        if (SUCCEEDED(hr) && eventDriven) hr = captureClient->SetEventHandle(captureEvent);
        if (SUCCEEDED(hr)) hr = captureClient->GetService(IID_PPV_ARGS(&capture));
        if (SUCCEEDED(hr)) hr = captureClient->Start();
        if (SUCCEEDED(hr))
        {
            // The background router owns this invariant. Any automatic restart
            // (login, device re-enumeration, or process recovery) therefore
            // restores HXAudio as the default without requiring a UI button.
            if (ReadRegistryDword(L"RoutingEnabled", 0) != 0)
                SetDefaultEndpoint(DeviceId(source.Get()));
            WriteRegistryDword(L"RouterLastError", ERROR_SUCCESS);
            SetEvent(readyEvent);
        }

        ULONGLONG nextDiagnosticRefresh = 0;
        ULONGLONG nextTargetRecovery = GetTickCount64() + 1000;
        std::vector<float> floatBusBuffer;
        while (SUCCEEDED(hr))
        {
            const ULONGLONG loopNow = GetTickCount64();
            if (loopNow >= nextTargetRecovery)
            {
                for (auto& worker : workers) worker->RestartIfNeeded();
                nextTargetRecovery = loopNow + 1000;
            }
            if (WaitForSingleObject(dspSettingsEvent, 0) == WAIT_OBJECT_0)
            {
                DspSettings commonSettings = LoadDspSettings();
                commonSettings.peqBands.clear();
                dsp.Configure(floatBusFormat.Format.nSamplesPerSec,
                    floatBusFormat.Format.nChannels, commonSettings);
                for (auto& worker : workers)
                    worker->ConfigurePeq(LoadPeqBandsForDevice(
                        worker->ProfileKey(), worker->Id()));
            }

            DWORD waitResult = WAIT_TIMEOUT;
            if (eventDriven)
            {
                HANDLE events[] = { routerStopEvent, captureEvent, sourceChangedEvent };
                // A loopback stream can remain formally valid after a player
                // tears down one shared-mode session and opens the next song.
                // Use a bounded wait so a previously active but now orphaned
                // capture client is rebuilt instead of staying silent forever.
                waitResult = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, 500);
                if (waitResult == WAIT_OBJECT_0) break;
                if (waitResult == WAIT_OBJECT_0 + 2)
                {
                    hr = AUDCLNT_E_DEVICE_INVALIDATED;
                    break;
                }
                if (waitResult != WAIT_OBJECT_0 + 1 && waitResult != WAIT_TIMEOUT)
                {
                    hr = E_FAIL;
                    break;
                }
            }
            else
            {
                HANDLE events[] = { routerStopEvent, sourceChangedEvent };
                waitResult = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE,
                    std::max<DWORD>(1, bufferMs / 2));
                if (waitResult == WAIT_OBJECT_0) break;
                if (waitResult == WAIT_OBJECT_0 + 1)
                {
                    hr = AUDCLNT_E_DEVICE_INVALIDATED;
                    break;
                }
                if (waitResult != WAIT_TIMEOUT)
                {
                    hr = E_FAIL;
                    break;
                }
            }

            UINT32 packetFrames = 0;
            hr = capture->GetNextPacketSize(&packetFrames);
            while (SUCCEEDED(hr) && packetFrames > 0)
            {
                BYTE* sourceData = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = capture->GetBuffer(&sourceData, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;

                const BYTE* inputPacket =
                    (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 ? nullptr : sourceData;
                HRESULT conversionResult = S_OK;
                if (!ConvertToFloatBus(inputPacket, frames, captureFormat, floatBusBuffer))
                {
                    conversionResult = AUDCLNT_E_UNSUPPORTED_FORMAT;
                }
                else
                {
                    const DWORD capturedPeak = FloatPeakQ31(floatBusBuffer);
                    const FloatStatistics capturedStatistics = MeasureFloatStatistics(
                        floatBusBuffer, floatBusFormat.Format.nChannels);
                    dsp.Process(floatBusBuffer.data(), frames);
                    const DWORD dspPeak = FloatPeakQ31(floatBusBuffer);
                    const ULONGLONG diagnosticNow = GetTickCount64();
                    if (diagnosticNow >= nextDiagnosticRefresh)
                    {
                        WriteRegistryDword(L"DiagnosticCapturedPeakQ31", capturedPeak);
                        WriteRegistryDword(L"DiagnosticDspPeakQ31", dspPeak);
                        WriteRegistryDword(L"DiagnosticPacketFrames", frames);
                        WriteRegistryDword(L"DiagnosticPacketFlags", flags);
                        WriteRegistryDword(L"DiagnosticCapturedRmsQ31", capturedStatistics.rmsQ31);
                        WriteRegistryDword(L"DiagnosticCapturedAcRmsQ31", capturedStatistics.acRmsQ31);
                        WriteRegistryDword(L"DiagnosticCapturedDeltaRmsQ31", capturedStatistics.deltaRmsQ31);
                        WriteRegistryDword(L"DiagnosticCapturedZeroCrossings", capturedStatistics.zeroCrossings);
                        WriteRegistryDword(L"DiagnosticCapturedNonFinite", capturedStatistics.nonFinite);
                        WriteRegistryDword(L"DiagnosticCapturedMeanQ31",
                            static_cast<DWORD>(capturedStatistics.meanQ31));
                        if (inputPacket && frames != 0)
                        {
                            DWORD firstSamples[4]{};
                            const size_t availableBytes = static_cast<size_t>(frames) *
                                captureFormat->nBlockAlign;
                            CopyMemory(firstSamples, inputPacket,
                                std::min(availableBytes, sizeof(firstSamples)));
                            WriteRegistryDword(L"DiagnosticRawSample0", firstSamples[0]);
                            WriteRegistryDword(L"DiagnosticRawSample1", firstSamples[1]);
                            WriteRegistryDword(L"DiagnosticRawSample2", firstSamples[2]);
                            WriteRegistryDword(L"DiagnosticRawSample3", firstSamples[3]);
                        }
                        if (!workers.empty())
                        {
                            auto diagnostic = std::find_if(workers.begin(), workers.end(),
                                [](const std::unique_ptr<TargetWorker>& candidate)
                                { return candidate->IsActive(); });
                            if (diagnostic == workers.end()) diagnostic = workers.begin();
                            TargetWorker* worker = diagnostic->get();
                            WriteRegistryDword(L"DiagnosticWorkerActive", worker->IsActive() ? 1 : 0);
                            WriteRegistryDword(L"DiagnosticTargetResult", static_cast<DWORD>(worker->Result()));
                            WriteRegistryDword(L"DiagnosticTargetCopiedBytes", worker->LastCopiedBytes());
                            WriteRegistryDword(L"DiagnosticTargetPeakQ31", worker->LastPeakQ31());
                            WriteRegistryDword(L"DiagnosticTargetRingBytes", worker->RingBytes());
                            WriteRegistryDword(L"DiagnosticTargetPaddingFrames", worker->LastPaddingFrames());
                            WriteRegistryDword(L"DiagnosticTargetVolumeQ31", worker->EndpointVolumeQ31());
                            WriteRegistryDword(L"DiagnosticTargetMuted", worker->EndpointMuted());
                            WriteRegistryDword(L"DiagnosticSessionVolumeQ31", worker->SessionVolumeQ31());
                            WriteRegistryDword(L"DiagnosticSessionMuted", worker->SessionMuted());
                            WriteRegistryDword(L"DiagnosticTargetMeterQ31", worker->EndpointMeterQ31());
                        }
                        nextDiagnosticRefresh = diagnosticNow + 250;
                    }

                    for (auto& worker : workers) worker->Push(floatBusBuffer.data(), frames);
                }

                const HRESULT releaseResult = capture->ReleaseBuffer(frames);
                if (FAILED(releaseResult)) hr = releaseResult;
                if (FAILED(hr)) break;
                if (FAILED(conversionResult))
                {
                    hr = conversionResult;
                    break;
                }
                hr = capture->GetNextPacketSize(&packetFrames);
            }

        }

        ResetEvent(readyEvent);
        if (captureClient) captureClient->Stop();
        SetEvent(sessionStopEvent);
        for (auto& worker : workers) worker->Join();
        RestoreHardwareOriginals(false);
        if (captureFormat) CoTaskMemFree(captureFormat);
        CloseHandle(dspSettingsEvent);
        CloseHandle(captureEvent);
        CloseHandle(sessionStopEvent);
        return hr;
    }

    HRESULT RunRouter()
    {
        HANDLE mutex = CreateMutexW(nullptr, FALSE, kRouterMutex);
        if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            if (mutex) CloseHandle(mutex);
            return S_FALSE;
        }

        HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, kStopEvent);
        HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, kRouterReadyEvent);
        HANDLE sourceChangedEvent = CreateEventW(nullptr, FALSE, FALSE, kRoutingSettingsEvent);
        HANDLE defaultChangedEvent = CreateEventW(nullptr, FALSE, TRUE, nullptr);
        HANDLE routeStoppedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stopEvent || !readyEvent || !sourceChangedEvent || !defaultChangedEvent ||
            !routeStoppedEvent)
        {
            const HRESULT error = HRESULT_FROM_WIN32(GetLastError());
            if (routeStoppedEvent) CloseHandle(routeStoppedEvent);
            if (defaultChangedEvent) CloseHandle(defaultChangedEvent);
            if (sourceChangedEvent) CloseHandle(sourceChangedEvent);
            if (readyEvent) CloseHandle(readyEvent);
            if (stopEvent) CloseHandle(stopEvent);
            CloseHandle(mutex);
            return error;
        }
        ResetEvent(stopEvent);
        ResetEvent(readyEvent);

        ConfigureRealtimeThread(0, L"HXAudio routing supervisor");
        MmcssRegistration mmcss;

        DefaultEndpointGuardContext defaultGuardContext{
            stopEvent, defaultChangedEvent, routeStoppedEvent };
        HANDLE defaultGuardThread = CreateThread(nullptr, 0,
            DefaultEndpointGuardThread, &defaultGuardContext, 0, nullptr);
        if (!defaultGuardThread)
        {
            const HRESULT error = HRESULT_FROM_WIN32(GetLastError());
            CloseHandle(routeStoppedEvent);
            CloseHandle(defaultChangedEvent);
            CloseHandle(sourceChangedEvent);
            CloseHandle(readyEvent);
            CloseHandle(stopEvent);
            CloseHandle(mutex);
            return error;
        }

        ComPtr<IMMDeviceEnumerator> notificationEnumerator;
        HRESULT notificationResult = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, IID_PPV_ARGS(&notificationEnumerator));
        std::wstring notificationSourceId;
        if (SUCCEEDED(notificationResult))
        {
            ComPtr<IMMDevice> notificationSource;
            if (SUCCEEDED(FindVirtualDevice(notificationEnumerator.Get(), notificationSource)))
                notificationSourceId = DeviceId(notificationSource.Get());
        }
        RouterDeviceNotifications notifications(sourceChangedEvent,
            defaultChangedEvent, notificationSourceId);
        if (SUCCEEDED(notificationResult))
        {
            notificationResult = notificationEnumerator->RegisterEndpointNotificationCallback(&notifications);
        }

        HRESULT lastResult = S_OK;
        while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0)
        {
            ResetEvent(sourceChangedEvent);
            ResetEvent(readyEvent);
            lastResult = RunRouterSession(stopEvent, readyEvent, sourceChangedEvent);
            WriteRegistryDword(L"RouterLastError", static_cast<DWORD>(lastResult));
            if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
            WaitForSingleObject(stopEvent, 500);
        }
        SetEvent(routeStoppedEvent);

        ResetEvent(readyEvent);
        if (SUCCEEDED(notificationResult) && notificationEnumerator)
        {
            notificationEnumerator->UnregisterEndpointNotificationCallback(&notifications);
        }
        if (defaultGuardThread)
        {
            SetEvent(stopEvent);
            WaitForSingleObject(defaultGuardThread, 3000);
            CloseHandle(defaultGuardThread);
        }
        CloseHandle(defaultChangedEvent);
        CloseHandle(routeStoppedEvent);
        CloseHandle(sourceChangedEvent);
        CloseHandle(readyEvent);
        CloseHandle(stopEvent);
        CloseHandle(mutex);
        return S_OK;
    }

    DWORD CurrentBufferMs()
    {
        if (!g_bufferSlider) return 5;
        const LRESULT position = SendMessageW(g_bufferSlider, TBM_GETPOS, 0, 0);
        const size_t index = static_cast<size_t>(std::clamp<LRESULT>(position, 0, ARRAYSIZE(kBufferChoices) - 1));
        return kBufferChoices[index];
    }

    bool SetWindowTextIfChanged(HWND control, const std::wstring& text)
    {
        if (!control) return false;
        const int length = GetWindowTextLengthW(control);
        wchar_t current[1024]{};
        if (length < static_cast<int>(ARRAYSIZE(current)))
        {
            GetWindowTextW(control, current, ARRAYSIZE(current));
            if (text == current) return false;
        }
        SetWindowTextW(control, text.c_str());
        return true;
    }

    void UpdateBufferLabel()
    {
        if (!g_bufferValue) return;
        if (g_apoMode)
        {
            SetWindowTextIfChanged(g_bufferValue, L"由上方“输出”开关按设备独立接管");
            return;
        }
        const DWORD milliseconds = CurrentBufferMs();
        std::wstring text = std::to_wstring(milliseconds) + L" ms";
        if (milliseconds <= 3) text += L"（最低延迟 / 较高 CPU）";
        else if (milliseconds <= 5) text += L"（低延迟）";
        else if (milliseconds <= 10) text += L"（均衡）";
        else text += L"（更稳 / 较低 CPU）";
        SetWindowTextIfChanged(g_bufferValue, text);
    }

    void UpdateRouterStatusLabel()
    {
        // Keep process recovery independent from any visual status control.
        // The UI intentionally has no periodically rewritten status label: an
        // opaque, stable header avoids both overdraw artifacts and timer-driven
        // repaints while the background route still self-heals.
        if (!g_apoMode && ReadRegistryDword(L"RoutingEnabled", 0) != 0 &&
            !RouterProcessIsRunning())
        {
            StartBackgroundProcess();
        }
        if (g_apoMode && !RouterProcessIsRunning())
        {
            SetAutoStart(true);
            StartBackgroundProcess();
        }
        if (!g_status) return;
        if (g_apoMode)
        {
            const DWORD endpoints = ReadMachineRegistryDword(L"ApoAttachedEndpoints", 0);
            const DWORD guard = ReadMachineRegistryDword(L"ApoGuardState", 0);
            const std::wstring state = guard == 1
                ? L"状态：APO 已自动接管 " + std::to_wstring(endpoints) + L" 个输出端点"
                : L"状态：APO 已安装，自动挂接服务正在恢复";
            SetWindowTextW(g_status, state.c_str());
            return;
        }
        const std::wstring state = RouterIsReady()
            ? L"状态：已接管所选输出设备"
            : (RouterProcessIsRunning() ? L"状态：正在连接所选输出设备" : L"状态：等待音频设备连接");
        const std::wstring cpu = L" | CPU：" + std::to_wstring(g_cpu.physicalCores) + L" 核 / " +
            std::to_wstring(g_cpu.logicalProcessors.size()) + L" 线程";
        SetWindowTextW(g_status, (state + cpu + EngineBypassStatusText()).c_str());
    }

    void RefreshDevices()
    {
        g_refreshingDeviceList = true;
        SendMessageW(g_deviceList, WM_SETREDRAW, FALSE, 0);
        SendMessageW(g_deviceList, LB_RESETCONTENT, 0, 0);
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
            FAILED(EnumeratePhysicalDevices(enumerator.Get())))
        {
            g_refreshingDeviceList = false;
            SendMessageW(g_deviceList, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(g_deviceList, nullptr, FALSE);
            if (g_status) SetWindowTextW(g_status, L"状态：无法枚举输出设备");
            return;
        }

        const bool selectionConfigured =
            RegistryValueIsType(L"TargetEndpoints", REG_MULTI_SZ) ||
            RegistryValueIsType(L"TargetEndpointProfiles", REG_MULTI_SZ);
        std::vector<std::wstring> selected = ReadRegistryMultiString(L"TargetEndpoints");
        const std::vector<std::wstring> selectedProfiles =
            ReadRegistryMultiString(L"TargetEndpointProfiles");
        const bool playbackConfigured =
            RegistryValueIsType(L"PlaybackEndpoints", REG_MULTI_SZ) ||
            RegistryValueIsType(L"PlaybackEndpointProfiles", REG_MULTI_SZ);
        std::vector<std::wstring> playback =
            ReadRegistryMultiString(L"PlaybackEndpoints");
        const std::vector<std::wstring> playbackProfiles =
            ReadRegistryMultiString(L"PlaybackEndpointProfiles");
        if (!g_apoMode && selected.empty())
        {
            const std::wstring legacyTarget = ReadRegistryString(L"TargetEndpoint");
            if (!legacyTarget.empty()) selected.push_back(legacyTarget);
        }
        if (!g_apoMode && selected.empty())
        {
            ComPtr<IMMDevice> currentDefault;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &currentDefault)) &&
                !IsVirtualName(DeviceName(currentDefault.Get())))
            {
                selected.push_back(DeviceId(currentDefault.Get()));
            }
            else
            {
                const std::wstring original = ReadRegistryString(L"OriginalDefaultEndpoint");
                if (!original.empty()) selected.push_back(original);
            }
        }
        if (g_apoMode && !playbackConfigured)
        {
            ComPtr<IMMDevice> currentDefault;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(
                eRender, eMultimedia, &currentDefault)) &&
                !IsVirtualName(DeviceName(currentDefault.Get())))
                playback.push_back(DeviceId(currentDefault.Get()));
        }

        for (size_t i = 0; i < g_devices.size(); ++i)
        {
            g_devices[i].playbackSelected =
                std::find(playback.begin(), playback.end(), g_devices[i].id) !=
                    playback.end() ||
                std::find(playbackProfiles.begin(), playbackProfiles.end(),
                    g_devices[i].profileKey) != playbackProfiles.end();
            SendMessageW(g_deviceList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(g_devices[i].name.c_str()));
            if ((g_apoMode && !selectionConfigured) ||
                std::find(selected.begin(), selected.end(), g_devices[i].id) != selected.end() ||
                std::find(selectedProfiles.begin(), selectedProfiles.end(),
                    g_devices[i].profileKey) != selectedProfiles.end())
            {
                SendMessageW(g_deviceList, LB_SETSEL, TRUE, static_cast<LPARAM>(i));
            }
        }

        g_refreshingDeviceList = false;
        SendMessageW(g_deviceList, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_deviceList, nullptr, FALSE);
        QueueEndpointVolumeRefresh(g_deviceList);
        UpdateRouterStatusLabel();
    }

    std::vector<std::wstring> SelectedDeviceIds()
    {
        std::vector<std::wstring> selected;
        for (size_t i = 0; i < g_devices.size(); ++i)
        {
            if (SendMessageW(g_deviceList, LB_GETSEL, static_cast<WPARAM>(i), 0) > 0)
            {
                selected.push_back(g_devices[i].id);
            }
        }
        return selected;
    }

    std::vector<std::wstring> SelectedPlaybackDeviceIds()
    {
        std::vector<std::wstring> selected;
        for (const auto& device : g_devices)
            if (device.playbackSelected) selected.push_back(device.id);
        return selected;
    }

    void TogglePlaybackSelection(HWND window, int index)
    {
        if (!g_apoMode || index < 0 ||
            static_cast<size_t>(index) >= g_devices.size()) return;
        DeviceInfo& device = g_devices[static_cast<size_t>(index)];
        device.playbackSelected = !device.playbackSelected;
        InvalidateMixerChannel(window, index);
        QueueApoPlaybackApply();
    }

    std::wstring PhysicalDeviceName(const std::wstring& id)
    {
        const auto device = std::find_if(g_devices.begin(), g_devices.end(),
            [&](const DeviceInfo& item) { return item.id == id; });
        return device != g_devices.end() ? device->name : id;
    }

    DeviceInfo PhysicalDeviceInfo(const std::wstring& id)
    {
        const auto device = std::find_if(g_devices.begin(), g_devices.end(),
            [&](const DeviceInfo& item) { return item.id == id; });
        if (device != g_devices.end()) return *device;
        return { id, id, L"ENDPOINT|" + id };
    }

    std::vector<DeviceInfo> PeqEditableDevices()
    {
        std::vector<std::wstring> ids = SelectedDeviceIds();
        if (ids.empty()) ids = LoadTargetIds();
        if (ids.empty())
        {
            for (const auto& device : g_devices) ids.push_back(device.id);
        }

        std::vector<DeviceInfo> devices;
        for (const auto& id : ids)
        {
            if (id.empty() || std::any_of(devices.begin(), devices.end(),
                [&](const DeviceInfo& item) { return item.id == id; })) continue;
            devices.push_back(PhysicalDeviceInfo(id));
        }
        return devices;
    }

    int SliderPosition(HWND slider)
    {
        return slider ? static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0)) : 0;
    }

    std::wstring ReadWindowText(HWND control)
    {
        if (!control) return {};
        const int length = GetWindowTextLengthW(control);
        std::vector<wchar_t> text(static_cast<size_t>(length) + 1, L'\0');
        GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
        return text.data();
    }

    void UpdatePeqSummary()
    {
        if (!g_peqSummary) return;
        const auto devices = PeqEditableDevices();
        std::wstring text;
        if (devices.size() == 1)
        {
            const auto bands = LoadPeqBandsForDevice(
                devices.front().profileKey, devices.front().id);
            const size_t enabled = static_cast<size_t>(std::count_if(bands.begin(), bands.end(),
                [](const PeqBand& band) { return band.enabled; }));
            text = L"PEQ：" + devices.front().name + L" | " + std::to_wstring(enabled) +
                L" 个启用 / " + std::to_wstring(bands.size()) + L" 个频段";
        }
        else
        {
            text = L"PEQ：按设备独立配置 | 当前选择 " +
                std::to_wstring(devices.size()) + L" 台输出";
        }
        SetWindowTextIfChanged(g_peqSummary, text);
    }

    void SignalDspSettingsChanged();
    void ReloadVirtualBassEditor();

    void UpdateVirtualBassSummary()
    {
        if (!g_virtualBassSummary) return;
        const DspSettings settings = LoadDspSettings();
        if (g_virtualBassMasterEnable)
        {
            const LRESULT requested = settings.virtualBassEnabled
                ? BST_CHECKED : BST_UNCHECKED;
            if (SendMessageW(g_virtualBassMasterEnable, BM_GETCHECK, 0, 0) != requested)
                SendMessageW(g_virtualBassMasterEnable, BM_SETCHECK, requested, 0);
        }
        std::wstring mode;
        if (settings.virtualBassMode == VirtualBassMode::Harmonics357)
            mode = L"3/5/7";
        else if (settings.virtualBassMode == VirtualBassMode::Custom)
            mode = L"自定义 " + FormatParameter(settings.virtualBassMultipliers[0]) +
                L"/" + FormatParameter(settings.virtualBassMultipliers[1]) + L"/" +
                FormatParameter(settings.virtualBassMultipliers[2]);
        else
            mode = L"2/3/4";
        std::wstring text = settings.virtualBassEnabled ? L"虚拟低频：已启用 " : L"虚拟低频：已关闭 ";
        text += mode;
        text += L" | 替换 < " + FormatParameter(settings.virtualBassCutoffHz) + L" Hz";
        text += settings.virtualBassSubsonicHz > 0.0
            ? L" | 超低切除 < " + FormatParameter(settings.virtualBassSubsonicHz) + L" Hz"
            : L" | 超低切除：关闭";
        text += L" | 强度 " + FormatParameter(settings.virtualBassAmount * 100.0) + L"%";
        SetWindowTextIfChanged(g_virtualBassSummary, text);
    }

    void SetVirtualBassMasterEnabled(bool enabled)
    {
        WriteRegistryDword(L"DspVirtualBassEnabled", enabled ? 1 : 0);
        SignalDspSettingsChanged();
        UpdateVirtualBassSummary();
        if (g_virtualBassWindow) ReloadVirtualBassEditor();
        if (g_hardwareStatus)
        {
            SetWindowTextW(g_hardwareStatus, enabled
                ? L"虚拟低频总开关已开启，现有参数已实时恢复。"
                : L"虚拟低频总开关已关闭，整个虚拟低频模块已旁路。");
        }
    }

    void UpdateHardwareLabels()
    {
        UpdateVirtualBassSummary();
        UpdatePeqSummary();
    }

    bool SaveHardwareUiSettings(HWND owner)
    {
        double preampDb = 0.0;
        if (!TryParseFiniteDouble(ReadWindowText(g_preampEdit), preampDb))
        {
            MessageBoxW(owner, L"总增益必须是有限数字，可使用小数或科学计数法。", L"HXAudio PEQ",
                MB_OK | MB_ICONWARNING);
            SetFocus(g_preampEdit);
            return false;
        }
        if (preampDb < HXAudioDsp::kMinimumGainDb ||
            preampDb > HXAudioDsp::kMaximumGainDb)
        {
            MessageBoxW(owner, L"总增益范围为 -30 dB 到 +30 dB。不会自动衰减、压限或回收音量。",
                L"HXAudio PEQ", MB_OK | MB_ICONWARNING);
            SetFocus(g_preampEdit);
            return false;
        }

        WriteRegistryString(L"DspPreampDb", FormatParameter(preampDb));
        if (g_peqWindow && g_peqEditorDirty)
        {
            if (!SavePeqBandsForDevice(g_peqEditorDeviceKey, g_peqEditorBands)) return false;
            g_peqEditorDirty = false;
        }
        else if (!RegistryValueIsType(L"DspPeqBands", REG_MULTI_SZ))
        {
            if (!SavePeqBands(LoadPeqBands())) return false;
        }
        return true;
    }

    void SignalDspSettingsChanged()
    {
        if (g_apoMode)
            ApplyStaticHardwareCompensation(
                StaticGainApplyMode::ReductionsOnly);
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kDspSettingsEvent);
        if (event)
        {
            SetEvent(event);
            CloseHandle(event);
        }
        const DWORD generation = NotifyApoDspSettingsChanged();
        if (g_apoMode)
        {
            WaitForApoSettingsGeneration(generation, 400);
            ApplyStaticHardwareCompensation(StaticGainApplyMode::All);
        }
    }

    void ApplyDefaultFont(HWND parent);

    RECT VirtualBassGraphRect(HWND window)
    {
        RECT client{};
        GetClientRect(window, &client);
        return { UiScale(window, 48), UiScale(window, 86),
            std::max<LONG>(client.right - UiScale(window, 32), UiScale(window, 49)),
            UiScale(window, 382) };
    }

    void UpdateVirtualBassCustomControls()
    {
        const bool custom = g_virtualBassMode &&
            SendMessageW(g_virtualBassMode, CB_GETCURSEL, 0, 0) == 2;
        if (g_virtualBassCustomLabel)
            ShowWindow(g_virtualBassCustomLabel, custom ? SW_SHOW : SW_HIDE);
        for (HWND edit : g_virtualBassCustom)
        {
            if (!edit) continue;
            EnableWindow(edit, custom);
            ShowWindow(edit, custom ? SW_SHOW : SW_HIDE);
        }
    }

    double VirtualBassGraphMaximumHz()
    {
        double cutoff = 80.0;
        TryParseFiniteDouble(ReadWindowText(g_virtualBassCutoff), cutoff);
        const LRESULT mode = g_virtualBassMode
            ? SendMessageW(g_virtualBassMode, CB_GETCURSEL, 0, 0) : 0;
        double highest = mode == 1 ? 7.0 : 4.0;
        if (mode == 2)
        {
            highest = 1.0;
            for (HWND edit : g_virtualBassCustom)
            {
                double multiplier = 0.0;
                if (TryParseFiniteDouble(ReadWindowText(edit), multiplier) &&
                    ValidVirtualBassMultiplier(multiplier))
                    highest = std::max(highest, multiplier);
            }
        }
        return std::max(1000.0, std::min(cutoff * highest * 1.35, 1.0e9));
    }

    int VirtualBassFrequencyToX(double frequency, const RECT& graph)
    {
        const double minimum = 5.0;
        const double maximum = VirtualBassGraphMaximumHz();
        const double width = std::max<LONG>(graph.right - graph.left, 1);
        if (!std::isfinite(frequency) || frequency <= minimum) return graph.left;
        if (frequency >= maximum) return graph.right;
        const double position = (std::log(frequency) - std::log(minimum)) /
            (std::log(maximum) - std::log(minimum));
        return graph.left + static_cast<int>(position * width);
    }

    double VirtualBassXToFrequency(int x, const RECT& graph)
    {
        const double position = std::clamp(
            static_cast<double>(x - graph.left) /
            std::max<LONG>(graph.right - graph.left, 1), 0.0, 1.0);
        return std::exp(std::log(5.0) + position *
            (std::log(VirtualBassGraphMaximumHz()) - std::log(5.0)));
    }

    bool ReadVirtualBassEditorSettings(DspSettings& settings, HWND owner, bool showError)
    {
        settings = LoadDspSettings();
        settings.virtualBassEnabled = g_virtualBassEnabled &&
            SendMessageW(g_virtualBassEnabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
        settings.virtualBassAmount = SliderPosition(g_virtualBassAmount) / 100.0;
        const LRESULT mode = SendMessageW(g_virtualBassMode, CB_GETCURSEL, 0, 0);
        settings.virtualBassMode = mode == 1 ? VirtualBassMode::Harmonics357 :
            (mode == 2 ? VirtualBassMode::Custom : VirtualBassMode::Harmonics234);

        if (settings.virtualBassMode == VirtualBassMode::Custom)
        {
            for (size_t index = 0; index < settings.virtualBassMultipliers.size(); ++index)
            {
                double multiplier = 0.0;
                if (!TryParseFiniteDouble(ReadWindowText(g_virtualBassCustom[index]),
                    multiplier) || !ValidVirtualBassMultiplier(multiplier))
                {
                    if (showError) MessageBoxW(owner,
                        L"三个自定义倍频都必须是大于 0、可表示的有限数字，允许输入小数。",
                        L"HXAudio 虚拟低频", MB_OK | MB_ICONWARNING);
                    return false;
                }
                settings.virtualBassMultipliers[index] = multiplier;
            }
        }

        if (!TryParseFiniteDouble(ReadWindowText(g_virtualBassCutoff), settings.virtualBassCutoffHz) ||
            settings.virtualBassCutoffHz <= 0.0)
        {
            if (showError) MessageBoxW(owner, L"低频替换截止必须是大于 0 的有限频率。",
                L"HXAudio 虚拟低频", MB_OK | MB_ICONWARNING);
            return false;
        }
        if (!TryParseFiniteDouble(ReadWindowText(g_virtualBassSubsonic), settings.virtualBassSubsonicHz) ||
            settings.virtualBassSubsonicHz < 0.0 ||
            settings.virtualBassSubsonicHz >= settings.virtualBassCutoffHz)
        {
            if (showError) MessageBoxW(owner,
                L"超低频切除必须大于等于 0，并且低于替换截止频率；输入 0 可关闭超低切除。",
                L"HXAudio 虚拟低频", MB_OK | MB_ICONWARNING);
            return false;
        }
        return true;
    }

    void SetVirtualBassStatus(const std::wstring& text)
    {
        SetWindowTextIfChanged(g_virtualBassStatus, text);
    }

    bool CommitVirtualBassEditor(HWND owner, bool showError)
    {
        DspSettings settings;
        if (!ReadVirtualBassEditorSettings(settings, owner, showError))
        {
            SetVirtualBassStatus(L"参数尚未应用：请检查频率或倍频输入。");
            return false;
        }
        WriteRegistryDword(L"DspVirtualBassEnabled", settings.virtualBassEnabled ? 1 : 0);
        WriteRegistryDword(L"DspVirtualBassAmount",
            static_cast<DWORD>(std::lround(settings.virtualBassAmount * 100.0)));
        WriteRegistryString(L"DspVirtualBassCutoffHzText", FormatParameter(settings.virtualBassCutoffHz));
        WriteRegistryString(L"DspVirtualBassSubsonicHzText", FormatParameter(settings.virtualBassSubsonicHz));
        WriteRegistryDword(L"DspVirtualBassCutoffHz",
            static_cast<DWORD>(std::clamp(settings.virtualBassCutoffHz, 1.0,
                static_cast<double>(MAXDWORD))));
        WriteRegistryDword(L"DspVirtualBassSubsonicHz",
            static_cast<DWORD>(std::clamp(settings.virtualBassSubsonicHz, 0.0,
                static_cast<double>(MAXDWORD))));
        WriteRegistryDword(L"DspVirtualBassMode",
            static_cast<DWORD>(settings.virtualBassMode));
        WriteRegistryString(L"DspVirtualBassMultiplier1",
            FormatParameter(settings.virtualBassMultipliers[0]));
        WriteRegistryString(L"DspVirtualBassMultiplier2",
            FormatParameter(settings.virtualBassMultipliers[1]));
        WriteRegistryString(L"DspVirtualBassMultiplier3",
            FormatParameter(settings.virtualBassMultipliers[2]));
        SignalDspSettingsChanged();
        UpdateVirtualBassSummary();
        g_virtualBassDirty = false;
        SetVirtualBassStatus(RouterIsReady()
            ? L"已实时应用：原低频已切除并由所选谐波替换。"
            : L"设置已保存；启动 HXAudio 后生效。");
        return true;
    }

    void VirtualBassEditorChanged(HWND window, bool allowLiveApply)
    {
        if (g_virtualBassUpdating) return;
        g_virtualBassDirty = true;
        if (g_virtualBassAmountValue)
        {
            const std::wstring amount = std::to_wstring(SliderPosition(g_virtualBassAmount)) + L" %";
            SetWindowTextIfChanged(g_virtualBassAmountValue, amount);
        }
        const RECT graph = VirtualBassGraphRect(window);
        InvalidateRect(window, &graph, FALSE);
        if (allowLiveApply && g_virtualBassLiveApply &&
            SendMessageW(g_virtualBassLiveApply, BM_GETCHECK, 0, 0) == BST_CHECKED)
        {
            const ULONGLONG now = GetTickCount64();
            if (now - g_virtualBassLastLiveCommit >= 30)
            {
                if (CommitVirtualBassEditor(window, false)) g_virtualBassLastLiveCommit = now;
            }
        }
    }

    void ReloadVirtualBassEditor()
    {
        if (!g_virtualBassWindow) return;
        const DspSettings settings = LoadDspSettings();
        g_virtualBassUpdating = true;
        SendMessageW(g_virtualBassEnabled, BM_SETCHECK,
            settings.virtualBassEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_virtualBassMode, CB_SETCURSEL,
            static_cast<WPARAM>(settings.virtualBassMode), 0);
        SendMessageW(g_virtualBassAmount, TBM_SETPOS, TRUE,
            static_cast<LPARAM>(std::clamp(std::lround(settings.virtualBassAmount * 100.0), 0L, 200L)));
        SetWindowTextW(g_virtualBassAmountValue,
            (std::to_wstring(SliderPosition(g_virtualBassAmount)) + L" %").c_str());
        SetWindowTextW(g_virtualBassCutoff, FormatParameter(settings.virtualBassCutoffHz).c_str());
        SetWindowTextW(g_virtualBassSubsonic, FormatParameter(settings.virtualBassSubsonicHz).c_str());
        for (size_t index = 0; index < settings.virtualBassMultipliers.size(); ++index)
            SetWindowTextW(g_virtualBassCustom[index],
                FormatParameter(settings.virtualBassMultipliers[index]).c_str());
        UpdateVirtualBassCustomControls();
        g_virtualBassUpdating = false;
        g_virtualBassDirty = false;
        InvalidateRect(g_virtualBassWindow, nullptr, FALSE);
    }

    void PaintVirtualBassEditor(HWND window, HDC suppliedDestination = nullptr)
    {
        PAINTSTRUCT paint{};
        HDC destination = suppliedDestination
            ? suppliedDestination : BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const int width = std::max<LONG>(client.right, 1);
        const int height = std::max<LONG>(client.bottom, 1);
        UiSurfaceCache* cache = EnsureUiSurfaceCache(window, destination, width, height);
        if (!cache)
        {
            if (!suppliedDestination) EndPaint(window, &paint);
            return;
        }
        HDC dc = cache->canvas;
        void* pixels = cache->pixels;
        RECT update{};
        if (!UiPaintRegion(destination, client, cache->valid, update))
        {
            if (!suppliedDestination) EndPaint(window, &paint);
            return;
        }
        const int savedDc = SaveDC(dc);
        IntersectClipRect(dc, update.left, update.top, update.right, update.bottom);
        PrepareUiCanvasRegion(pixels, width, height, dc, update);

        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const float scale = static_cast<float>(UiDpi(window)) / 96.0f;
        const auto toRect = [&](const RECT& rectangle)
        {
            return Gdiplus::RectF(static_cast<float>(rectangle.left),
                static_cast<float>(rectangle.top),
                static_cast<float>(rectangle.right - rectangle.left),
                static_cast<float>(rectangle.bottom - rectangle.top));
        };
        RECT graph = VirtualBassGraphRect(window);
        Gdiplus::RectF graphBounds = toRect(graph);
        Gdiplus::RectF shadow = graphBounds;
        shadow.Y += 2.0f * scale;
        FillRounded(graphics, shadow, 12.0f * scale,
            Gdiplus::Color(g_ui.dark ? 52 : 18, 0, 0, 0));
        FillRounded(graphics, graphBounds, 12.0f * scale, GdiColor(g_ui.graph));
        StrokeRounded(graphics, graphBounds, 12.0f * scale, GdiColor(g_ui.border));
        Gdiplus::RectF controlCard(static_cast<float>(UiScale(window, 24)),
            static_cast<float>(UiScale(window, 402)),
            static_cast<float>(std::max<LONG>(1, client.right - UiScale(window, 48))),
            static_cast<float>(std::max<LONG>(1, client.bottom - UiScale(window, 422))));
        FillRounded(graphics, controlCard, 12.0f * scale, GdiColor(g_ui.surface));
        StrokeRounded(graphics, controlCard, 12.0f * scale, GdiColor(g_ui.border));
        DrawUiText(graphics, dc, window, L"虚拟低频谐波映射",
            Gdiplus::RectF(static_cast<float>(UiScale(window, 28)),
                static_cast<float>(UiScale(window, 14)),
                static_cast<float>(UiScale(window, 500)),
                static_cast<float>(UiScale(window, 38))),
            UiFontRole::Section, g_ui.text);
        DrawUiText(graphics, dc, window,
            L"将不可闻的超低频替换为扬声器更容易重放的自然谐波",
            Gdiplus::RectF(static_cast<float>(UiScale(window, 28)),
                static_cast<float>(UiScale(window, 46)),
                static_cast<float>(UiScale(window, 680)),
                static_cast<float>(UiScale(window, 26))),
            UiFontRole::Caption, g_ui.muted);

        const double grid[] = { 10, 20, 30, 40, 60, 80, 100, 150, 200, 300, 500, 1000, 2000, 5000 };
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_ui.muted);
        HGDIOBJ previousFont = SelectObject(dc, UiFont(UiDpi(window), UiFontRole::Caption));
        for (double frequency : grid)
        {
            if (frequency > VirtualBassGraphMaximumHz()) continue;
            const int x = VirtualBassFrequencyToX(frequency, graph);
            Gdiplus::Pen gridPen(GdiColor(g_ui.grid, 150), 1.0f);
            graphics.DrawLine(&gridPen, static_cast<float>(x), static_cast<float>(graph.top + UiScale(window, 8)),
                static_cast<float>(x), static_cast<float>(graph.bottom - UiScale(window, 8)));
            const std::wstring label = FormatParameter(frequency);
            RECT labelRect{ x - UiScale(window, 25), graph.bottom - UiScale(window, 23),
                x + UiScale(window, 25), graph.bottom - UiScale(window, 4) };
            DrawTextW(dc, label.c_str(), -1, &labelRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }

        DspSettings settings;
        if (!ReadVirtualBassEditorSettings(settings, window, false)) settings = LoadDspSettings();
        const double sourceLow = settings.virtualBassSubsonicHz > 0.0
            ? settings.virtualBassSubsonicHz : 5.0;
        const double sourceHigh = settings.virtualBassCutoffHz;
        const std::array<double, 3> multipliers =
            EffectiveVirtualBassMultipliers(settings);

        const int laneHeight = UiScale(window, 54);
        RECT sourceLane{ graph.left, graph.top + UiScale(window, 22),
            graph.right, graph.top + UiScale(window, 22) + laneHeight };
        const int sourceLeft = VirtualBassFrequencyToX(sourceLow, graph);
        const int sourceRight = VirtualBassFrequencyToX(sourceHigh, graph);
        RECT sourceBand{ sourceLeft, sourceLane.top + UiScale(window, 14),
            sourceRight, sourceLane.bottom - UiScale(window, 6) };
        FillRounded(graphics, toRect(sourceBand), 7.0f * scale, GdiColor(g_ui.danger, 145));
        SetTextColor(dc, g_ui.text);
        RECT sourceText{ graph.left + UiScale(window, 12), sourceLane.top - UiScale(window, 8),
            graph.right, sourceLane.top + UiScale(window, 15) };
        DrawTextW(dc, L"被提取并替换的原始低频", -1, &sourceText, DT_LEFT | DT_SINGLELINE);

        for (int lane = 0; lane < 3; ++lane)
        {
            const double multiplier = multipliers[lane];
            RECT harmonicLane{ graph.left, graph.top + UiScale(window, 86) + lane * laneHeight,
                graph.right, graph.top + UiScale(window, 86) + (lane + 1) * laneHeight };
            RECT harmonicBand{
                VirtualBassFrequencyToX(sourceLow * multiplier, graph), harmonicLane.top + UiScale(window, 14),
                VirtualBassFrequencyToX(sourceHigh * multiplier, graph), harmonicLane.bottom - UiScale(window, 6) };
            const COLORREF laneColor = lane == 0 ? g_ui.accent :
                (lane == 1 ? g_ui.success : RGB(154, 103, 201));
            FillRounded(graphics, toRect(harmonicBand), 7.0f * scale,
                GdiColor(laneColor, g_ui.dark ? 180 : 118));
            const std::wstring label = FormatParameter(multiplier) + L" 倍频";
            RECT laneText{ graph.left + UiScale(window, 12), harmonicLane.top - UiScale(window, 8),
                graph.right, harmonicLane.top + UiScale(window, 15) };
            DrawTextW(dc, label.c_str(), -1, &laneText, DT_LEFT | DT_SINGLELINE);
        }

        const int cutoffX = VirtualBassFrequencyToX(settings.virtualBassCutoffHz, graph);
        Gdiplus::Pen cutoffPen(GdiColor(g_ui.danger), g_virtualBassDraggingCutoff ? 4.0f : 2.6f);
        graphics.DrawLine(&cutoffPen, static_cast<float>(cutoffX), static_cast<float>(graph.top + 4),
            static_cast<float>(cutoffX), static_cast<float>(graph.bottom - 4));
        Gdiplus::SolidBrush cutoffHandle(GdiColor(g_ui.danger));
        graphics.FillEllipse(&cutoffHandle, static_cast<float>(cutoffX - UiScale(window, 5)),
            static_cast<float>(graph.top + UiScale(window, 8)),
            static_cast<float>(UiScale(window, 10)), static_cast<float>(UiScale(window, 10)));
        if (settings.virtualBassSubsonicHz > 0.0)
        {
            const int subsonicX = VirtualBassFrequencyToX(settings.virtualBassSubsonicHz, graph);
            Gdiplus::Pen subsonicPen(GdiColor(g_ui.accent),
                g_virtualBassDraggingSubsonic ? 4.0f : 2.6f);
            subsonicPen.SetDashStyle(Gdiplus::DashStyleDash);
            graphics.DrawLine(&subsonicPen, static_cast<float>(subsonicX), static_cast<float>(graph.top + 4),
                static_cast<float>(subsonicX), static_cast<float>(graph.bottom - 4));
            Gdiplus::SolidBrush subsonicHandle(GdiColor(g_ui.accent));
            graphics.FillEllipse(&subsonicHandle, static_cast<float>(subsonicX - UiScale(window, 5)),
                static_cast<float>(graph.top + UiScale(window, 8)),
                static_cast<float>(UiScale(window, 10)), static_cast<float>(UiScale(window, 10)));
        }
        SelectObject(dc, previousFont);
        graphics.Flush(Gdiplus::FlushIntentionSync);
        FinalizeUiCanvasAlphaRegion(pixels, width, height, update);
        cache->valid = true;
        RestoreDC(dc, savedDc);
        BitBlt(destination, update.left, update.top,
            update.right - update.left, update.bottom - update.top,
            dc, update.left, update.top, SRCCOPY);
        if (!suppliedDestination) EndPaint(window, &paint);
    }

    LRESULT CALLBACK VirtualBassEditorProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
        {
            // CreateWindowEx does not assign the returned HWND to the global
            // until WM_CREATE has finished.  Make reload/selection logic valid
            // during creation as well.
            g_virtualBassWindow = window;
            ConfigureModernWindow(window);
            HWND title = CreateWindowW(L"STATIC", L"虚拟低频谐波映射",
                WS_CHILD | WS_VISIBLE, 28, 20, 380, 28, window, nullptr, nullptr, nullptr);
            HWND subtitle = CreateWindowW(L"STATIC", L"将不可闻的超低频替换为扬声器更容易重放的自然谐波",
                WS_CHILD | WS_VISIBLE, 28, 51, 650, 22, window, nullptr, nullptr, nullptr);
            g_virtualBassEnabled = CreateWindowW(L"BUTTON", L"启用低频替换",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                44, 420, 154, 28, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVirtualBassEnabledId)), nullptr, nullptr);
            SetPropW(g_virtualBassEnabled, kUiToggleSwitchProperty,
                reinterpret_cast<HANDLE>(1));
            CreateWindowW(L"STATIC", L"谐波模式", WS_CHILD | WS_VISIBLE,
                226, 424, 82, 22, window, nullptr, nullptr, nullptr);
            g_virtualBassMode = CreateWindowW(WC_COMBOBOXW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                314, 416, 170, 180, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVirtualBassModeId)), nullptr, nullptr);
            SendMessageW(g_virtualBassMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"2 / 3 / 4 次"));
            SendMessageW(g_virtualBassMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"3 / 5 / 7 次"));
            SendMessageW(g_virtualBassMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"自定义倍频"));
            g_virtualBassCustomLabel = CreateWindowW(L"STATIC", L"三个倍频",
                WS_CHILD, 494, 424, 82, 22, window, nullptr, nullptr, nullptr);
            constexpr std::array<int, 3> customIds{
                kVirtualBassCustom1Id, kVirtualBassCustom2Id,
                kVirtualBassCustom3Id };
            constexpr std::array<int, 3> customX{ 578, 640, 702 };
            for (size_t index = 0; index < g_virtualBassCustom.size(); ++index)
            {
                g_virtualBassCustom[index] = CreateWindowExW(WS_EX_CLIENTEDGE,
                    L"EDIT", index == 0 ? L"2" : (index == 1 ? L"3" : L"4"),
                    WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
                    customX[index], 416, 54, 30, window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(customIds[index])),
                    nullptr, nullptr);
            }

            CreateWindowW(L"STATIC", L"谐波强度", WS_CHILD | WS_VISIBLE,
                44, 468, 90, 22, window, nullptr, nullptr, nullptr);
            g_virtualBassAmount = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS,
                138, 456, 510, 42, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVirtualBassAmountId)), nullptr, nullptr);
            SendMessageW(g_virtualBassAmount, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
            SendMessageW(g_virtualBassAmount, TBM_SETTICFREQ, 20, 0);
            g_virtualBassAmountValue = CreateWindowW(L"STATIC", L"0 %", WS_CHILD | WS_VISIBLE,
                666, 468, 92, 22, window, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC", L"替换截止", WS_CHILD | WS_VISIBLE,
                44, 522, 90, 22, window, nullptr, nullptr, nullptr);
            g_virtualBassCutoff = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"80", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                138, 514, 132, 30, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVirtualBassCutoffId)), nullptr, nullptr);
            CreateWindowW(L"STATIC", L"Hz（其下原频率被切掉并替换）", WS_CHILD | WS_VISIBLE,
                282, 522, 310, 22, window, nullptr, nullptr, nullptr);
            CreateWindowW(L"STATIC", L"超低切除", WS_CHILD | WS_VISIBLE,
                44, 562, 90, 22, window, nullptr, nullptr, nullptr);
            g_virtualBassSubsonic = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"18", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                138, 554, 132, 30, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVirtualBassSubsonicId)), nullptr, nullptr);
            CreateWindowW(L"STATIC", L"Hz（0 = 关闭，不参与谐波生成）", WS_CHILD | WS_VISIBLE,
                282, 562, 300, 22, window, nullptr, nullptr, nullptr);

            g_virtualBassLiveApply = CreateWindowW(L"BUTTON", L"实时应用", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                44, 612, 132, 28, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVirtualBassLiveApplyId)), nullptr, nullptr);
            SetPropW(g_virtualBassLiveApply, kUiToggleSwitchProperty,
                reinterpret_cast<HANDLE>(1));
            SendMessageW(g_virtualBassLiveApply, BM_SETCHECK, BST_CHECKED, 0);
            CreateWindowW(L"BUTTON", L"应用", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                520, 606, 110, 36, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVirtualBassApplyId)), nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"恢复推荐值", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                642, 606, 134, 36, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVirtualBassResetId)), nullptr, nullptr);
            g_virtualBassStatus = CreateWindowW(L"STATIC", L"拖动红/蓝频率线，或输入精确频率。", WS_CHILD | WS_VISIBLE,
                44, 654, 720, 24, window, nullptr, nullptr, nullptr);
            ScaleChildrenForDpi(window, 96, UiDpi(window));
            ApplyDefaultFont(window);
            SetUiFontRole(title, UiFontRole::Section);
            SetUiFontRole(subtitle, UiFontRole::Caption);
            ShowWindow(title, SW_HIDE);
            ShowWindow(subtitle, SW_HIDE);
            ReloadVirtualBassEditor();
            return 0;
        }
        case WM_PAINT:
            PaintVirtualBassEditor(window);
            return 0;
        case WM_PRINTCLIENT:
            PaintVirtualBassEditor(window, reinterpret_cast<HDC>(wParam));
            return 0;
        case kApplyInitialDpiSizeMessage:
            SizeWindowForCurrentDpi(window, 820, 735);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DRAWITEM:
            if (DrawModernButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam)) ||
                DrawModernChoiceItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam))) return TRUE;
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
            return ControlColor(message, wParam, lParam);
        case WM_DPICHANGED:
            ApplyDpiChange(window, wParam, lParam);
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            RefreshUiPalette();
            ConfigureModernWindow(window);
            ModernizeChildren(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
        {
            DspSettings settings;
            if (!ReadVirtualBassEditorSettings(settings, window, false)) settings = LoadDspSettings();
            const RECT graph = VirtualBassGraphRect(window);
            const int x = static_cast<short>(LOWORD(lParam));
            const int y = static_cast<short>(HIWORD(lParam));
            if (y >= graph.top && y <= graph.bottom)
            {
                const int hitRadius = UiScale(window, 12);
                g_virtualBassDraggingCutoff = std::abs(x - VirtualBassFrequencyToX(
                    settings.virtualBassCutoffHz, graph)) <= hitRadius;
                g_virtualBassDraggingSubsonic = settings.virtualBassSubsonicHz > 0.0 &&
                    std::abs(x - VirtualBassFrequencyToX(settings.virtualBassSubsonicHz, graph)) <= hitRadius;
                if (g_virtualBassDraggingCutoff || g_virtualBassDraggingSubsonic) SetCapture(window);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            const POINT pointer{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            const RECT pointerGraph = VirtualBassGraphRect(window);
            const bool inside = PtInRect(&pointerGraph, pointer) != FALSE;
            if (inside != g_virtualBassPointerInGraph)
            {
                g_virtualBassPointerInGraph = inside;
                InvalidateRect(window, &pointerGraph, FALSE);
            }
            if (inside)
            {
                TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, window, 0 };
                TrackMouseEvent(&tracking);
            }
            if ((wParam & MK_LBUTTON) && (g_virtualBassDraggingCutoff || g_virtualBassDraggingSubsonic))
            {
                const RECT graph = VirtualBassGraphRect(window);
                double frequency = VirtualBassXToFrequency(static_cast<short>(LOWORD(lParam)), graph);
                DspSettings settings;
                if (!ReadVirtualBassEditorSettings(settings, window, false)) settings = LoadDspSettings();
                if (g_virtualBassDraggingSubsonic)
                    frequency = std::min(frequency, settings.virtualBassCutoffHz * 0.95);
                else
                    frequency = std::max(frequency, std::max(5.0, settings.virtualBassSubsonicHz * 1.05));
                SetWindowTextIfChanged(
                    g_virtualBassDraggingSubsonic
                        ? g_virtualBassSubsonic : g_virtualBassCutoff,
                    FormatParameter(frequency));
                VirtualBassEditorChanged(window, true);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_virtualBassPointerInGraph = false;
        {
            const RECT graph = VirtualBassGraphRect(window);
            InvalidateRect(window, &graph, FALSE);
        }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT)
            {
                POINT pointer{};
                GetCursorPos(&pointer);
                ScreenToClient(window, &pointer);
                DspSettings settings;
                if (!ReadVirtualBassEditorSettings(settings, window, false)) settings = LoadDspSettings();
                const RECT graph = VirtualBassGraphRect(window);
                const int hitRadius = UiScale(window, 12);
                if (PtInRect(&graph, pointer) &&
                    (std::abs(pointer.x - VirtualBassFrequencyToX(settings.virtualBassCutoffHz, graph)) <= hitRadius ||
                    (settings.virtualBassSubsonicHz > 0.0 && std::abs(pointer.x -
                        VirtualBassFrequencyToX(settings.virtualBassSubsonicHz, graph)) <= hitRadius)))
                {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (GetCapture() == window) ReleaseCapture();
            g_virtualBassDraggingCutoff = false;
            g_virtualBassDraggingSubsonic = false;
            return 0;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lParam) == g_virtualBassAmount)
                VirtualBassEditorChanged(window, true);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case kVirtualBassEnabledId:
                if (HIWORD(wParam) == BN_CLICKED) VirtualBassEditorChanged(window, true);
                break;
            case kVirtualBassModeId:
                if (HIWORD(wParam) == CBN_SELCHANGE)
                {
                    UpdateVirtualBassCustomControls();
                    VirtualBassEditorChanged(window, true);
                }
                break;
            case kVirtualBassCutoffId:
            case kVirtualBassSubsonicId:
            case kVirtualBassCustom1Id:
            case kVirtualBassCustom2Id:
            case kVirtualBassCustom3Id:
                if (HIWORD(wParam) == EN_CHANGE) VirtualBassEditorChanged(window, true);
                break;
            case kVirtualBassApplyId:
                CommitVirtualBassEditor(window, true);
                break;
            case kVirtualBassResetId:
                g_virtualBassUpdating = true;
                SendMessageW(g_virtualBassEnabled, BM_SETCHECK, BST_CHECKED, 0);
                SendMessageW(g_virtualBassMode, CB_SETCURSEL, 0, 0);
                SendMessageW(g_virtualBassAmount, TBM_SETPOS, TRUE, 100);
                SetWindowTextW(g_virtualBassAmountValue, L"100 %");
                SetWindowTextW(g_virtualBassCutoff, L"80");
                SetWindowTextW(g_virtualBassSubsonic, L"18");
                SetWindowTextW(g_virtualBassCustom[0], L"2");
                SetWindowTextW(g_virtualBassCustom[1], L"3");
                SetWindowTextW(g_virtualBassCustom[2], L"4");
                UpdateVirtualBassCustomControls();
                g_virtualBassUpdating = false;
                g_virtualBassDirty = true;
                {
                    const RECT graph = VirtualBassGraphRect(window);
                    InvalidateRect(window, &graph, FALSE);
                }
                CommitVirtualBassEditor(window, false);
                break;
            }
            return 0;
        case WM_CLOSE:
            if (g_virtualBassDirty)
            {
                const int answer = MessageBoxW(window, L"虚拟低频还有未应用修改。关闭前保存并实时应用吗？",
                    L"HXAudio 虚拟低频", MB_YESNOCANCEL | MB_ICONQUESTION);
                if (answer == IDCANCEL) return 0;
                if (answer == IDYES && !CommitVirtualBassEditor(window, true)) return 0;
            }
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            DestroyUiSurfaceCache(window);
            g_virtualBassWindow = nullptr;
            g_virtualBassEnabled = nullptr;
            g_virtualBassMode = nullptr;
            g_virtualBassAmount = nullptr;
            g_virtualBassAmountValue = nullptr;
            g_virtualBassCutoff = nullptr;
            g_virtualBassSubsonic = nullptr;
            g_virtualBassCustomLabel = nullptr;
            g_virtualBassCustom = {};
            g_virtualBassLiveApply = nullptr;
            g_virtualBassStatus = nullptr;
            g_virtualBassDirty = false;
            g_virtualBassPointerInGraph = false;
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void OpenVirtualBassEditor(HWND owner)
    {
        if (g_virtualBassWindow)
        {
            ShowWindow(g_virtualBassWindow, SW_RESTORE);
            SetForegroundWindow(g_virtualBassWindow);
            return;
        }
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        constexpr wchar_t className[] = L"HXAudioVirtualBassEditor";
        WNDCLASSEXW existing{ sizeof(existing) };
        if (!GetClassInfoExW(instance, className, &existing))
        {
            WNDCLASSEXW cls{ sizeof(cls) };
            cls.style = CS_DBLCLKS;
            cls.lpfnWndProc = VirtualBassEditorProc;
            cls.hInstance = instance;
            cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            cls.hIcon = g_uiIcons.empty() ? LoadIconW(nullptr, IDI_APPLICATION) : g_uiIcons.front();
            cls.hIconSm = g_uiIcons.size() > 1 ? g_uiIcons[1] : cls.hIcon;
            cls.hbrBackground = nullptr;
            cls.lpszClassName = className;
            RegisterClassExW(&cls);
        }
        const UINT dpi = owner ? UiDpi(owner) : GetDpiForSystem();
        g_virtualBassWindow = CreateWindowExW(WS_EX_DLGMODALFRAME, className,
            L"HXAudio 虚拟低频替换器", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
            WS_MINIMIZEBOX | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(820, dpi, 96), MulDiv(735, dpi, 96),
            owner, nullptr, instance, nullptr);
        if (g_virtualBassWindow)
        {
            SizeWindowForCurrentDpi(g_virtualBassWindow, 820, 735);
            ShowWindow(g_virtualBassWindow, SW_SHOW);
            // A hidden owner can initially place the editor in a 96-DPI
            // context. Size again after showing, when the target monitor DPI
            // is authoritative.
            SizeWindowForCurrentDpi(g_virtualBassWindow, 820, 735);
            PostMessageW(g_virtualBassWindow, kApplyInitialDpiSizeMessage, 0, 0);
            UpdateWindow(g_virtualBassWindow);
        }
    }

    bool PeqSelectionIsValid()
    {
        return g_peqSelectedBand >= 0 &&
            static_cast<size_t>(g_peqSelectedBand) < g_peqEditorBands.size();
    }

    RECT PeqGraphRect(HWND window)
    {
        RECT client{};
        GetClientRect(window, &client);
        RECT graph{ UiScale(window, 74), UiScale(window, 86),
            std::max<LONG>(client.right - UiScale(window, 28), UiScale(window, 75)),
            std::max<LONG>(client.bottom - UiScale(window, 226), UiScale(window, 87)) };
        return graph;
    }

    RECT PeqGainZoomOutRect(HWND window)
    {
        const RECT graph = PeqGraphRect(window);
        const int width = UiScale(window, 36);
        const int height = UiScale(window, 34);
        const int margin = UiScale(window, 10);
        return { graph.left + margin, graph.top + margin,
            graph.left + margin + width, graph.top + margin + height };
    }

    RECT PeqGainZoomInRect(HWND window)
    {
        RECT result = PeqGainZoomOutRect(window);
        const int offset = (result.right - result.left) + UiScale(window, 6);
        OffsetRect(&result, offset, 0);
        return result;
    }

    RECT PeqGainZoomDirtyRect(HWND window)
    {
        RECT dirty = PeqGraphRect(window);
        dirty.left = 0;
        return dirty;
    }

    double PeqSafeLog(double value)
    {
        return std::log(std::max(value, std::numeric_limits<double>::min()));
    }

    int PeqFrequencyToX(double frequency, const RECT& graph)
    {
        const double denominator = PeqSafeLog(g_peqViewMaxHz) - PeqSafeLog(g_peqViewMinHz);
        if (!std::isfinite(frequency) || frequency <= 0.0 || denominator <= 0.0) return graph.left;
        const double position = (PeqSafeLog(frequency) - PeqSafeLog(g_peqViewMinHz)) / denominator;
        const double x = graph.left + position * std::max<LONG>(graph.right - graph.left, 1);
        return static_cast<int>(std::clamp(x,
            static_cast<double>(std::numeric_limits<int>::min()),
            static_cast<double>(std::numeric_limits<int>::max())));
    }

    double PeqXToFrequency(int x, const RECT& graph)
    {
        const double width = std::max<LONG>(graph.right - graph.left, 1);
        const double position = (static_cast<double>(x) - graph.left) / width;
        const double logarithm = PeqSafeLog(g_peqViewMinHz) + position *
            (PeqSafeLog(g_peqViewMaxHz) - PeqSafeLog(g_peqViewMinHz));
        const double maximumLog = std::log(std::numeric_limits<double>::max());
        const double minimumLog = std::log(std::numeric_limits<double>::min());
        if (logarithm >= maximumLog) return std::numeric_limits<double>::max();
        if (logarithm <= minimumLog) return std::numeric_limits<double>::min();
        return std::exp(logarithm);
    }

    int PeqGainToY(double gainDb, const RECT& graph)
    {
        const double height = std::max<LONG>(graph.bottom - graph.top, 1);
        const double y = graph.top + (g_peqViewDbRange - gainDb) * height /
            (2.0 * g_peqViewDbRange);
        return static_cast<int>(std::clamp(y,
            static_cast<double>(std::numeric_limits<int>::min()),
            static_cast<double>(std::numeric_limits<int>::max())));
    }

    double PeqYToGain(int y, const RECT& graph)
    {
        const double height = std::max<LONG>(graph.bottom - graph.top, 1);
        return g_peqViewDbRange - (static_cast<double>(y) - graph.top) *
            (2.0 * g_peqViewDbRange) / height;
    }

    std::wstring PeqAxisFrequencyText(double frequency)
    {
        wchar_t text[40]{};
        if (frequency >= 1.0e9 || frequency < 0.01)
            swprintf_s(text, L"%.0e", frequency);
        else if (frequency >= 1000000.0)
            swprintf_s(text, L"%.0fM", frequency / 1000000.0);
        else if (frequency >= 1000.0)
            swprintf_s(text, L"%.0fk", frequency / 1000.0);
        else if (frequency >= 10.0)
            swprintf_s(text, L"%.0f", frequency);
        else
            swprintf_s(text, L"%.2g", frequency);
        return text;
    }

    std::wstring PeqGuiParameterText(double value)
    {
        std::wostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(8) << value;
        return stream.str();
    }

    double PeqNiceGridStep(double roughStep)
    {
        if (!std::isfinite(roughStep) || roughStep <= 0.0) return 1.0;
        const double exponent = std::floor(std::log10(roughStep));
        const double base = std::pow(10.0, exponent);
        const double fraction = roughStep / base;
        const double nice = fraction <= 1.0 ? 1.0 : fraction <= 2.0 ? 2.0 :
            fraction <= 5.0 ? 5.0 : 10.0;
        return nice * base;
    }

    double BiquadMagnitudeDb(const BiquadCoefficients& coefficients, double frequency)
    {
        constexpr double pi = 3.14159265358979323846;
        const double omega = 2.0 * pi * frequency / 48000.0;
        const std::complex<double> z1(std::cos(omega), -std::sin(omega));
        const std::complex<double> z2 = z1 * z1;
        const std::complex<double> numerator = coefficients.b0 + coefficients.b1 * z1 + coefficients.b2 * z2;
        const std::complex<double> denominator = 1.0 + coefficients.a1 * z1 + coefficients.a2 * z2;
        const double denominatorMagnitude = std::abs(denominator);
        if (!std::isfinite(denominatorMagnitude) || denominatorMagnitude <= 0.0) return 600.0;
        const double magnitude = std::abs(numerator) / denominatorMagnitude;
        if (!std::isfinite(magnitude)) return 600.0;
        if (magnitude <= std::numeric_limits<double>::min()) return -600.0;
        return 20.0 * std::log10(magnitude);
    }

    double PeqStoredPreampDb()
    {
        return LoadDspSettings().preampDb;
    }

    double PeqResponseAtFrequency(double frequency, int excludedBand, double preampDb)
    {
        double responseDb = preampDb;
        if (!std::isfinite(frequency) || frequency <= 0.0 || frequency >= 24000.0) return responseDb;
        for (size_t index = 0; index < g_peqEditorBands.size(); ++index)
        {
            if (static_cast<int>(index) == excludedBand || !g_peqEditorBands[index].enabled) continue;
            responseDb += BiquadMagnitudeDb(MakePeqBand(48000.0, g_peqEditorBands[index]), frequency);
        }
        return responseDb;
    }

    double PeqHandleResponseDb(size_t index, double preampDb)
    {
        if (index >= g_peqEditorBands.size()) return 0.0;
        const PeqBand& band = g_peqEditorBands[index];
        if (band.frequencyHz >= 24000.0) return band.gainDb;
        if (band.enabled) return PeqResponseAtFrequency(band.frequencyHz, -1, preampDb);
        return PeqResponseAtFrequency(band.frequencyHz, static_cast<int>(index), preampDb) +
            BiquadMagnitudeDb(MakePeqBand(48000.0, band), band.frequencyHz);
    }

    void PeqEnableSelectedControls(bool enabled)
    {
        EnableWindow(g_peqEnabled, enabled);
        EnableWindow(g_peqType, enabled);
        EnableWindow(g_peqFrequency, enabled);
        EnableWindow(g_peqGain, enabled);
        EnableWindow(g_peqQ, enabled);
    }

    void SetPeqEditorFields(const PeqBand& band)
    {
        g_peqUpdatingFields = true;
        const LRESULT checked = band.enabled ? BST_CHECKED : BST_UNCHECKED;
        if (SendMessageW(g_peqEnabled, BM_GETCHECK, 0, 0) != checked)
            SendMessageW(g_peqEnabled, BM_SETCHECK, checked, 0);
        if (SendMessageW(g_peqType, CB_GETCURSEL, 0, 0) !=
            static_cast<LRESULT>(band.type))
            SendMessageW(g_peqType, CB_SETCURSEL,
                static_cast<WPARAM>(band.type), 0);
        SetWindowTextIfChanged(g_peqFrequency,
            PeqGuiParameterText(band.frequencyHz));
        SetWindowTextIfChanged(g_peqGain, PeqGuiParameterText(band.gainDb));
        SetWindowTextIfChanged(g_peqQ, PeqGuiParameterText(band.q));
        g_peqUpdatingFields = false;
        PeqEnableSelectedControls(true);
    }

    void SetPeqSelection(int index)
    {
        g_peqSelectedBand = index;
        if (PeqSelectionIsValid()) SetPeqEditorFields(g_peqEditorBands[g_peqSelectedBand]);
        else PeqEnableSelectedControls(false);
        if (g_peqWindow)
        {
            const RECT graph = PeqGraphRect(g_peqWindow);
            InvalidateRect(g_peqWindow, &graph, FALSE);
        }
    }

    bool ReadPeqEditorFields(HWND owner, PeqBand& band)
    {
        band.enabled = SendMessageW(g_peqEnabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const LRESULT selection = SendMessageW(g_peqType, CB_GETCURSEL, 0, 0);
        if (selection < 0 || selection > static_cast<LRESULT>(PeqFilterType::Notch)) return false;
        band.type = static_cast<PeqFilterType>(selection);
        if (!TryParseFiniteDouble(ReadWindowText(g_peqFrequency), band.frequencyHz) || band.frequencyHz <= 0.0)
        {
            MessageBoxW(owner, L"频点必须是大于 0 的有限数字。没有固定 Hz 上限；超过当前 Nyquist 的值会原样保存，并在该采样率下旁路。",
                L"HXAudio PEQ", MB_OK | MB_ICONWARNING);
            SetFocus(g_peqFrequency);
            return false;
        }
        if (!TryParseFiniteDouble(ReadWindowText(g_peqGain), band.gainDb))
        {
            MessageBoxW(owner, L"增益必须是有限数字。", L"HXAudio PEQ",
                MB_OK | MB_ICONWARNING);
            SetFocus(g_peqGain);
            return false;
        }
        if (band.gainDb < HXAudioDsp::kMinimumPeqGainDb ||
            band.gainDb > HXAudioDsp::kMaximumPeqGainDb)
        {
            MessageBoxW(owner, L"每个 PEQ 频段的增益范围为 -40 dB 到 +40 dB。",
                L"HXAudio PEQ", MB_OK | MB_ICONWARNING);
            SetFocus(g_peqGain);
            return false;
        }
        band.gainDb = HXAudioDsp::ClampPeqGainDb(band.gainDb);
        if (!TryParseFiniteDouble(ReadWindowText(g_peqQ), band.q) || band.q <= 0.0)
        {
            MessageBoxW(owner, L"Q / 斜率必须是大于 0 的有限数字。", L"HXAudio PEQ",
                MB_OK | MB_ICONWARNING);
            SetFocus(g_peqQ);
            return false;
        }
        return true;
    }

    bool CommitPeqEditor()
    {
        if (g_uiSmokeTest)
        {
            g_peqEditorDirty = false;
            return true;
        }
        if (!SavePeqBandsForDevice(g_peqEditorDeviceKey, g_peqEditorBands))
        {
            MessageBoxW(g_peqWindow,
                L"保存 PEQ 失败，当前修改尚未送入音频处理进程。",
                L"HXAudio PEQ", MB_OK | MB_ICONERROR);
            return false;
        }
        g_peqEditorDirty = false;
        SignalDspSettingsChanged();
        UpdatePeqSummary();
        return true;
    }

    void PeqBandsChanged(bool forceLiveCommit)
    {
        g_peqEditorDirty = true;
        if (g_peqWindow)
        {
            const RECT graph = PeqGraphRect(g_peqWindow);
            InvalidateRect(g_peqWindow, &graph, FALSE);
        }
        if (SendMessageW(g_peqLiveApply, BM_GETCHECK, 0, 0) == BST_CHECKED)
        {
            const ULONGLONG now = GetTickCount64();
            if (forceLiveCommit || now - g_peqLastLiveCommit >= 50)
            {
                CommitPeqEditor();
                g_peqLastLiveCommit = now;
            }
        }
    }

    void PeqEnsureSelectedVisible()
    {
        if (!PeqSelectionIsValid()) return;
        const PeqBand& band = g_peqEditorBands[g_peqSelectedBand];
        if (band.frequencyHz < g_peqViewMinHz)
            g_peqViewMinHz = std::max(band.frequencyHz / 1.5, std::numeric_limits<double>::min());
        if (band.frequencyHz > g_peqViewMaxHz)
            g_peqViewMaxHz = band.frequencyHz > std::numeric_limits<double>::max() / 1.5 ?
                std::numeric_limits<double>::max() : band.frequencyHz * 1.5;
        const double handleDb = PeqHandleResponseDb(static_cast<size_t>(g_peqSelectedBand), PeqStoredPreampDb());
        if (std::abs(handleDb) > g_peqViewDbRange * 0.9)
            g_peqViewDbRange = std::clamp(std::abs(handleDb) * 1.25,
                kPeqMinimumViewDbRange, HXAudioDsp::kMaximumPeqGainDb);
    }

    void PeqFitView()
    {
        double minimumFrequency = 10.0;
        double maximumFrequency = 24000.0;
        const double preampDb = PeqStoredPreampDb();
        double maximumGain = std::abs(preampDb);
        for (const auto& band : g_peqEditorBands)
        {
            if (!std::isfinite(band.frequencyHz) || band.frequencyHz <= 0.0) continue;
            if (band.frequencyHz < minimumFrequency)
                minimumFrequency = std::max(band.frequencyHz / 1.35, std::numeric_limits<double>::min());
            if (band.frequencyHz > maximumFrequency)
                maximumFrequency = band.frequencyHz > std::numeric_limits<double>::max() / 1.35 ?
                    std::numeric_limits<double>::max() : band.frequencyHz * 1.35;
            maximumGain = std::max(maximumGain, std::abs(band.gainDb));
        }
        if (!(maximumFrequency > minimumFrequency))
        {
            minimumFrequency = 10.0;
            maximumFrequency = 24000.0;
        }
        g_peqViewMinHz = minimumFrequency;
        g_peqViewMaxHz = maximumFrequency;
        for (size_t index = 0; index < g_peqEditorBands.size(); ++index)
            maximumGain = std::max(maximumGain, std::abs(PeqHandleResponseDb(index, preampDb)));
        g_peqViewDbRange = std::clamp(maximumGain * 1.25,
            kPeqMinimumViewDbRange, HXAudioDsp::kMaximumPeqGainDb);
        if (g_peqWindow)
        {
            const RECT graph = PeqGraphRect(g_peqWindow);
            InvalidateRect(g_peqWindow, &graph, FALSE);
        }
    }

    int PeqHitTest(HWND window, POINT point)
    {
        const RECT graph = PeqGraphRect(window);
        const double preampDb = PeqStoredPreampDb();
        int bestIndex = -1;
        double bestDistance = static_cast<double>(UiScale(window, 16));
        for (size_t index = 0; index < g_peqEditorBands.size(); ++index)
        {
            const PeqBand& band = g_peqEditorBands[index];
            if (band.frequencyHz < g_peqViewMinHz || band.frequencyHz > g_peqViewMaxHz) continue;
            const int x = PeqFrequencyToX(band.frequencyHz, graph);
            const double dx = static_cast<double>(point.x - x);
            if (std::abs(dx) >= bestDistance) continue;
            const double handleDb = PeqHandleResponseDb(index, preampDb);
            if (handleDb < -g_peqViewDbRange || handleDb > g_peqViewDbRange) continue;
            const int y = PeqGainToY(handleDb, graph);
            const double dy = static_cast<double>(point.y - y);
            const double distance = std::sqrt(dx * dx + dy * dy);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestIndex = static_cast<int>(index);
            }
        }
        return bestIndex;
    }

    void PeqDeleteSelected()
    {
        if (!PeqSelectionIsValid()) return;
        g_peqEditorBands.erase(g_peqEditorBands.begin() + g_peqSelectedBand);
        const int next = g_peqEditorBands.empty() ? -1 :
            std::min<int>(g_peqSelectedBand, static_cast<int>(g_peqEditorBands.size() - 1));
        SetPeqSelection(next);
        PeqBandsChanged(true);
    }

    bool PeqApplyExactValues(HWND owner)
    {
        if (!PeqSelectionIsValid()) return false;
        PeqBand band;
        if (!ReadPeqEditorFields(owner, band)) return false;
        g_peqEditorBands[g_peqSelectedBand] = band;
        PeqEnsureSelectedVisible();
        PeqBandsChanged(true);
        return true;
    }

    void PeqZoomFrequencyView(HWND window, int mouseX, double wheelSteps)
    {
        const RECT graph = PeqGraphRect(window);
        const double width = std::max<LONG>(graph.right - graph.left, 1);
        const double anchor = (static_cast<double>(mouseX) - graph.left) / width;
        const double minimumLog = PeqSafeLog(g_peqViewMinHz);
        const double maximumLog = PeqSafeLog(g_peqViewMaxHz);
        const double span = maximumLog - minimumLog;
        const double newSpan = std::clamp(span * std::pow(1.25, -wheelSteps), 1.0e-9,
            std::log(std::numeric_limits<double>::max()) - std::log(std::numeric_limits<double>::min()));
        const double anchorLog = minimumLog + anchor * span;
        double newMinimum = anchorLog - anchor * newSpan;
        double newMaximum = newMinimum + newSpan;
        const double lowest = std::log(std::numeric_limits<double>::min());
        const double highest = std::log(std::numeric_limits<double>::max());
        if (newMinimum < lowest)
        {
            newMaximum += lowest - newMinimum;
            newMinimum = lowest;
        }
        if (newMaximum > highest)
        {
            newMinimum -= newMaximum - highest;
            newMaximum = highest;
        }
        g_peqViewMinHz = std::exp(newMinimum);
        g_peqViewMaxHz = std::exp(newMaximum);
        InvalidateRect(window, &graph, FALSE);
    }

    void PeqZoomGainView(HWND window, double wheelSteps)
    {
        const double range = g_peqViewDbRange * std::pow(1.25, -wheelSteps);
        if (!std::isfinite(range) || range <= 0.0) return;
        g_peqViewDbRange = std::clamp(range,
            kPeqMinimumViewDbRange, HXAudioDsp::kMaximumPeqGainDb);
        const RECT dirty = PeqGainZoomDirtyRect(window);
        // Gain labels live in the gutter to the left of the graph. Invalidating
        // only the plot updated the curve/grid while leaving cached Y-axis text
        // from the previous range on screen.
        InvalidateRect(window, &dirty, FALSE);
    }

    void PaintPeqEditor(HWND window, HDC destination)
    {
        RECT client{};
        GetClientRect(window, &client);
        const int width = std::max<LONG>(client.right, 1);
        const int height = std::max<LONG>(client.bottom, 1);
        UiSurfaceCache* cache = EnsureUiSurfaceCache(window, destination, width, height);
        if (!cache) return;
        HDC canvas = cache->canvas;
        void* pixels = cache->pixels;
        RECT update{};
        if (!UiPaintRegion(destination, client, cache->valid, update)) return;
        const int savedDc = SaveDC(canvas);
        IntersectClipRect(canvas, update.left, update.top, update.right, update.bottom);
        PrepareUiCanvasRegion(pixels, width, height, canvas, update);
        SetBkMode(canvas, TRANSPARENT);

        const RECT graph = PeqGraphRect(window);
        Gdiplus::Graphics graphics(canvas);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        const float scale = static_cast<float>(UiDpi(window)) / 96.0f;
        const Gdiplus::RectF graphBounds(static_cast<float>(graph.left),
            static_cast<float>(graph.top), static_cast<float>(graph.right - graph.left),
            static_cast<float>(graph.bottom - graph.top));
        Gdiplus::RectF graphShadow = graphBounds;
        graphShadow.Y += 2.0f * scale;
        FillRounded(graphics, graphShadow, 12.0f * scale,
            Gdiplus::Color(g_ui.dark ? 52 : 18, 0, 0, 0));
        FillRounded(graphics, graphBounds, 12.0f * scale, GdiColor(g_ui.graph));
        StrokeRounded(graphics, graphBounds, 12.0f * scale, GdiColor(g_ui.border));

        Gdiplus::RectF topCard(static_cast<float>(UiScale(window, 24)),
            static_cast<float>(UiScale(window, 18)),
            static_cast<float>(std::max<LONG>(1, client.right - UiScale(window, 48))),
            static_cast<float>(UiScale(window, 50)));
        FillRounded(graphics, topCard, 10.0f * scale, GdiColor(g_ui.surface));
        StrokeRounded(graphics, topCard, 10.0f * scale, GdiColor(g_ui.border));
        Gdiplus::RectF bottomCard(static_cast<float>(UiScale(window, 24)),
            static_cast<float>(graph.bottom + UiScale(window, 36)),
            static_cast<float>(std::max<LONG>(1, client.right - UiScale(window, 48))),
            static_cast<float>(std::max<LONG>(1,
                client.bottom - graph.bottom - UiScale(window, 54))));
        FillRounded(graphics, bottomCard, 12.0f * scale, GdiColor(g_ui.surface));
        StrokeRounded(graphics, bottomCard, 12.0f * scale, GdiColor(g_ui.border));

        constexpr double nyquist = 24000.0;
        if (g_peqViewMaxHz > nyquist)
        {
            RECT unavailable = graph;
            unavailable.left = std::clamp(PeqFrequencyToX(nyquist, graph),
                static_cast<int>(graph.left), static_cast<int>(graph.right));
            Gdiplus::SolidBrush unavailableBrush(GdiColor(g_ui.muted, g_ui.dark ? 18 : 12));
            graphics.FillRectangle(&unavailableBrush, unavailable.left, unavailable.top,
                unavailable.right - unavailable.left, unavailable.bottom - unavailable.top);
        }

        Gdiplus::Pen gridPen(GdiColor(g_ui.grid, g_ui.dark ? 155 : 190), 1.0f);
        Gdiplus::Pen zeroPen(GdiColor(g_ui.muted, 185), 1.7f);
        SetTextColor(canvas, g_ui.muted);
        HGDIOBJ oldFont = SelectObject(canvas, UiFont(UiDpi(window), UiFontRole::Caption));
        const double gainStep = PeqNiceGridStep(g_peqViewDbRange / 4.0);
        const int gainLines = static_cast<int>(std::ceil(g_peqViewDbRange / gainStep));
        for (int line = -gainLines; line <= gainLines; ++line)
        {
            const double gain = line * gainStep;
            if (gain < -g_peqViewDbRange || gain > g_peqViewDbRange) continue;
            const int y = PeqGainToY(gain, graph);
            graphics.DrawLine(line == 0 ? &zeroPen : &gridPen,
                static_cast<float>(graph.left), static_cast<float>(y),
                static_cast<float>(graph.right), static_cast<float>(y));
            wchar_t label[40]{};
            swprintf_s(label, L"%+.4g", gain);
            RECT labelRect{ UiScale(window, 4), y - UiScale(window, 9),
                graph.left - UiScale(window, 8), y + UiScale(window, 10) };
            DrawTextW(canvas, label, -1, &labelRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }

        const int minimumExponent = static_cast<int>(std::floor(std::log10(g_peqViewMinHz))) - 1;
        const int maximumExponent = static_cast<int>(std::ceil(std::log10(g_peqViewMaxHz))) + 1;
        for (int exponent = std::max(minimumExponent, -307); exponent <= std::min(maximumExponent, 308); ++exponent)
        {
            const double decade = std::pow(10.0, exponent);
            for (double multiplier : { 1.0, 2.0, 5.0 })
            {
                const double frequency = decade * multiplier;
                if (!std::isfinite(frequency) || frequency < g_peqViewMinHz || frequency > g_peqViewMaxHz) continue;
                const int x = PeqFrequencyToX(frequency, graph);
                graphics.DrawLine(&gridPen, static_cast<float>(x), static_cast<float>(graph.top),
                    static_cast<float>(x), static_cast<float>(graph.bottom));
                const std::wstring label = PeqAxisFrequencyText(frequency);
                RECT labelRect{ x - UiScale(window, 32), graph.bottom + UiScale(window, 5),
                    x + UiScale(window, 32), graph.bottom + UiScale(window, 25) };
                DrawTextW(canvas, label.c_str(), -1, &labelRect, DT_CENTER | DT_TOP | DT_SINGLELINE);
            }
        }

        if (g_peqViewMinHz < nyquist && g_peqViewMaxHz > nyquist)
        {
            const int x = PeqFrequencyToX(nyquist, graph);
            Gdiplus::Pen nyquistPen(GdiColor(g_ui.danger, 180), 1.2f);
            nyquistPen.SetDashStyle(Gdiplus::DashStyleDash);
            graphics.DrawLine(&nyquistPen, static_cast<float>(x), static_cast<float>(graph.top),
                static_cast<float>(x), static_cast<float>(graph.bottom));
            SetTextColor(canvas, g_ui.danger);
            RECT labelRect{ x - UiScale(window, 92), graph.top + UiScale(window, 7),
                x - UiScale(window, 7), graph.top + UiScale(window, 27) };
            DrawTextW(canvas, L"24 kHz Nyquist", -1, &labelRect, DT_RIGHT | DT_SINGLELINE);
            SetTextColor(canvas, g_ui.muted);
        }

        std::vector<BiquadCoefficients> coefficients;
        coefficients.reserve(g_peqEditorBands.size());
        for (const auto& band : g_peqEditorBands)
            if (band.enabled) coefficients.push_back(MakePeqBand(48000.0, band));
        const double preampDb = PeqStoredPreampDb();
        std::vector<Gdiplus::PointF> response;
        response.reserve(std::max<LONG>(graph.right - graph.left + 1, 0));
        for (int x = graph.left; x <= graph.right; ++x)
        {
            const double frequency = PeqXToFrequency(x, graph);
            if (frequency >= nyquist) break;
            double responseDb = preampDb;
            for (const auto& coefficient : coefficients)
                responseDb += BiquadMagnitudeDb(coefficient, frequency);
            const int unclippedY = PeqGainToY(responseDb, graph);
            response.push_back({ static_cast<float>(x), static_cast<float>(
                std::clamp(unclippedY, static_cast<int>(graph.top), static_cast<int>(graph.bottom))) });
        }
        if (response.size() >= 2)
        {
            Gdiplus::GraphicsPath areaPath;
            areaPath.AddLines(response.data(), static_cast<INT>(response.size()));
            areaPath.AddLine(response.back(), Gdiplus::PointF(response.back().X,
                static_cast<float>(graph.bottom)));
            areaPath.AddLine(Gdiplus::PointF(response.back().X, static_cast<float>(graph.bottom)),
                Gdiplus::PointF(response.front().X, static_cast<float>(graph.bottom)));
            areaPath.CloseFigure();
            Gdiplus::LinearGradientBrush areaBrush(graphBounds,
                GdiColor(g_ui.accent, g_ui.dark ? 78 : 54), GdiColor(g_ui.accent, 3),
                Gdiplus::LinearGradientModeVertical);
            const Gdiplus::GraphicsState state = graphics.Save();
            Gdiplus::GraphicsPath clipPath;
            AddRoundedPath(clipPath, graphBounds, 12.0f * scale);
            graphics.SetClip(&clipPath);
            graphics.FillPath(&areaBrush, &areaPath);
            Gdiplus::Pen responseGlow(GdiColor(g_ui.accent, 55), 7.0f * scale);
            graphics.DrawLines(&responseGlow, response.data(), static_cast<INT>(response.size()));
            Gdiplus::Pen responsePen(GdiColor(g_ui.accent), 2.6f * scale);
            responsePen.SetLineJoin(Gdiplus::LineJoinRound);
            graphics.DrawLines(&responsePen, response.data(), static_cast<INT>(response.size()));
            graphics.Restore(state);
        }

        if (g_peqDraggingBand && PeqSelectionIsValid())
        {
            const PeqBand& band = g_peqEditorBands[g_peqSelectedBand];
            const int x = PeqFrequencyToX(band.frequencyHz, graph);
            const int y = PeqGainToY(PeqHandleResponseDb(g_peqSelectedBand, preampDb), graph);
            Gdiplus::Pen guide(GdiColor(g_ui.accent, 105), 1.0f);
            guide.SetDashStyle(Gdiplus::DashStyleDot);
            graphics.DrawLine(&guide, static_cast<float>(x), static_cast<float>(graph.top),
                static_cast<float>(x), static_cast<float>(graph.bottom));
            graphics.DrawLine(&guide, static_cast<float>(graph.left), static_cast<float>(y),
                static_cast<float>(graph.right), static_cast<float>(y));
        }

        int labelIndex = PeqSelectionIsValid() ? g_peqSelectedBand : g_peqHoveredBand;
        int labelX = 0;
        int labelY = 0;
        for (size_t index = 0; index < g_peqEditorBands.size(); ++index)
        {
            const PeqBand& band = g_peqEditorBands[index];
            if (band.frequencyHz < g_peqViewMinHz || band.frequencyHz > g_peqViewMaxHz) continue;
            const double handleDb = PeqHandleResponseDb(index, preampDb);
            if (handleDb < -g_peqViewDbRange || handleDb > g_peqViewDbRange) continue;
            const int x = PeqFrequencyToX(band.frequencyHz, graph);
            const int y = PeqGainToY(handleDb, graph);
            const bool selected = static_cast<int>(index) == g_peqSelectedBand;
            const bool hovered = static_cast<int>(index) == g_peqHoveredBand;
            const float radius = static_cast<float>(
                UiScale(window, selected ? 8 : hovered ? 7 : 6));
            const COLORREF pointColor = !band.enabled ? g_ui.muted :
                selected ? g_ui.accent : BlendColor(g_ui.accent, RGB(255, 255, 255), g_ui.dark ? 0.20 : 0.10);
            if (selected || hovered)
            {
                Gdiplus::SolidBrush halo(GdiColor(pointColor, selected ? 50 : 30));
                graphics.FillEllipse(&halo, x - radius - 5.0f * scale, y - radius - 5.0f * scale,
                    radius * 2.0f + 10.0f * scale, radius * 2.0f + 10.0f * scale);
            }
            Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(62, 0, 0, 0));
            graphics.FillEllipse(&shadowBrush, x - radius, y - radius + 2.0f * scale,
                radius * 2.0f, radius * 2.0f);
            Gdiplus::SolidBrush pointBrush(GdiColor(pointColor));
            graphics.FillEllipse(&pointBrush, x - radius, y - radius, radius * 2.0f, radius * 2.0f);
            Gdiplus::Pen pointRing(GdiColor(g_ui.dark ? RGB(245, 245, 245) : RGB(255, 255, 255)),
                selected ? 2.4f * scale : 1.6f * scale);
            graphics.DrawEllipse(&pointRing, x - radius, y - radius, radius * 2.0f, radius * 2.0f);
            if (static_cast<int>(index) == labelIndex)
            {
                labelX = x;
                labelY = y;
            }
        }

        if (labelIndex >= 0 && static_cast<size_t>(labelIndex) < g_peqEditorBands.size())
        {
            const PeqBand& band = g_peqEditorBands[labelIndex];
            const int tooltipWidth = UiScale(window, 330);
            const int tooltipHeight = UiScale(window, 38);
            int left = labelX + UiScale(window, 16);
            if (left + tooltipWidth > graph.right - UiScale(window, 8))
                left = labelX - tooltipWidth - UiScale(window, 16);
            left = std::clamp(left, static_cast<int>(graph.left + UiScale(window, 8)),
                static_cast<int>(graph.right - tooltipWidth - UiScale(window, 8)));
            int top = labelY - tooltipHeight - UiScale(window, 12);
            if (top < graph.top + UiScale(window, 8)) top = labelY + UiScale(window, 16);
            Gdiplus::RectF tooltip(static_cast<float>(left), static_cast<float>(top),
                static_cast<float>(tooltipWidth), static_cast<float>(tooltipHeight));
            Gdiplus::RectF tooltipShadow = tooltip;
            tooltipShadow.Y += 2.0f * scale;
            FillRounded(graphics, tooltipShadow, 8.0f * scale, Gdiplus::Color(72, 0, 0, 0));
            FillRounded(graphics, tooltip, 8.0f * scale, GdiColor(g_ui.elevated));
            StrokeRounded(graphics, tooltip, 8.0f * scale, GdiColor(g_ui.border));
            const std::wstring label = L"#" + std::to_wstring(labelIndex + 1) + L"  " +
                PeqTypeDisplayName(band.type) + L"   " + PeqGuiParameterText(band.frequencyHz) +
                L" Hz   " + PeqGuiParameterText(band.gainDb) + L" dB   Q " + PeqGuiParameterText(band.q);
            RECT tooltipText{ left + UiScale(window, 12), top,
                left + tooltipWidth - UiScale(window, 10), top + tooltipHeight };
            SetTextColor(canvas, g_ui.text);
            SelectObject(canvas, UiFont(UiDpi(window), UiFontRole::Body));
            DrawTextW(canvas, label.c_str(), -1, &tooltipText,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        const auto drawZoomButton = [&](const RECT& bounds, const wchar_t* label,
            bool hovered)
        {
            Gdiplus::RectF button(static_cast<float>(bounds.left),
                static_cast<float>(bounds.top),
                static_cast<float>(bounds.right - bounds.left),
                static_cast<float>(bounds.bottom - bounds.top));
            Gdiplus::RectF shadow = button;
            shadow.Y += 1.5f * scale;
            FillRounded(graphics, shadow, 7.0f * scale,
                Gdiplus::Color(g_ui.dark ? 58 : 22, 0, 0, 0));
            const COLORREF fill = hovered
                ? BlendColor(g_ui.surfaceAlt, g_ui.accent, g_ui.dark ? 0.20 : 0.10)
                : g_ui.elevated;
            FillRounded(graphics, button, 7.0f * scale, GdiColor(fill));
            StrokeRounded(graphics, button, 7.0f * scale,
                GdiColor(hovered ? g_ui.accent : g_ui.border));
            graphics.Flush(Gdiplus::FlushIntentionSync);
            RECT textBounds = bounds;
            SetTextColor(canvas, g_ui.text);
            SelectObject(canvas, UiFont(UiDpi(window), UiFontRole::Section));
            DrawTextW(canvas, label, -1, &textBounds,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        };
        drawZoomButton(PeqGainZoomOutRect(window), L"−",
            g_peqGainZoomHover == 1);
        drawZoomButton(PeqGainZoomInRect(window), L"+",
            g_peqGainZoomHover == 2);

        SelectObject(canvas, oldFont);
        StrokeRounded(graphics, graphBounds, 12.0f * scale, GdiColor(g_ui.border));
        graphics.Flush(Gdiplus::FlushIntentionSync);
        FinalizeUiCanvasAlphaRegion(pixels, width, height, update);
        cache->valid = true;
        RestoreDC(canvas, savedDc);
        BitBlt(destination, update.left, update.top,
            update.right - update.left, update.bottom - update.top,
            canvas, update.left, update.top, SRCCOPY);
    }

    void ReloadPeqEditor()
    {
        if (!g_peqWindow) return;
        g_peqEditorBands = LoadPeqBandsForDevice(
            g_peqEditorDeviceKey, g_peqEditorDeviceId);
        g_peqEditorDirty = false;
        g_peqSelectedBand = g_peqEditorBands.empty() ? -1 : 0;
        PeqFitView();
        SetPeqSelection(g_peqSelectedBand);
    }

    void ReloadPeqDeviceChoices()
    {
        if (!g_peqDevice) return;
        const std::wstring previous = g_peqEditorDeviceId.empty()
            ? ReadRegistryString(L"PeqEditorDeviceId") : g_peqEditorDeviceId;
        g_peqEditorDevices = PeqEditableDevices();
        if (g_peqEditorDevices.empty())
            g_peqEditorDevices.push_back({ L"", L"默认兼容曲线", L"" });

        SendMessageW(g_peqDevice, CB_RESETCONTENT, 0, 0);
        int selected = 0;
        for (size_t index = 0; index < g_peqEditorDevices.size(); ++index)
        {
            SendMessageW(g_peqDevice, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(g_peqEditorDevices[index].name.c_str()));
            if (g_peqEditorDevices[index].id == previous) selected = static_cast<int>(index);
        }
        SendMessageW(g_peqDevice, CB_SETCURSEL, selected, 0);
        g_peqEditorDeviceId = g_peqEditorDevices[selected].id;
        g_peqEditorDeviceKey = g_peqEditorDevices[selected].profileKey;
        if (!g_peqEditorDeviceId.empty())
            WriteRegistryString(L"PeqEditorDeviceId", g_peqEditorDeviceId);
        ReloadPeqEditor();
    }

    bool SwitchPeqEditorDevice(int index)
    {
        if (index < 0 || static_cast<size_t>(index) >= g_peqEditorDevices.size()) return false;
        const std::wstring nextId = g_peqEditorDevices[index].id;
        if (nextId == g_peqEditorDeviceId) return true;
        if (g_peqEditorDirty && !CommitPeqEditor()) return false;
        g_peqEditorDeviceId = nextId;
        g_peqEditorDeviceKey = g_peqEditorDevices[index].profileKey;
        if (!nextId.empty()) WriteRegistryString(L"PeqEditorDeviceId", nextId);
        ReloadPeqEditor();
        return true;
    }

    LRESULT CALLBACK PeqEditorProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
        {
            ConfigureModernWindow(window);
            g_peqWindow = window;
            const int controlsY = 540;
            HWND deviceLabel = CreateWindowW(L"STATIC", L"调节设备", WS_CHILD | WS_VISIBLE,
                44, 33, 78, 22, window, nullptr, nullptr, nullptr);
            g_peqDevice = CreateWindowW(WC_COMBOBOXW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                132, 25, 720, 400, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPeqDeviceId)), nullptr, nullptr);
            g_peqEnabled = CreateWindowW(L"BUTTON", L"启用频点", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                44, controlsY, 112, 28, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPeqEnabledId)), nullptr, nullptr);
            CreateWindowW(L"STATIC", L"类型", WS_CHILD | WS_VISIBLE,
                174, controlsY + 4, 42, 22, window, nullptr, nullptr, nullptr);
            g_peqType = CreateWindowW(WC_COMBOBOXW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                220, controlsY - 3, 132, 300, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPeqTypeId)), nullptr, nullptr);
            for (int type = 0; type <= static_cast<int>(PeqFilterType::Notch); ++type)
                SendMessageW(g_peqType, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(PeqTypeDisplayName(static_cast<PeqFilterType>(type))));

            CreateWindowW(L"STATIC", L"频点 Hz", WS_CHILD | WS_VISIBLE,
                368, controlsY + 4, 64, 22, window, nullptr, nullptr, nullptr);
            g_peqFrequency = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1000",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                438, controlsY - 3, 120, 30, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPeqFrequencyId)), nullptr, nullptr);
            CreateWindowW(L"STATIC", L"增益 dB", WS_CHILD | WS_VISIBLE,
                576, controlsY + 4, 64, 22, window, nullptr, nullptr, nullptr);
            g_peqGain = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                646, controlsY - 3, 100, 30, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPeqGainId)), nullptr, nullptr);
            CreateWindowW(L"STATIC", L"Q / 斜率", WS_CHILD | WS_VISIBLE,
                762, controlsY + 4, 68, 22, window, nullptr, nullptr, nullptr);
            g_peqQ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0.9",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                836, controlsY - 3, 90, 30, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPeqQId)), nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"应用精确值", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                1004, controlsY - 6, 132, 36, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPeqApplyValueId)), nullptr, nullptr);

            CreateWindowW(L"STATIC",
                L"双击添加频点 · 拖动改变频率与增益 · 滚轮缩放增益范围 · Shift + 滚轮缩放频率 · Ctrl + 滚轮调 Q · 右键或 Delete 删除",
                WS_CHILD | WS_VISIBLE, 44, controlsY + 43, 1090, 30,
                window, nullptr, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"适配全部频点", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                44, controlsY + 86, 132, 36, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPeqFitViewId)), nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"删除选中点", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                188, controlsY + 86, 122, 36, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPeqDeleteId)), nullptr, nullptr);
            g_peqLiveApply = CreateWindowW(L"BUTTON", L"实时应用", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                326, controlsY + 91, 130, 28, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPeqLiveApplyId)), nullptr, nullptr);
            SetPropW(g_peqLiveApply, kUiToggleSwitchProperty,
                reinterpret_cast<HANDLE>(1));
            SendMessageW(g_peqLiveApply, BM_SETCHECK, BST_CHECKED, 0);

            ScaleChildrenForDpi(window, 96, UiDpi(window));
            ApplyDefaultFont(window);
            SetUiFontRole(deviceLabel, UiFontRole::Body);
            ReloadPeqDeviceChoices();
            return 0;
        }
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            PaintPeqEditor(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintPeqEditor(window, reinterpret_cast<HDC>(wParam));
            return 0;
        case kApplyInitialDpiSizeMessage:
            SizeWindowForCurrentDpi(window, 1180, 760);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DRAWITEM:
            if (DrawModernButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam)) ||
                DrawModernChoiceItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam))) return TRUE;
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
            return ControlColor(message, wParam, lParam);
        case WM_DPICHANGED:
            ApplyDpiChange(window, wParam, lParam);
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            RefreshUiPalette();
            ConfigureModernWindow(window);
            ModernizeChildren(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
        {
            SetFocus(window);
            POINT point{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            const RECT zoomOut = PeqGainZoomOutRect(window);
            const RECT zoomIn = PeqGainZoomInRect(window);
            if (PtInRect(&zoomOut, point))
            {
                PeqZoomGainView(window, -1.0);
                return 0;
            }
            if (PtInRect(&zoomIn, point))
            {
                PeqZoomGainView(window, 1.0);
                return 0;
            }
            const int hit = PeqHitTest(window, point);
            if (hit >= 0)
            {
                SetPeqSelection(hit);
                g_peqDraggingBand = true;
                const RECT graph = PeqGraphRect(window);
                const int handleX = PeqFrequencyToX(g_peqEditorBands[hit].frequencyHz, graph);
                const int handleY = PeqGainToY(PeqHandleResponseDb(hit, PeqStoredPreampDb()), graph);
                g_peqDragOffset = { point.x - handleX, point.y - handleY };
                SetCapture(window);
            }
            else SetPeqSelection(-1);
            return 0;
        }
        case WM_LBUTTONDBLCLK:
        {
            POINT point{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            const RECT graph = PeqGraphRect(window);
            if (!PtInRect(&graph, point)) return 0;
            const RECT zoomOut = PeqGainZoomOutRect(window);
            const RECT zoomIn = PeqGainZoomInRect(window);
            if (PtInRect(&zoomOut, point) || PtInRect(&zoomIn, point)) return 0;
            const int hit = PeqHitTest(window, point);
            if (hit >= 0)
            {
                SetPeqSelection(hit);
                return 0;
            }
            PeqBand band;
            band.enabled = true;
            band.type = PeqFilterType::Peak;
            band.frequencyHz = PeqXToFrequency(point.x, graph);
            band.gainDb = HXAudioDsp::ClampPeqGainDb(PeqYToGain(point.y, graph) -
                PeqResponseAtFrequency(band.frequencyHz, -1, PeqStoredPreampDb()));
            band.q = 0.9;
            g_peqEditorBands.push_back(band);
            SetPeqSelection(static_cast<int>(g_peqEditorBands.size() - 1));
            PeqBandsChanged(true);
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            POINT pointer{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            g_peqPointer = pointer;
            const RECT pointerGraph = PeqGraphRect(window);
            const bool inside = PtInRect(&pointerGraph, pointer) != FALSE;
            const RECT zoomOut = PeqGainZoomOutRect(window);
            const RECT zoomIn = PeqGainZoomInRect(window);
            const int zoomHover = PtInRect(&zoomOut, pointer) ? 1 :
                PtInRect(&zoomIn, pointer) ? 2 : 0;
            if (inside)
            {
                TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, window, 0 };
                TrackMouseEvent(&tracking);
            }
            const int hovered = inside && zoomHover == 0 ?
                PeqHitTest(window, pointer) : -1;
            if (hovered != g_peqHoveredBand || inside != g_peqPointerInGraph ||
                zoomHover != g_peqGainZoomHover)
            {
                g_peqHoveredBand = hovered;
                g_peqPointerInGraph = inside;
                g_peqGainZoomHover = zoomHover;
                InvalidateRect(window, &pointerGraph, FALSE);
            }
            if (g_peqDraggingBand && PeqSelectionIsValid())
            {
                const RECT graph = PeqGraphRect(window);
                const int x = std::clamp(static_cast<int>(pointer.x - g_peqDragOffset.x),
                    static_cast<int>(graph.left), static_cast<int>(graph.right));
                const int y = std::clamp(static_cast<int>(pointer.y - g_peqDragOffset.y),
                    static_cast<int>(graph.top), static_cast<int>(graph.bottom));
                PeqBand& band = g_peqEditorBands[g_peqSelectedBand];
                band.frequencyHz = PeqXToFrequency(x, graph);
                double targetResponseDb = PeqYToGain(y, graph);
                if ((GetKeyState(VK_MENU) & 0x8000) == 0 && std::abs(targetResponseDb) < 0.14)
                    targetResponseDb = 0.0;
                const double otherResponseDb = PeqResponseAtFrequency(
                    band.frequencyHz, g_peqSelectedBand, PeqStoredPreampDb());
                if (band.type == PeqFilterType::Peak)
                    band.gainDb = targetResponseDb - otherResponseDb;
                else if (band.type == PeqFilterType::LowShelf || band.type == PeqFilterType::HighShelf)
                    band.gainDb = 2.0 * (targetResponseDb - otherResponseDb);
                band.gainDb = HXAudioDsp::ClampPeqGainDb(band.gainDb);
                SetPeqEditorFields(band);
                PeqBandsChanged(false);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_peqHoveredBand = -1;
            g_peqPointerInGraph = false;
            g_peqGainZoomHover = 0;
        {
            const RECT graph = PeqGraphRect(window);
            InvalidateRect(window, &graph, FALSE);
        }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT)
            {
                POINT pointer{};
                GetCursorPos(&pointer);
                ScreenToClient(window, &pointer);
                const RECT graph = PeqGraphRect(window);
                if (PtInRect(&graph, pointer))
                {
                    const RECT zoomOut = PeqGainZoomOutRect(window);
                    const RECT zoomIn = PeqGainZoomInRect(window);
                    if (PtInRect(&zoomOut, pointer) || PtInRect(&zoomIn, pointer))
                    {
                        SetCursor(LoadCursorW(nullptr, IDC_HAND));
                        return TRUE;
                    }
                    SetCursor(LoadCursorW(nullptr,
                        (g_peqDraggingBand || PeqHitTest(window, pointer) >= 0) ? IDC_SIZEALL : IDC_CROSS));
                    return TRUE;
                }
            }
            break;
        case WM_LBUTTONUP:
            if (g_peqDraggingBand)
            {
                g_peqDraggingBand = false;
                if (GetCapture() == window) ReleaseCapture();
                PeqEnsureSelectedVisible();
                PeqBandsChanged(true);
            }
            return 0;
        case WM_CAPTURECHANGED:
            g_peqDraggingBand = false;
            return 0;
        case WM_RBUTTONDOWN:
        {
            POINT point{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            const int hit = PeqHitTest(window, point);
            if (hit >= 0)
            {
                SetPeqSelection(hit);
                PeqDeleteSelected();
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            const double steps = static_cast<double>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
            POINT point{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            ScreenToClient(window, &point);
            const RECT graph = PeqGraphRect(window);
            if (!PtInRect(&graph, point)) break;
            const UINT keys = GET_KEYSTATE_WPARAM(wParam);
            if (keys & MK_SHIFT)
            {
                PeqZoomFrequencyView(window, point.x, steps);
            }
            else if (keys & MK_CONTROL)
            {
                if (PeqSelectionIsValid())
                {
                    PeqBand& band = g_peqEditorBands[g_peqSelectedBand];
                    const double q = band.q * std::pow(1.15, steps);
                    if (std::isfinite(q) && q > 0.0)
                    {
                        band.q = q;
                        SetPeqEditorFields(band);
                        PeqBandsChanged(true);
                    }
                }
            }
            else PeqZoomGainView(window, steps);
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_DELETE)
            {
                PeqDeleteSelected();
                return 0;
            }
            if (PeqSelectionIsValid() && (wParam == VK_LEFT || wParam == VK_RIGHT ||
                wParam == VK_UP || wParam == VK_DOWN || wParam == VK_SPACE))
            {
                PeqBand& band = g_peqEditorBands[g_peqSelectedBand];
                const bool coarse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                if (wParam == VK_LEFT || wParam == VK_RIGHT)
                {
                    const double semitoneSteps = coarse ? 1.0 : 0.125;
                    band.frequencyHz *= std::pow(2.0,
                        (wParam == VK_RIGHT ? semitoneSteps : -semitoneSteps) / 12.0);
                }
                else if (wParam == VK_UP || wParam == VK_DOWN)
                    band.gainDb = HXAudioDsp::ClampPeqGainDb(band.gainDb +
                        (wParam == VK_UP ? 1.0 : -1.0) * (coarse ? 1.0 : 0.1));
                else if (wParam == VK_SPACE)
                    band.enabled = !band.enabled;
                SetPeqEditorFields(band);
                PeqEnsureSelectedVisible();
                PeqBandsChanged(true);
                return 0;
            }
            break;
        case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
            case kPeqDeviceId:
                if (HIWORD(wParam) == CBN_SELCHANGE)
                {
                    const int previous = static_cast<int>(std::distance(g_peqEditorDevices.begin(),
                        std::find_if(g_peqEditorDevices.begin(), g_peqEditorDevices.end(),
                            [&](const DeviceInfo& item) { return item.id == g_peqEditorDeviceId; })));
                    const int selected = static_cast<int>(SendMessageW(g_peqDevice, CB_GETCURSEL, 0, 0));
                    if (!SwitchPeqEditorDevice(selected) && previous >= 0 &&
                        static_cast<size_t>(previous) < g_peqEditorDevices.size())
                        SendMessageW(g_peqDevice, CB_SETCURSEL, previous, 0);
                }
                break;
            case kPeqEnabledId:
                if (!g_peqUpdatingFields && HIWORD(wParam) == BN_CLICKED && PeqSelectionIsValid())
                {
                    g_peqEditorBands[g_peqSelectedBand].enabled =
                        SendMessageW(g_peqEnabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    PeqBandsChanged(true);
                }
                break;
            case kPeqTypeId:
                if (!g_peqUpdatingFields && HIWORD(wParam) == CBN_SELCHANGE && PeqSelectionIsValid())
                {
                    const LRESULT type = SendMessageW(g_peqType, CB_GETCURSEL, 0, 0);
                    if (type >= 0 && type <= static_cast<LRESULT>(PeqFilterType::Notch))
                    {
                        g_peqEditorBands[g_peqSelectedBand].type = static_cast<PeqFilterType>(type);
                        PeqBandsChanged(true);
                    }
                }
                break;
            case kPeqApplyValueId:
                PeqApplyExactValues(window);
                break;
            case kPeqFitViewId:
                PeqFitView();
                break;
            case kPeqGainZoomOutId:
                PeqZoomGainView(window, -1.0);
                break;
            case kPeqGainZoomInId:
                PeqZoomGainView(window, 1.0);
                break;
            case kPeqDeleteId:
                PeqDeleteSelected();
                break;
            case kPeqLiveApplyId:
                if (HIWORD(wParam) == BN_CLICKED &&
                    SendMessageW(g_peqLiveApply, BM_GETCHECK, 0, 0) == BST_CHECKED && g_peqEditorDirty)
                    CommitPeqEditor();
                break;
            }
            return 0;
        }
        case WM_CLOSE:
            if (g_peqEditorDirty)
            {
                const int answer = MessageBoxW(window, L"PEQ 还有未应用修改。关闭前保存并实时应用吗？",
                    L"HXAudio PEQ", MB_YESNOCANCEL | MB_ICONQUESTION);
                if (answer == IDCANCEL) return 0;
                if (answer == IDYES && !CommitPeqEditor()) return 0;
            }
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            DestroyUiSurfaceCache(window);
            g_peqWindow = nullptr;
            g_peqEnabled = nullptr;
            g_peqType = nullptr;
            g_peqFrequency = nullptr;
            g_peqGain = nullptr;
            g_peqQ = nullptr;
            g_peqLiveApply = nullptr;
            g_peqDevice = nullptr;
            g_peqGainZoomHover = 0;
            g_peqEditorBands.clear();
            g_peqEditorDevices.clear();
            g_peqEditorDeviceId.clear();
            g_peqEditorDeviceKey.clear();
            g_peqEditorDirty = false;
            g_peqSelectedBand = -1;
            g_peqDraggingBand = false;
            g_peqHoveredBand = -1;
            g_peqPointerInGraph = false;
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void OpenPeqEditor(HWND owner)
    {
        if (g_peqWindow)
        {
            ShowWindow(g_peqWindow, SW_RESTORE);
            SetForegroundWindow(g_peqWindow);
            return;
        }

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        constexpr wchar_t className[] = L"HXAudioPeqEditor";
        WNDCLASSEXW existing{};
        existing.cbSize = sizeof(existing);
        if (!GetClassInfoExW(instance, className, &existing))
        {
            WNDCLASSEXW cls{ sizeof(cls) };
            cls.style = CS_DBLCLKS;
            cls.lpfnWndProc = PeqEditorProc;
            cls.hInstance = instance;
            cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            cls.hIcon = g_uiIcons.empty() ? LoadIconW(nullptr, IDI_APPLICATION) : g_uiIcons.front();
            cls.hIconSm = g_uiIcons.size() > 1 ? g_uiIcons[1] : cls.hIcon;
            cls.hbrBackground = nullptr;
            cls.lpszClassName = className;
            RegisterClassExW(&cls);
        }
        const UINT dpi = owner ? UiDpi(owner) : GetDpiForSystem();
        g_peqWindow = CreateWindowExW(WS_EX_DLGMODALFRAME, className, L"HXAudio 参数均衡器（PEQ）",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1180, dpi, 96), MulDiv(760, dpi, 96),
            owner, nullptr, instance, nullptr);
        if (g_peqWindow)
        {
            SizeWindowForCurrentDpi(g_peqWindow, 1180, 760);
            if (!g_uiSmokeTest)
            {
                ShowWindow(g_peqWindow, SW_SHOW);
                SizeWindowForCurrentDpi(g_peqWindow, 1180, 760);
                PostMessageW(g_peqWindow, kApplyInitialDpiSizeMessage, 0, 0);
                UpdateWindow(g_peqWindow);
            }
        }
    }

    std::wstring HardwareIdentityDisplayText(
        const std::vector<std::wstring>& selected,
        const HardwareCapabilitySummary& summary)
    {
        std::wstring primaryId;
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> currentDefault;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
            SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia,
                &currentDefault)) && !IsVirtualName(DeviceName(currentDefault.Get())))
        {
            const std::wstring defaultId = DeviceId(currentDefault.Get());
            if (selected.empty() || std::find(selected.begin(), selected.end(),
                defaultId) != selected.end()) primaryId = defaultId;
        }
        if (primaryId.empty() && !selected.empty()) primaryId = selected.front();
        if (primaryId.empty() && !g_devices.empty()) primaryId = g_devices.front().id;

        const EndpointHardwareIdentity identity = QueryEndpointHardwareIdentity(primaryId);
        if (!identity.available)
            return L"功放芯片：Windows/驱动未公开独立硬件 ID | Codec：当前端点未提供可读 PnP 标识";

        std::wstring text = L"功放芯片：";
        text += summary.amplifierDevices != 0
            ? L"驱动公开硬件功放节点（独立芯片 ID 未公开）"
            : L"Windows/驱动未公开独立 PnP ID";
        text += L" | Codec：" + identity.chipModel;
        text += L" | 硬件：" + identity.hardwareVersion;
        text += L"\r\n驱动：";
        if (!identity.driverProvider.empty()) text += identity.driverProvider + L" ";
        text += identity.driverVersion.empty() ? L"版本未公开" : identity.driverVersion;
        text += L" | 功放节点 " + std::to_wstring(summary.amplifierDevices) + L"/" +
            std::to_wstring(summary.devices);
        if (selected.size() > 1)
            text += L" | 当前选择 " + std::to_wstring(selected.size()) + L" 台输出";
        return text;
    }

    void ProbeHardwareForSelection()
    {
        const auto selected = SelectedDeviceIds();
        EnableWindow(g_preampEdit, TRUE);
        if (g_hardwareStatus)
        {
            const std::wstring text = selected.empty()
                ? L"APO 音效配置按设备独立保存；请选择需要处理的输出设备。"
                : L"已选择 " + std::to_wstring(selected.size()) +
                    L" 台输出设备；PEQ 与音效配置按设备独立保存。";
            SetWindowTextIfChanged(g_hardwareStatus, text);
        }
        UpdateHardwareLabels();
    }

    void ApplyHardwareFromUi(HWND owner)
    {
        if (!SaveHardwareUiSettings(owner)) return;
        RestoreStaticHardwareCompensation();
        RestoreHardwareOriginals(true);
        SignalDspSettingsChanged();
        if (g_hardwareStatus)
        {
            SetWindowTextW(g_hardwareStatus, RouterIsReady()
                ? L"设置已应用"
                : L"设置已保存，等待音频设备连接");
        }
    }

    void RestoreHardwareFromUi(HWND owner)
    {
        SetWindowTextW(g_preampEdit, L"0");
        WriteRegistryDword(L"DspVirtualBassEnabled", 0);
        WriteRegistryDword(L"DspVirtualBassAmount", 0);
        WriteRegistryDword(L"DspVirtualBassCutoffHz", 80);
        WriteRegistryDword(L"DspVirtualBassSubsonicHz", 18);
        WriteRegistryDword(L"DspVirtualBassMode", 0);
        WriteRegistryString(L"DspVirtualBassCutoffHzText", L"80");
        WriteRegistryString(L"DspVirtualBassSubsonicHzText", L"18");
        WriteRegistryString(L"DspVirtualBassMultiplier1", L"2");
        WriteRegistryString(L"DspVirtualBassMultiplier2", L"3");
        WriteRegistryString(L"DspVirtualBassMultiplier3", L"4");
        SavePeqBands({});
        DeleteRegistryValuesWithPrefix(L"DspPeqBands_");
        DeleteRegistryValuesWithPrefix(L"DspPeqDeviceId_");
        if (g_peqWindow) ReloadPeqEditor();
        if (g_virtualBassWindow) ReloadVirtualBassEditor();
        UpdateHardwareLabels();
        if (!SaveHardwareUiSettings(owner)) return;
        RestoreStaticHardwareCompensation();
        RestoreHardwareOriginals(true);
        SignalDspSettingsChanged();
        if (g_hardwareStatus)
        {
            SetWindowTextW(g_hardwareStatus, L"虚拟低频已关闭，总增益归零，PEQ 频段已清空");
        }
    }

    struct RoutingApplyRequest
    {
        std::vector<std::wstring> selected;
        std::vector<std::wstring> profiles;
        DWORD bufferMs = 5;
        ULONGLONG generation = 0;
    };

    SRWLOCK g_routingApplyLock = SRWLOCK_INIT;
    RoutingApplyRequest g_routingApplyRequest;
    HANDLE g_routingApplyEvent = nullptr;
    HANDLE g_routingApplyStopEvent = nullptr;
    HANDLE g_routingApplyThread = nullptr;

    void ExecuteRoutingApply(const RoutingApplyRequest& request)
    {
        if (g_apoMode)
        {
            const std::vector<std::wstring> activeTargets =
                ReadRegistryMultiString(L"TargetEndpoints");
            const std::vector<std::wstring> activeProfiles =
                ReadRegistryMultiString(L"TargetEndpointProfiles");
            if (activeTargets == request.selected && activeProfiles == request.profiles)
                return;
            WriteRegistryMultiString(L"TargetEndpoints", request.selected);
            WriteRegistryMultiString(L"TargetEndpointProfiles", request.profiles);
            WriteRegistryDword(L"ApoEndpointSelectionSerial",
                ReadRegistryDword(L"ApoEndpointSelectionSerial", 0) + 1);
            if (RouterProcessIsRunning()) SignalRoutingSettingsChanged();
            else StartBackgroundProcess();
            return;
        }
        if (request.selected.empty()) return;

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> currentDefault;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr))
        {
            WriteRegistryDword(L"RouterUiApplyLastError", static_cast<DWORD>(hr));
            return;
        }

        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &currentDefault)))
        {
            const std::wstring currentId = DeviceId(currentDefault.Get());
            if (!IsVirtualName(DeviceName(currentDefault.Get()))) WriteRegistryString(L"OriginalDefaultEndpoint", currentId);
        }

        const std::vector<std::wstring> activeTargets = LoadTargetIds();
        const std::vector<std::wstring> activeProfiles = LoadTargetProfileKeys(activeTargets);
        const bool configurationChanged = activeTargets != request.selected ||
            activeProfiles != request.profiles ||
            ReadRegistryDword(L"BufferMs", 5) != request.bufferMs;

        WriteRegistryMultiString(L"TargetEndpoints", request.selected);
        WriteRegistryMultiString(L"TargetEndpointProfiles", request.profiles);
        WriteRegistryDword(L"BufferMs", request.bufferMs);
        WriteRegistryDword(L"RoutingEnabled", 1);
        SetAutoStart(true);

        std::wstring virtualId;
        hr = EnsureVirtualEndpointActive(enumerator.Get(), virtualId, 5000);
        if (FAILED(hr))
        {
            WriteRegistryDword(L"RouterUiApplyLastError", static_cast<DWORD>(hr));
            return;
        }

        bool routerRunning = RouterProcessIsRunning();
        if (routerRunning && (configurationChanged || !RouterIsReady()))
        {
            ResetRouterReadySignal();
            if (!SignalRoutingSettingsChanged())
            {
                // A pre-update background process does not own the new change
                // event. Replace it once, then all later changes are in-place.
                StopBackground();
                WaitForRouterStopped(10000);
                routerRunning = RouterProcessIsRunning();
            }
        }

        if (!routerRunning && !StartBackgroundProcess())
        {
            WriteRegistryDword(L"RouterUiApplyLastError",
                HRESULT_FROM_WIN32(GetLastError()));
            return;
        }

        const HRESULT defaultResult = AllDefaultRolesMatch(enumerator.Get(), virtualId)
            ? S_OK : SetDefaultEndpoint(virtualId);
        if (FAILED(defaultResult))
        {
            WriteRegistryDword(L"RouterUiApplyLastError",
                static_cast<DWORD>(defaultResult));
            return;
        }
        WriteRegistryDword(L"RouterUiApplyLastError", ERROR_SUCCESS);
    }

    DWORD WINAPI RoutingApplyWorker(void*)
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HANDLE events[] = { g_routingApplyStopEvent, g_routingApplyEvent };
        while (true)
        {
            const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(events),
                events, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0) break;
            if (wait != WAIT_OBJECT_0 + 1) break;
            while (true)
            {
                RoutingApplyRequest request;
                AcquireSRWLockShared(&g_routingApplyLock);
                request = g_routingApplyRequest;
                ReleaseSRWLockShared(&g_routingApplyLock);
                ExecuteRoutingApply(request);
                AcquireSRWLockShared(&g_routingApplyLock);
                const bool current = request.generation ==
                    g_routingApplyRequest.generation;
                ReleaseSRWLockShared(&g_routingApplyLock);
                if (current || WaitForSingleObject(
                    g_routingApplyStopEvent, 0) == WAIT_OBJECT_0) break;
            }
        }
        if (SUCCEEDED(co)) CoUninitialize();
        return ERROR_SUCCESS;
    }

    bool EnsureRoutingApplyWorker()
    {
        if (g_routingApplyThread) return true;
        g_routingApplyStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_routingApplyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_routingApplyStopEvent || !g_routingApplyEvent) return false;
        g_routingApplyThread = CreateThread(nullptr, 0,
            RoutingApplyWorker, nullptr, 0, nullptr);
        return g_routingApplyThread != nullptr;
    }

    void QueueSelectedRoutingApply()
    {
        RoutingApplyRequest request;
        request.selected = SelectedDeviceIds();
        if (!g_apoMode && request.selected.empty()) return;
        request.profiles.reserve(request.selected.size());
        for (const auto& id : request.selected)
            request.profiles.push_back(PhysicalDeviceInfo(id).profileKey);
        request.bufferMs = CurrentBufferMs();

        if (g_apoMode)
        {
            ExecuteRoutingApply(request);
            return;
        }

        AcquireSRWLockExclusive(&g_routingApplyLock);
        request.generation = g_routingApplyRequest.generation + 1;
        g_routingApplyRequest = std::move(request);
        ReleaseSRWLockExclusive(&g_routingApplyLock);
        if (EnsureRoutingApplyWorker()) SetEvent(g_routingApplyEvent);
        else WriteRegistryDword(L"RouterUiApplyLastError",
            HRESULT_FROM_WIN32(GetLastError()));
    }

    void QueueApoPlaybackApply()
    {
        if (!g_apoMode) return;
        const std::vector<std::wstring> selected =
            SelectedPlaybackDeviceIds();
        const std::vector<std::wstring> existing =
            ReadRegistryMultiString(L"PlaybackEndpoints");
        std::vector<std::wstring> ordered;
        const auto appendIfSelected = [&](const std::wstring& id)
        {
            if (std::find(selected.begin(), selected.end(), id) != selected.end() &&
                std::find(ordered.begin(), ordered.end(), id) == ordered.end())
                ordered.push_back(id);
        };
        for (const auto& id : existing) appendIfSelected(id);
        for (const auto& id : selected) appendIfSelected(id);

        std::vector<std::wstring> profiles;
        profiles.reserve(ordered.size());
        for (const auto& id : ordered)
            profiles.push_back(PhysicalDeviceInfo(id).profileKey);
        WriteRegistryMultiString(L"PlaybackEndpoints", ordered);
        WriteRegistryMultiString(L"PlaybackEndpointProfiles", profiles);
        WriteRegistryDword(L"ApoPlaybackEnabled", ordered.empty() ? 0 : 1);

        if (ordered.empty())
        {
            SetAutoStart(true);
            if (RouterProcessIsRunning()) SignalRoutingSettingsChanged();
            else StartBackgroundProcess();
            return;
        }

        SetAutoStart(true);
        if (RouterProcessIsRunning()) SignalRoutingSettingsChanged();
        else StartBackgroundProcess();
    }

    void ShutdownRoutingApplyWorker()
    {
        if (!g_routingApplyThread) return;
        SetEvent(g_routingApplyStopEvent);
        SetEvent(g_routingApplyEvent);
        const bool stopped = WaitForSingleObject(g_routingApplyThread, 250) == WAIT_OBJECT_0;
        CloseHandle(g_routingApplyThread);
        g_routingApplyThread = nullptr;
        if (stopped)
        {
            CloseHandle(g_routingApplyEvent);
            CloseHandle(g_routingApplyStopEvent);
            g_routingApplyEvent = nullptr;
            g_routingApplyStopEvent = nullptr;
        }
    }

    bool StopAndRestore(HWND owner, bool quiet)
    {
        // During an in-place Virtual -> APO switch, InstallationMode may already
        // say APO while the previous virtual router still owns the stop mutex.
        // Runtime ownership is authoritative; never strand that process merely
        // because the selected backend has changed.
        if (g_apoMode && !RouterProcessIsRunning())
        {
            RestoreStaticHardwareCompensation();
            RestoreHardwareOriginals(false);
            if (g_status) UpdateRouterStatusLabel();
            return true;
        }
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        const std::wstring restoreId = SUCCEEDED(hr) ? ResolveRestoreEndpoint(enumerator.Get()) : std::wstring();
        if (!restoreId.empty()) WriteRegistryString(L"OriginalDefaultEndpoint", restoreId);

        // Stop the owner of the default-endpoint invariant first. Restoring the
        // physical endpoint before the background process exits allowed a
        // concurrently rebuilding session to switch the default back again.
        WriteRegistryDword(L"RoutingEnabled", 0);
        SetAutoStart(false);
        StopBackground();
        WaitForRouterStopped(10000);
        RestoreStaticHardwareCompensation();
        RestoreHardwareOriginals(false);

        hr = restoreId.empty() ? HRESULT_FROM_WIN32(ERROR_NOT_FOUND) : SetDefaultEndpoint(restoreId);

        bool restored = false;
        if (SUCCEEDED(hr))
        {
            for (int attempt = 0; attempt < 40; ++attempt)
            {
                if (AllDefaultRolesMatch(enumerator.Get(), restoreId))
                {
                    restored = true;
                    break;
                }
                Sleep(50);
            }
        }

        if (!quiet && !restored)
        {
            MessageBoxW(owner, L"路由已停止，但原输出设备目前不可用。请在 Windows 声音设置中选择输出。", L"HXAudio", MB_OK | MB_ICONINFORMATION);
            ShellExecuteW(owner, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (g_status)
        {
            SetWindowTextW(g_status, restored
                ? L"状态：原输出已恢复"
                : L"状态：已停止，但 Windows 未确认默认输出恢复");
        }
        return restored;
    }

    void ApplyDefaultFont(HWND parent)
    {
        ModernizeChildren(parent);
    }

    struct MainChildLayout
    {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    struct MainLayoutContext
    {
        HWND parent = nullptr;
        UINT dpi = 96;
        int widthDelta = 0;
        int expandableShift = 0;
        int flowShift = 0;
        HDWP* defer = nullptr;
    };

    void CaptureMainChildLayouts(HWND window)
    {
        EnumChildWindows(window, [](HWND child, LPARAM value) -> BOOL
        {
            HWND parent = reinterpret_cast<HWND>(value);
            if (GetParent(child) != parent ||
                GetPropW(child, kUiMainLayoutProperty)) return TRUE;
            RECT rectangle{};
            GetWindowRect(child, &rectangle);
            MapWindowPoints(nullptr, parent,
                reinterpret_cast<POINT*>(&rectangle), 2);
            const UINT childDpi = UiDpi(parent);
            auto* layout = new MainChildLayout{
                MulDiv(rectangle.left, 96, childDpi),
                MulDiv(rectangle.top, 96, childDpi),
                MulDiv(rectangle.right - rectangle.left, 96, childDpi),
                MulDiv(rectangle.bottom - rectangle.top, 96, childDpi) };
            SetPropW(child, kUiMainLayoutProperty, layout);
            return TRUE;
        }, reinterpret_cast<LPARAM>(window));
    }

    void ReleaseMainChildLayouts(HWND window)
    {
        EnumChildWindows(window, [](HWND child, LPARAM value) -> BOOL
        {
            if (GetParent(child) != reinterpret_cast<HWND>(value)) return TRUE;
            auto* layout = reinterpret_cast<MainChildLayout*>(
                RemovePropW(child, kUiMainLayoutProperty));
            delete layout;
            return TRUE;
        }, reinterpret_cast<LPARAM>(window));
    }

    int MainExpandableLayoutShift(HWND window)
    {
        RECT client{};
        GetClientRect(window, &client);
        const int logicalHeight = MulDiv(client.bottom, 96,
            static_cast<int>(UiDpi(window)));
        // The base geometry already contains every row. Extra client height is
        // assigned to the device mixer and then carried through the remaining
        // cards as one continuous vertical flow.
        const int baseClientHeight = g_apoMode ? 754 : 806;
        return std::max(0, logicalHeight - baseClientHeight);
    }

    int MainFlowLayoutShift(HWND window)
    {
        // APO has no real-time-buffer row. Close that space uniformly instead
        // of leaving the old controls hidden while their layout gap remains.
        return MainExpandableLayoutShift(window) - (g_apoMode ? 52 : 0);
    }

    int MainWindowLogicalHeight()
    {
        return g_apoMode ? 834 : 886;
    }

    void LayoutMainWindow(HWND window)
    {
        RECT client{};
        GetClientRect(window, &client);
        const UINT dpi = UiDpi(window);
        const int logicalWidth = MulDiv(client.right, 96,
            static_cast<int>(dpi));
        const int widthDelta = logicalWidth - 904;
        const int expandableShift = MainExpandableLayoutShift(window);
        const int flowShift = MainFlowLayoutShift(window);
        HDWP defer = BeginDeferWindowPos(32);
        MainLayoutContext context{ window, dpi, widthDelta,
            expandableShift, flowShift, &defer };
        EnumChildWindows(window, [](HWND child, LPARAM value) -> BOOL
        {
            auto* context = reinterpret_cast<MainLayoutContext*>(value);
            if (GetParent(child) != context->parent) return TRUE;
            const auto* base = reinterpret_cast<const MainChildLayout*>(
                GetPropW(child, kUiMainLayoutProperty));
            if (!base) return TRUE;
            int x = base->x;
            int y = base->y;
            int width = base->width;
            int height = base->height;
            if (child == g_deviceList)
            {
                width = std::max(320, base->width + context->widthDelta);
                height = std::max(126,
                    base->height + context->expandableShift);
            }
            else
            {
                if (base->y >= 318) y += context->flowShift;
                const bool stretch = child == g_bufferSlider ||
                    child == g_hardwareStatus || child == g_peqSummary ||
                    child == g_virtualBassSummary ||
                    (base->y == 154 && base->x == 44);
                if (stretch)
                    width = std::max(100, base->width + context->widthDelta);
                const int id = GetDlgCtrlID(child);
                const bool anchorRight = base->x >= 480 ||
                    child == g_bufferValue || id == kRefreshButtonId ||
                    id == kAboutButtonId;
                if (anchorRight) x += context->widthDelta;
            }
            const int scaledX = MulDiv(x, context->dpi, 96);
            const int scaledY = MulDiv(y, context->dpi, 96);
            const int scaledWidth = MulDiv(width, context->dpi, 96);
            const int scaledHeight = MulDiv(height, context->dpi, 96);
            if (*context->defer)
                *context->defer = DeferWindowPos(*context->defer, child,
                    nullptr, scaledX, scaledY, scaledWidth, scaledHeight,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
        if (defer) EndDeferWindowPos(defer);
        DestroyUiSurfaceCache(window);
        InvalidateRect(window, nullptr, FALSE);
    }

    void PaintMainWindow(HWND window, HDC destination)
    {
        RECT client{};
        GetClientRect(window, &client);
        const int width = std::max<LONG>(client.right, 1);
        const int height = std::max<LONG>(client.bottom, 1);
        UiSurfaceCache* cache = EnsureUiSurfaceCache(window, destination, width, height);
        if (!cache) return;
        HDC canvas = cache->canvas;
        void* pixels = cache->pixels;
        RECT update{};
        if (!UiPaintRegion(destination, client, cache->valid, update)) return;
        const int savedDc = SaveDC(canvas);
        IntersectClipRect(canvas, update.left, update.top, update.right, update.bottom);
        PrepareUiCanvasRegion(pixels, width, height, canvas, update);

        Gdiplus::Graphics graphics(canvas);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const float scale = static_cast<float>(UiDpi(window)) / 96.0f;
        const float logicalWidth = static_cast<float>(client.right) / scale;
        const int expandableShift = MainExpandableLayoutShift(window);
        const int flowShift = MainFlowLayoutShift(window);
        const float cardRight = std::max(736.0f, logicalWidth - 24.0f);
        const auto rectangle = [&](float left, float top, float right, float bottom)
        {
            return Gdiplus::RectF(left * scale, top * scale,
                (right - left) * scale, (bottom - top) * scale);
        };
        const auto card = [&](const Gdiplus::RectF& bounds)
        {
            Gdiplus::RectF shadow = bounds;
            shadow.Y += 2.0f * scale;
            FillRounded(graphics, shadow, 12.0f * scale,
                Gdiplus::Color(g_ui.dark ? 48 : 18, 0, 0, 0));
            FillRounded(graphics, bounds, 12.0f * scale, GdiColor(g_ui.surface));
            StrokeRounded(graphics, bounds, 12.0f * scale, GdiColor(g_ui.border), 1.0f);
        };

        const float expandableOffset = static_cast<float>(expandableShift);
        const float flowOffset = static_cast<float>(flowShift);
        card(rectangle(24.0f, 104.0f, cardRight,
            (g_apoMode ? 372.0f + expandableOffset :
                418.0f + flowOffset)));
        const float dspTop = (g_apoMode ? 434.0f : 432.0f) + flowOffset;
        card(rectangle(24.0f, dspTop, cardRight,
            682.0f + flowOffset));
        card(rectangle(24.0f, 706.0f + flowOffset, cardRight,
            804.0f + flowOffset));
        FillRounded(graphics, rectangle(42.0f, 510.0f + flowOffset,
            cardRight - 18.0f, 552.0f + flowOffset), 8.0f * scale,
            GdiColor(g_ui.surfaceAlt));
        FillRounded(graphics, rectangle(42.0f, 556.0f + flowOffset,
            cardRight - 18.0f, 602.0f + flowOffset), 8.0f * scale,
            GdiColor(g_ui.surfaceAlt));
        FillRounded(graphics, rectangle(42.0f, 614.0f + flowOffset,
            cardRight - 18.0f, 668.0f + flowOffset), 8.0f * scale,
            GdiColor(g_ui.surfaceAlt));

        const Gdiplus::RectF accentBar = rectangle(24, 122, 29, 170);
        FillRounded(graphics, accentBar, 2.5f * scale, GdiColor(g_ui.accent));
        const Gdiplus::RectF dspBar = rectangle(24.0f,
            dspTop + 18.0f, 29.0f, dspTop + 66.0f);
        FillRounded(graphics, dspBar, 2.5f * scale, GdiColor(g_ui.accent));

        if (!g_uiIcons.empty())
            DrawIconEx(canvas, UiScale(window, 24), UiScale(window, 28), g_uiIcons.front(),
                UiScale(window, 42), UiScale(window, 42), 0, nullptr, DI_NORMAL);
        DrawUiText(graphics, canvas, window, L"HXAudio Desktop",
            rectangle(76, 27, 620, 71),
            UiFontRole::Title, g_ui.text);

        graphics.Flush(Gdiplus::FlushIntentionSync);
        FinalizeUiCanvasAlphaRegion(pixels, width, height, update);
        cache->valid = true;
        RestoreDC(canvas, savedDc);
        BitBlt(destination, update.left, update.top,
            update.right - update.left, update.bottom - update.top,
            canvas, update.left, update.top, SRCCOPY);
    }

    void PaintAboutWindow(HWND window, HDC destination)
    {
        RECT client{};
        GetClientRect(window, &client);
        const int width = std::max<LONG>(client.right, 1);
        const int height = std::max<LONG>(client.bottom, 1);
        UiSurfaceCache* cache = EnsureUiSurfaceCache(window,
            destination, width, height);
        if (!cache) return;
        PrepareUiCanvas(cache->pixels, width, height, cache->canvas, client);
        Gdiplus::Graphics graphics(cache->canvas);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const float scale = static_cast<float>(UiDpi(window)) / 96.0f;
        const auto rectangle = [&](float left, float top, float right, float bottom)
        {
            return Gdiplus::RectF(left * scale, top * scale,
                (right - left) * scale, (bottom - top) * scale);
        };

        FillRounded(graphics, rectangle(22, 82, 438, 218), 12.0f * scale,
            GdiColor(g_ui.surface));
        StrokeRounded(graphics, rectangle(22, 82, 438, 218), 12.0f * scale,
            GdiColor(g_ui.border), 1.0f);
        if (!g_uiIcons.empty())
            DrawIconEx(cache->canvas, UiScale(window, 24), UiScale(window, 20),
                g_uiIcons.front(), UiScale(window, 48), UiScale(window, 48),
                0, nullptr, DI_NORMAL);
        DrawUiText(graphics, cache->canvas, window, L"HXAudio Desktop",
            rectangle(88, 16, 420, 58), UiFontRole::Title, g_ui.text);

        const struct { const wchar_t* label; const wchar_t* value; float top; } rows[] = {
            { L"版本", L"1.0.1b（内测）", 92.0f },
            { L"作者", L"Huber_HaYu", 132.0f },
            { L"工作室", L"Huber Lab", 172.0f }
        };
        for (const auto& row : rows)
        {
            DrawUiText(graphics, cache->canvas, window, row.label,
                rectangle(44, row.top, 132, row.top + 30),
                UiFontRole::Caption, g_ui.muted);
            DrawUiText(graphics, cache->canvas, window, row.value,
                rectangle(142, row.top, 416, row.top + 30),
                UiFontRole::Body, g_ui.text);
        }

        graphics.Flush(Gdiplus::FlushIntentionSync);
        FinalizeUiCanvasAlpha(cache->pixels, width, height);
        cache->valid = true;
        BitBlt(destination, 0, 0, width, height,
            cache->canvas, 0, 0, SRCCOPY);
    }

    LRESULT CALLBACK AboutWindowProc(HWND window, UINT message,
        WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
        {
            ConfigureModernWindow(window);
            CreateWindowW(L"BUTTON", L"关闭",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                328, 228, 108, 36, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(
                    kAboutCloseButtonId)), nullptr, nullptr);
            ScaleChildrenForDpi(window, 96, UiDpi(window));
            ApplyDefaultFont(window);
            ModernizeChildren(window);
            return 0;
        }
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            PaintAboutWindow(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintAboutWindow(window, reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DRAWITEM:
            if (DrawModernButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam)))
                return TRUE;
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return ControlColor(message, wParam, lParam);
        case WM_COMMAND:
            if (LOWORD(wParam) == kAboutCloseButtonId)
            {
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_DPICHANGED:
            ApplyDpiChange(window, wParam, lParam);
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            RefreshUiPalette();
            ConfigureModernWindow(window);
            ModernizeChildren(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_DESTROY:
            DestroyUiSurfaceCache(window);
            g_aboutWindow = nullptr;
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void ShowAbout(HWND owner)
    {
        if (g_aboutWindow && IsWindow(g_aboutWindow))
        {
            ShowWindow(g_aboutWindow, SW_RESTORE);
            SetForegroundWindow(g_aboutWindow);
            return;
        }
        WNDCLASSEXW cls{ sizeof(cls) };
        cls.lpfnWndProc = AboutWindowProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hIcon = !g_uiIcons.empty() ? g_uiIcons.front() : nullptr;
        cls.hIconSm = cls.hIcon;
        cls.hbrBackground = nullptr;
        cls.lpszClassName = kAboutWindowClass;
        RegisterClassExW(&cls);
        g_aboutWindow = CreateWindowExW(WS_EX_TOOLWINDOW,
            kAboutWindowClass, L"关于 HXAudio Desktop",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, UiScale(owner, 460),
            UiScale(owner, 310), owner, nullptr, cls.hInstance, nullptr);
        if (!g_aboutWindow) return;
        SizeWindowForCurrentDpi(g_aboutWindow, 460, 310);
        RECT ownerRect{};
        RECT aboutRect{};
        GetWindowRect(owner, &ownerRect);
        GetWindowRect(g_aboutWindow, &aboutRect);
        const int x = ownerRect.left +
            ((ownerRect.right - ownerRect.left) -
                (aboutRect.right - aboutRect.left)) / 2;
        const int y = ownerRect.top +
            ((ownerRect.bottom - ownerRect.top) -
                (aboutRect.bottom - aboutRect.top)) / 2;
        SetWindowPos(g_aboutWindow, HWND_TOP, x, y, 0, 0,
            SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(g_aboutWindow);
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (g_taskbarCreatedMessage != 0 && message == g_taskbarCreatedMessage)
        {
            AddTrayIcon(window);
            return 0;
        }
        switch (message)
        {
        case WM_CREATE:
        {
            ConfigureModernWindow(window);
            HWND title = CreateWindowW(L"STATIC", L"HXAudio Desktop",
                WS_CHILD | WS_VISIBLE, 76, 22, 350, 38, window, nullptr, nullptr, nullptr);
            g_status = nullptr;

            HWND deviceSection = CreateWindowW(L"STATIC", L"输出设备",
                WS_CHILD | WS_VISIBLE, 44, 122, 220, 28, window, nullptr, nullptr, nullptr);
            CreateWindowW(L"STATIC",
                L"选择要处理的设备，单击“输出”进行开启/关闭，可多选",
                WS_CHILD | WS_VISIBLE, 44, 154, 590, 22, window, nullptr, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"刷新", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                704, 122, 74, 34, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshButtonId)), nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"关于", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                788, 122, 74, 34, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAboutButtonId)), nullptr, nullptr);
            g_deviceList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTBOXW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_MULTIPLESEL |
                LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                44, 184, 818, 162, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDeviceListId)), nullptr, nullptr);
            if (!g_apoMode)
            {
                CreateWindowW(L"STATIC", L"实时缓冲",
                    WS_CHILD | WS_VISIBLE, 44, 365, 88, 22, window,
                    nullptr, nullptr, nullptr);
                g_bufferSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS,
                    136, 354, 450, 42, window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(
                        kBufferSliderId)), nullptr, nullptr);
                SendMessageW(g_bufferSlider, TBM_SETRANGE, TRUE,
                    MAKELPARAM(0, ARRAYSIZE(kBufferChoices) - 1));
                SendMessageW(g_bufferSlider, TBM_SETTICFREQ, 1, 0);
                const DWORD savedBuffer = ReadRegistryDword(L"BufferMs", 5);
                size_t savedIndex = 1;
                for (size_t i = 0; i < ARRAYSIZE(kBufferChoices); ++i)
                {
                    if (kBufferChoices[i] == savedBuffer) savedIndex = i;
                }
                SendMessageW(g_bufferSlider, TBM_SETPOS, TRUE, savedIndex);
                g_bufferValue = CreateWindowW(L"STATIC", L"",
                    WS_CHILD | WS_VISIBLE, 602, 365, 250, 22, window,
                    nullptr, nullptr, nullptr);
            }

            HWND dspSection = CreateWindowW(L"STATIC", L"声音处理",
                WS_CHILD | WS_VISIBLE, 44, 450, 220, 28, window, nullptr, nullptr, nullptr);
            g_hardwareStatus = CreateWindowW(L"STATIC", L"正在读取设备能力",
                WS_CHILD | WS_VISIBLE, 44, 485, 812, 24, window, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC", L"总增益", WS_CHILD | WS_VISIBLE,
                44, 523, 82, 22, window, nullptr, nullptr, nullptr);
            const DspSettings initialDsp = LoadDspSettings();
            g_preampEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", FormatParameter(initialDsp.preampDb).c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                130, 515, 140, 30, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreampEditId)), nullptr, nullptr);
            CreateWindowW(L"STATIC", L"dB（±30）",
                WS_CHILD | WS_VISIBLE, 286, 523, 430, 22, window,
                nullptr, nullptr, nullptr);

            g_peqSummary = CreateWindowW(L"STATIC", L"PEQ：读取中", WS_CHILD | WS_VISIBLE,
                58, 566, 590, 24, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPeqSummaryId)), nullptr, nullptr);
            SetPropW(g_peqSummary, kUiSurfaceAltProperty, reinterpret_cast<HANDLE>(1));
            CreateWindowW(L"BUTTON", L"编辑 PEQ 曲线", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                688, 559, 158, 36, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPeqEditorButtonId)), nullptr, nullptr);

            g_virtualBassMasterEnable = CreateWindowW(L"BUTTON", L"启用虚拟低频",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                58, 627, 160, 28, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVirtualBassMasterEnableId)), nullptr, nullptr);
            SetPropW(g_virtualBassMasterEnable, kUiToggleSwitchProperty,
                reinterpret_cast<HANDLE>(1));
            SetPropW(g_virtualBassMasterEnable, kUiSurfaceAltProperty,
                reinterpret_cast<HANDLE>(1));
            g_virtualBassSummary = CreateWindowW(L"STATIC", L"虚拟低频：读取中", WS_CHILD | WS_VISIBLE,
                228, 626, 436, 32, window, nullptr, nullptr, nullptr);
            SetPropW(g_virtualBassSummary, kUiSurfaceAltProperty, reinterpret_cast<HANDLE>(1));
            CreateWindowW(L"BUTTON", L"编辑虚拟低频", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                688, 623, 158, 36, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVirtualBassEditorButtonId)), nullptr, nullptr);

            CreateWindowW(L"BUTTON", L"刷新设备能力", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                42, 752, 148, 36, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHardwareProbeButtonId)), nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"应用声音设置", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                636, 752, 210, 36, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHardwareApplyButtonId)), nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"恢复默认", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                480, 752, 140, 36, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHardwareRestoreButtonId)), nullptr, nullptr);

            ScaleChildrenForDpi(window, 96, UiDpi(window));
            ApplyDefaultFont(window);
            CaptureMainChildLayouts(window);
            LayoutMainWindow(window);
            SetUiFontRole(title, UiFontRole::Title);
            ShowWindow(title, SW_HIDE);
            SetUiFontRole(deviceSection, UiFontRole::Section);
            SetUiFontRole(dspSection, UiFontRole::Section);
            UpdateBufferLabel();
            RefreshDevices();
            ProbeHardwareForSelection();
            if (!g_uiSmokeTest && !g_uiPreview && !g_mainUiPreview &&
                !g_mainUiSmoke &&
                !g_virtualBassUiSmoke && !g_virtualBassUiPreview)
            {
                QueueSelectedRoutingApply();
                QueueApoPlaybackApply();
            }
            AddTrayIcon(window);
            SetTimer(window, kStatusTimerId, 2000, nullptr);
            return 0;
        }
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            PaintMainWindow(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintMainWindow(window, reinterpret_cast<HDC>(wParam));
            return 0;
        case kApplyInitialDpiSizeMessage:
            SizeWindowForCurrentDpi(window, 980, MainWindowLogicalHeight());
            return 0;
        case kShowControlWindowMessage:
            RestoreControlWindow(window);
            return 0;
        case kExitControlWindowMessage:
            DestroyWindow(window);
            return 0;
        case kTrayCallbackMessage:
        {
            const UINT event = LOWORD(lParam);
            if (event == NIN_SELECT || event == NIN_KEYSELECT ||
                event == WM_LBUTTONDBLCLK || event == WM_LBUTTONUP)
            {
                RestoreControlWindow(window);
            }
            else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP)
            {
                POINT point{ static_cast<short>(LOWORD(wParam)),
                    static_cast<short>(HIWORD(wParam)) };
                if (point.x == 0 && point.y == 0) GetCursorPos(&point);
                ShowTrayMenu(window, point);
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DRAWITEM:
            if (DrawModernButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam)) ||
                DrawModernChoiceItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam))) return TRUE;
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
            return ControlColor(message, wParam, lParam);
        case WM_DPICHANGED:
            ApplyDpiChange(window, wParam, lParam);
            LayoutMainWindow(window);
            RefreshTrayIconImage(window);
            return 0;
        case WM_GETMINMAXINFO:
        {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize.x = UiScale(window, 760);
            limits->ptMinTrackSize.y = UiScale(window,
                MainWindowLogicalHeight());
            return 0;
        }
        case WM_SIZE:
        {
            RECT outer{};
            GetWindowRect(window, &outer);
            const UINT dpi = UiDpi(window);
            SetPropW(window, kUiLogicalWidthProperty,
                reinterpret_cast<HANDLE>(static_cast<INT_PTR>(MulDiv(
                    outer.right - outer.left, 96, static_cast<int>(dpi)))));
            SetPropW(window, kUiLogicalHeightProperty,
                reinterpret_cast<HANDLE>(static_cast<INT_PTR>(MulDiv(
                    outer.bottom - outer.top, 96, static_cast<int>(dpi)))));
            if (GetPropW(g_deviceList, kUiMainLayoutProperty))
                LayoutMainWindow(window);
            return 0;
        }
        case WM_CLOSE:
            if (TrayUiEnabled())
            {
                ShowWindow(window, SW_HIDE);
                return 0;
            }
            DestroyWindow(window);
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            RefreshUiPalette();
            ConfigureModernWindow(window);
            ModernizeChildren(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_TIMER:
            if (wParam == kStatusTimerId)
            {
                UpdateRouterStatusLabel();
                ProbeHardwareForSelection();
            }
            return 0;
        case WM_HSCROLL:
            if (g_bufferSlider &&
                reinterpret_cast<HWND>(lParam) == g_bufferSlider)
            {
                UpdateBufferLabel();
                if (!g_apoMode && LOWORD(wParam) == TB_ENDTRACK)
                    QueueSelectedRoutingApply();
            }
            else UpdateHardwareLabels();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case kRefreshButtonId:
                RefreshDevices();
                ProbeHardwareForSelection();
                QueueSelectedRoutingApply();
                QueueApoPlaybackApply();
                break;
            case kAboutButtonId: ShowAbout(window); break;
            case kTrayOpenButtonId: RestoreControlWindow(window); break;
            case kTrayExitButtonId: DestroyWindow(window); break;
            case kHardwareProbeButtonId: ProbeHardwareForSelection(); break;
            case kHardwareApplyButtonId: ApplyHardwareFromUi(window); break;
            case kHardwareRestoreButtonId: RestoreHardwareFromUi(window); break;
            case kPeqEditorButtonId: OpenPeqEditor(window); break;
            case kVirtualBassEditorButtonId: OpenVirtualBassEditor(window); break;
            case kVirtualBassMasterEnableId:
                if (HIWORD(wParam) == BN_CLICKED)
                {
                    SetVirtualBassMasterEnabled(
                        SendMessageW(g_virtualBassMasterEnable, BM_GETCHECK, 0, 0) == BST_CHECKED);
                }
                break;
            case kDeviceListId:
                if (HIWORD(wParam) == LBN_SELCHANGE)
                {
                    if (!g_refreshingDeviceList && !g_mainUiSmoke)
                        QueueSelectedRoutingApply();
                    if (!g_apoMode) ProbeHardwareForSelection();
                    UpdatePeqSummary();
                    if (g_peqWindow && !g_peqEditorDirty) ReloadPeqDeviceChoices();
                }
                break;
            }
            return 0;
        case WM_DESTROY:
            RemoveTrayIcon();
            ShutdownEndpointVolumeWorker();
            ShutdownRoutingApplyWorker();
            DestroyUiSurfaceCache(window);
            ReleaseMainChildLayouts(window);
            KillTimer(window, kStatusTimerId);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    int RequestControlPanelShutdown()
    {
        std::vector<DWORD> processIds;
        HWND previous = nullptr;
        while ((previous = FindWindowExW(nullptr, previous,
            kControlWindowClass, nullptr)) != nullptr)
        {
            DWORD processId = 0;
            GetWindowThreadProcessId(previous, &processId);
            if (processId != 0 && processId != GetCurrentProcessId() &&
                std::find(processIds.begin(), processIds.end(), processId) ==
                    processIds.end())
                processIds.push_back(processId);
            PostMessageW(previous, kExitControlWindowMessage, 0, 0);
            // Previous releases do not understand the explicit tray-exit
            // message. Queue the legacy close behind it: the new build exits
            // on the first message, while an older build closes on WM_CLOSE.
            PostMessageW(previous, WM_CLOSE, 0, 0);
        }

        const ULONGLONG deadline = GetTickCount64() + 15000;
        for (const DWORD processId : processIds)
        {
            HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
            if (!process) continue;
            const ULONGLONG now = GetTickCount64();
            const DWORD remaining = now < deadline
                ? static_cast<DWORD>(deadline - now) : 0;
            const DWORD wait = WaitForSingleObject(process, remaining);
            CloseHandle(process);
            if (wait != WAIT_OBJECT_0) return 1;
        }
        return FindWindowW(kControlWindowClass, nullptr) == nullptr ? 0 : 1;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int show)
{
    UiRuntime uiRuntime;
    const std::wstring arguments = commandLine ? commandLine : L"";
    if (arguments.find(L"--shutdown-ui") != std::wstring::npos)
        return RequestControlPanelShutdown();
    const bool background = arguments.find(L"--background") != std::wstring::npos;
    const bool repairEscapedSessions =
        arguments.find(L"--repair-escaped-sessions") != std::wstring::npos;
    g_uiSmokeTest = arguments.find(L"--peq-ui-smoke") != std::wstring::npos;
    g_uiPreview = arguments.find(L"--peq-ui-preview") != std::wstring::npos;
    g_mainUiPreview = arguments.find(L"--main-ui-preview") != std::wstring::npos;
    g_mainUiSmoke = arguments.find(L"--main-ui-smoke") != std::wstring::npos;
    g_virtualBassUiSmoke = arguments.find(L"--virtual-bass-ui-smoke") != std::wstring::npos;
    g_virtualBassUiPreview = arguments.find(L"--virtual-bass-ui-preview") != std::wstring::npos;
    g_forceDarkUiPreview = arguments.find(L"--ui-force-dark") != std::wstring::npos;
    if (g_forceDarkUiPreview) RefreshUiPalette();
    const HRESULT co = CoInitializeEx(nullptr,
        background || repairEscapedSessions
            ? COINIT_MULTITHREADED : COINIT_APARTMENTTHREADED);
    g_cpu = DetectCpuTopology();
    g_apoMode = ApoModeIsInstalled();

    if (arguments.find(L"--sync-apo-settings") != std::wstring::npos)
    {
        const int result = SyncApoDspSettings();
        if (g_apoMode && result == 0)
        {
            ApplyStaticHardwareCompensation(StaticGainApplyMode::All);
            SetAutoStart(true);
            if (!RouterProcessIsRunning()) StartBackgroundProcess();
            else
            {
                SignalRoutingSettingsChanged();
                HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE,
                    kDspSettingsEvent);
                if (event)
                {
                    SetEvent(event);
                    CloseHandle(event);
                }
            }
        }
        if (SUCCEEDED(co)) CoUninitialize();
        return result;
    }

    if (arguments.find(L"--dsp-self-test") != std::wstring::npos)
    {
        const int result = RunDspSelfTest();
        if (SUCCEEDED(co)) CoUninitialize();
        return result;
    }

    if (arguments.find(L"--router-guard-self-test") != std::wstring::npos)
    {
        const int result = RunRouterGuardSelfTest();
        if (SUCCEEDED(co)) CoUninitialize();
        return result;
    }

    if (repairEscapedSessions)
    {
        // Session ownership belongs exclusively to the long-lived router. A
        // transient helper must never acquire and immediately abandon an app
        // policy, which was the original source of route oscillation.
        const HRESULT result = RouterProcessIsRunning()
            ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_READY);
        WriteRegistryDword(L"RouterManualSessionRepairCount", 0);
        if (SUCCEEDED(co)) CoUninitialize();
        return FAILED(result) ? static_cast<int>(result) : 0;
    }

    if (background)
    {
        if (g_apoMode)
        {
            NormalizeStoredPeqGainLimits();
            const HRESULT result = RunApoPlayback();
            if (SUCCEEDED(co)) CoUninitialize();
            return static_cast<int>(result);
        }
        const HRESULT result = RunRouter();
        if (SUCCEEDED(co)) CoUninitialize();
        return static_cast<int>(result);
    }
    if (arguments.find(L"--stop") != std::wstring::npos)
    {
        const bool restored = StopAndRestore(nullptr, true);
        if (SUCCEEDED(co)) CoUninitialize();
        return restored ? 0 : 10;
    }
    if (arguments.find(L"--install-or-resume") != std::wstring::npos ||
        arguments.find(L"--resume-after-upgrade") != std::wstring::npos ||
        arguments.find(L"--repair-routing") != std::wstring::npos)
    {
        if (g_apoMode)
        {
            const int result = SyncApoDspSettings();
            if (result == 0)
            {
                ApplyStaticHardwareCompensation(StaticGainApplyMode::All);
                SetAutoStart(true);
                if (!RouterProcessIsRunning()) StartBackgroundProcess();
            }
            if (SUCCEEDED(co)) CoUninitialize();
            return result;
        }
        const bool force = arguments.find(L"--install-or-resume") != std::wstring::npos ||
            arguments.find(L"--repair-routing") != std::wstring::npos;
        const HRESULT result = ResumeSavedRouting(force);
        if (SUCCEEDED(co)) CoUninitialize();
        return static_cast<int>(result);
    }

    NormalizeStoredPeqGainLimits();

    if (!g_apoMode && !g_uiSmokeTest && !g_uiPreview && !g_mainUiPreview &&
        !g_mainUiSmoke &&
        !g_virtualBassUiSmoke && !g_virtualBassUiPreview &&
        ReadRegistryDword(L"RoutingEnabled", 0) != 0 && !RouterProcessIsRunning())
    {
        StartBackgroundProcess();
    }
    if (g_apoMode && !g_uiSmokeTest && !g_uiPreview && !g_mainUiPreview &&
        !g_mainUiSmoke && !g_virtualBassUiSmoke &&
        !g_virtualBassUiPreview)
    {
        ApplyStaticHardwareCompensation(StaticGainApplyMode::All);
        SetAutoStart(true);
        if (!RouterProcessIsRunning()) StartBackgroundProcess();
    }

    if (TrayUiEnabled())
    {
        HWND existing = FindWindowW(kControlWindowClass, nullptr);
        if (existing)
        {
            PostMessageW(existing, kShowControlWindowMessage, 0, 0);
            if (SUCCEEDED(co)) CoUninitialize();
            return 0;
        }
        g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    }

    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&controls);
    HICON largeIcon = CreateHxAudioIcon(48);
    HICON smallIcon = CreateHxAudioIcon(24);
    WNDCLASSEXW cls{ sizeof(cls) };
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = largeIcon ? largeIcon : LoadIconW(nullptr, IDI_APPLICATION);
    cls.hIconSm = smallIcon ? smallIcon : cls.hIcon;
    cls.hbrBackground = nullptr;
    cls.lpszClassName = kControlWindowClass;
    RegisterClassExW(&cls);
    const UINT systemDpi = GetDpiForSystem();
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"HXAudio Desktop",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
        WS_MAXIMIZEBOX | WS_THICKFRAME | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(980, systemDpi, 96),
        MulDiv(MainWindowLogicalHeight(), systemDpi, 96),
        nullptr, nullptr, instance, nullptr);
    SizeWindowForCurrentDpi(window, 980, MainWindowLogicalHeight());
    if (g_mainUiSmoke)
    {
        const bool bufferUiValid = g_apoMode
            ? g_bufferSlider == nullptr && g_bufferValue == nullptr
            : g_bufferSlider != nullptr && g_bufferValue != nullptr;
        bool valid = window && g_status == nullptr && g_deviceList &&
            g_virtualBassMasterEnable && bufferUiValid;
        if (valid)
        {
            ShowWindow(window, SW_SHOWNOACTIVATE);
            UpdateWindow(window);
            const LONG_PTR listExStyle = GetWindowLongPtrW(g_deviceList, GWL_EXSTYLE);
            const HWND refresh = GetDlgItem(window, kRefreshButtonId);
            DWORD_PTR listSubclassData = 0;
            const bool customListPainter = GetWindowSubclass(g_deviceList,
                ModernMixerSubclass, 16, &listSubclassData) != FALSE;
            const bool modernMixer = customListPainter &&
                (listExStyle & WS_EX_CLIENTEDGE) == 0;
            const bool modernButton = refresh &&
                (GetWindowLongPtrW(refresh, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW;
            const bool modernToggle = GetPropW(g_virtualBassMasterEnable,
                kUiToggleSwitchProperty) != nullptr;

            for (int index = 0; index < 12; ++index)
            {
                const std::wstring item = L"UI scroll test " + std::to_wstring(index + 1);
                SendMessageW(g_deviceList, LB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(item.c_str()));
            }
            RECT originalListClient{};
            GetClientRect(g_deviceList, &originalListClient);
            RECT windowRect{};
            GetWindowRect(window, &windowRect);
            SetWindowPos(window, nullptr, 0, 0,
                windowRect.right - windowRect.left + UiScale(window, 100),
                windowRect.bottom - windowRect.top + UiScale(window, 80),
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT resizedListClient{};
            GetClientRect(g_deviceList, &resizedListClient);
            HRGN mixerRegion = CreateRectRgn(0, 0, 1, 1);
            RECT mixerRegionBounds{};
            const int mixerRegionType = GetWindowRgn(g_deviceList, mixerRegion);
            if (mixerRegionType != ERROR)
                GetRgnBox(mixerRegion, &mixerRegionBounds);
            DeleteObject(mixerRegion);
            const bool mixerRegionTracksResize = mixerRegionType != ERROR &&
                mixerRegionBounds.right >= resizedListClient.right &&
                mixerRegionBounds.bottom >= resizedListClient.bottom;
            const RECT resizedFaderRail = MixerFaderRailRect(g_deviceList, 0);
            const RECT resizedFaderHit = MixerFaderHitRect(g_deviceList, 0);
            const POINT resizedFaderPoint{
                (resizedFaderRail.left + resizedFaderRail.right) / 2,
                (resizedFaderRail.top + resizedFaderRail.bottom) / 2 };
            int resizedFaderIndex = -1;
            const bool faderHitAfterResize =
                PtInRect(&resizedFaderHit, resizedFaderPoint) != FALSE &&
                MixerIndexAtPoint(g_deviceList, resizedFaderPoint.x,
                    resizedFaderPoint.y, resizedFaderIndex) &&
                resizedFaderIndex == 0;
            const LONG_PTR windowStyle = GetWindowLongPtrW(window, GWL_STYLE);
            const bool resizableWindow =
                (windowStyle & WS_THICKFRAME) != 0 &&
                (windowStyle & WS_MAXIMIZEBOX) != 0 &&
                resizedListClient.right > originalListClient.right &&
                resizedListClient.bottom > originalListClient.bottom;

            RECT listRect{};
            GetWindowRect(g_deviceList, &listRect);
            const int wheelX = (listRect.left + listRect.right) / 2;
            const int wheelY = (listRect.top + listRect.bottom) / 2;
            SendMessageW(g_deviceList, WM_MOUSEWHEEL,
                MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)),
                MAKELPARAM(wheelX, wheelY));
            const bool wheelScrolled = MixerStoredScrollOffset(g_deviceList) > 0;
            const MixerScrollMetrics bottomMetrics =
                DeviceMixerScrollMetrics(g_deviceList);
            SetMixerScrollOffset(g_deviceList, bottomMetrics.maximumOffset);
            RECT listClient{};
            GetClientRect(g_deviceList, &listClient);
            const int itemCount = static_cast<int>(SendMessageW(
                g_deviceList, LB_GETCOUNT, 0, 0));
            const RECT finalChannel = MixerChannelRect(g_deviceList,
                std::max(0, itemCount - 1));
            const bool finalDeviceFullyVisible = itemCount > 0 &&
                finalChannel.left >= 0 && finalChannel.right <= listClient.right;
            StoreMixerScrollOffset(g_deviceList, 0);
            const RECT firstFader = MixerFaderRect(g_deviceList, 0);
            const bool volumePanelLayout = firstFader.bottom > firstFader.top &&
                firstFader.bottom <= listClient.bottom;
            bool radioOnlySelects = true;
            if (!g_devices.empty() && !g_apoMode)
            {
                const bool originalSelection = SendMessageW(g_deviceList,
                    LB_GETSEL, 0, 0) > 0;
                const RECT name = MixerNameRect(g_deviceList, 0);
                SendMessageW(g_deviceList, WM_LBUTTONDOWN, MK_LBUTTON,
                    MAKELPARAM((name.left + name.right) / 2,
                        (name.top + name.bottom) / 2));
                const bool nameDidNotSelect =
                    (SendMessageW(g_deviceList, LB_GETSEL, 0, 0) > 0) ==
                    originalSelection;
                const RECT radio = MixerRadioRect(g_deviceList, 0);
                SendMessageW(g_deviceList, WM_LBUTTONDOWN, MK_LBUTTON,
                    MAKELPARAM((radio.left + radio.right) / 2,
                        (radio.top + radio.bottom) / 2));
                const bool radioToggled =
                    (SendMessageW(g_deviceList, LB_GETSEL, 0, 0) > 0) !=
                    originalSelection;
                SendMessageW(g_deviceList, LB_SETSEL,
                    originalSelection ? TRUE : FALSE, 0);
                radioOnlySelects = nameDidNotSelect && radioToggled;
            }
            const bool endpointVolumesReadable = g_devices.empty() ||
                std::any_of(g_devices.begin(), g_devices.end(),
                    [](const DeviceInfo& device) { return device.volumeKnown; });

            ShowAbout(window);
            wchar_t aboutClass[64]{};
            if (g_aboutWindow) GetClassNameW(g_aboutWindow,
                aboutClass, ARRAYSIZE(aboutClass));
            const bool modernAbout = g_aboutWindow &&
                wcscmp(aboutClass, kAboutWindowClass) == 0 &&
                GetDlgItem(g_aboutWindow, kAboutCloseButtonId) != nullptr;
            if (g_aboutWindow) DestroyWindow(g_aboutWindow);

            HDC sliderDc = g_bufferSlider ? GetDC(g_bufferSlider) : nullptr;
            const bool sliderRendered = g_apoMode || sliderDc != nullptr;
            UiSurfaceCache* firstSliderCache = nullptr;
            bool persistentSliderBuffer = g_apoMode;
            if (sliderDc)
            {
                PaintModernSlider(g_bufferSlider, sliderDc);
                firstSliderCache = reinterpret_cast<UiSurfaceCache*>(
                    GetPropW(g_bufferSlider, kUiSurfaceCacheProperty));
                PaintModernSlider(g_bufferSlider, sliderDc);
                persistentSliderBuffer = firstSliderCache && firstSliderCache ==
                    reinterpret_cast<UiSurfaceCache*>(GetPropW(
                        g_bufferSlider, kUiSurfaceCacheProperty));
                ReleaseDC(g_bufferSlider, sliderDc);
            }
            SendMessageW(g_virtualBassMasterEnable, BM_SETCHECK,
                BST_UNCHECKED, 0);
            UiAnimationState* toggleAnimation = EnsureUiAnimation(
                g_virtualBassMasterEnable);
            if (toggleAnimation)
            {
                toggleAnimation->toggle = toggleAnimation->toggleTarget = 0.0f;
                SendMessageW(g_virtualBassMasterEnable, BM_SETCHECK,
                    BST_CHECKED, 0);
                toggleAnimation->lastTick = GetTickCount64() - 15;
                AdvanceUiAnimation(g_virtualBassMasterEnable);
            }
            const bool toggleHasIntermediateFrame = toggleAnimation &&
                toggleAnimation->toggle > 0.0f && toggleAnimation->toggle < 1.0f;
            LOGFONTW fontInfo{};
            const HFONT listFont = reinterpret_cast<HFONT>(
                SendMessageW(g_deviceList, WM_GETFONT, 0, 0));
            const bool modernFont = listFont &&
                GetObjectW(listFont, sizeof(fontInfo), &fontInfo) == sizeof(fontInfo) &&
                wcsstr(fontInfo.lfFaceName, L"Segoe UI") != nullptr;
            wchar_t mainTitle[128]{};
            GetWindowTextW(window, mainTitle, ARRAYSIZE(mainTitle));
            const bool desktopIdentity =
                wcscmp(mainTitle, L"HXAudio Desktop") == 0 &&
                FindResourceW(GetModuleHandleW(nullptr),
                    MAKEINTRESOURCEW(IDR_HXAUDIO_PNG), RT_RCDATA) != nullptr;
            valid = modernMixer && modernButton && modernToggle && wheelScrolled &&
                sliderRendered && persistentSliderBuffer &&
                toggleHasIntermediateFrame && modernFont &&
                finalDeviceFullyVisible && volumePanelLayout &&
                endpointVolumesReadable && modernAbout && radioOnlySelects &&
                resizableWindow && mixerRegionTracksResize &&
                faderHitAfterResize && desktopIdentity;
        }
        if (window) DestroyWindow(window);
        if (SUCCEEDED(co)) CoUninitialize();
        return valid ? 0 : 23;
    }
    if (g_virtualBassUiSmoke)
    {
        OpenVirtualBassEditor(window);
        bool interactionValid = false;
        if (g_virtualBassWindow && g_virtualBassEnabled && g_virtualBassMode &&
            g_virtualBassAmount && g_virtualBassCutoff && g_virtualBassSubsonic &&
            g_virtualBassCustom[0] && g_virtualBassCustom[1] &&
            g_virtualBassCustom[2] && g_virtualBassLiveApply &&
            g_virtualBassStatus)
        {
            SendMessageW(g_virtualBassMode, CB_SETCURSEL, 0, 0);
            SendMessageW(g_virtualBassWindow, WM_COMMAND,
                MAKEWPARAM(kVirtualBassModeId, CBN_SELCHANGE),
                reinterpret_cast<LPARAM>(g_virtualBassMode));
            wchar_t firstMode[64]{};
            SendMessageW(g_virtualBassMode, CB_GETLBTEXT, 0,
                reinterpret_cast<LPARAM>(firstMode));
            const LRESULT selectedMode = SendMessageW(g_virtualBassMode, CB_GETCURSEL, 0, 0);
            wchar_t selectedModeText[64]{};
            if (selectedMode >= 0)
                SendMessageW(g_virtualBassMode, CB_GETLBTEXT, selectedMode,
                    reinterpret_cast<LPARAM>(selectedModeText));
            const bool comboTextValid = wcscmp(firstMode, L"2 / 3 / 4 次") == 0 &&
                selectedMode >= 0 && wcscmp(selectedModeText, L"2 / 3 / 4 次") == 0;
            SendMessageW(g_virtualBassLiveApply, BM_SETCHECK, BST_UNCHECKED, 0);
            RedrawWindow(g_virtualBassWindow, nullptr, nullptr,
                RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            SendMessageW(g_virtualBassAmount, TBM_SETPOS, TRUE, 137);
            SendMessageW(g_virtualBassWindow, WM_HSCROLL, 0,
                reinterpret_cast<LPARAM>(g_virtualBassAmount));
            SendMessageW(g_virtualBassMode, CB_SETCURSEL, 2, 0);
            SendMessageW(g_virtualBassWindow, WM_COMMAND,
                MAKEWPARAM(kVirtualBassModeId, CBN_SELCHANGE),
                reinterpret_cast<LPARAM>(g_virtualBassMode));
            SetWindowTextW(g_virtualBassCustom[0], L"2.5");
            SetWindowTextW(g_virtualBassCustom[1], L"3.75");
            SetWindowTextW(g_virtualBassCustom[2], L"6.5");

            DspSettings settings;
            const bool customParsed = ReadVirtualBassEditorSettings(settings,
                g_virtualBassWindow, false) &&
                settings.virtualBassMode == VirtualBassMode::Custom &&
                std::abs(settings.virtualBassMultipliers[0] - 2.5) < 1.0e-9 &&
                std::abs(settings.virtualBassMultipliers[1] - 3.75) < 1.0e-9 &&
                std::abs(settings.virtualBassMultipliers[2] - 6.5) < 1.0e-9 &&
                IsWindowVisible(g_virtualBassCustom[0]);
            const RECT graph = VirtualBassGraphRect(g_virtualBassWindow);
            const int oldX = VirtualBassFrequencyToX(settings.virtualBassCutoffHz, graph);
            const std::wstring oldCutoff = ReadWindowText(g_virtualBassCutoff);
            const int newX = std::min<LONG>(oldX + 30, graph.right - 3);
            SendMessageW(g_virtualBassWindow, WM_LBUTTONDOWN, MK_LBUTTON,
                MAKELPARAM(oldX, graph.top + 20));
            SendMessageW(g_virtualBassWindow, WM_MOUSEMOVE, MK_LBUTTON,
                MAKELPARAM(newX, graph.top + 20));
            SendMessageW(g_virtualBassWindow, WM_LBUTTONUP, 0,
                MAKELPARAM(newX, graph.top + 20));
            interactionValid = comboTextValid && g_virtualBassDirty &&
                customParsed &&
                SliderPosition(g_virtualBassAmount) == 137 &&
                SendMessageW(g_virtualBassMode, CB_GETCURSEL, 0, 0) == 2 &&
                ReadWindowText(g_virtualBassCutoff) != oldCutoff;
        }
        const bool valid = window && g_virtualBassWindow && interactionValid;
        if (g_virtualBassWindow) DestroyWindow(g_virtualBassWindow);
        if (window) DestroyWindow(window);
        if (SUCCEEDED(co)) CoUninitialize();
        return valid ? 0 : 22;
    }
    if (g_uiSmokeTest)
    {
        OpenPeqEditor(window);
        const size_t initialBands = g_peqEditorBands.size();
        bool interactionValid = false;
        int interactionFailure = 21;
        if (g_peqWindow)
        {
            const bool defaultGainRange =
                g_peqViewDbRange >= kPeqMinimumViewDbRange &&
                g_peqViewDbRange <= HXAudioDsp::kMaximumPeqGainDb;
            wchar_t firstType[64]{};
            SendMessageW(g_peqType, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(firstType));
            const LRESULT selectedDevice = SendMessageW(g_peqDevice, CB_GETCURSEL, 0, 0);
            const bool comboTextValid = wcscmp(firstType, L"Peak") == 0 &&
                selectedDevice >= 0 && static_cast<size_t>(selectedDevice) < g_peqEditorDevices.size() &&
                ReadWindowText(g_peqDevice) == g_peqEditorDevices[static_cast<size_t>(selectedDevice)].name;
            const RECT graph = PeqGraphRect(g_peqWindow);
            HDC previewDc = GetDC(g_peqWindow);
            const bool rendered = previewDc != nullptr;
            bool performanceValid = false;
            if (previewDc)
            {
                PaintPeqEditor(g_peqWindow, previewDc);
                const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
                const int savedDc = SaveDC(previewDc);
                IntersectClipRect(previewDc, graph.left, graph.top, graph.right, graph.bottom);
                const ULONGLONG started = GetTickCount64();
                for (int frame = 0; frame < 48; ++frame)
                    PaintPeqEditor(g_peqWindow, previewDc);
                GdiFlush();
                const ULONGLONG elapsed = GetTickCount64() - started;
                RestoreDC(previewDc, savedDc);
                const DWORD gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
                performanceValid = elapsed < 4000 && gdiAfter <= gdiBefore + 8;
                ReleaseDC(g_peqWindow, previewDc);
            }
            POINT point{ graph.left + 30, graph.top + 30 };
            bool foundBlank = false;
            for (int y = graph.top + 20; y < graph.bottom - 20 && !foundBlank; y += 37)
            {
                for (int x = graph.left + 20; x < graph.right - 20; x += 53)
                {
                    POINT candidate{ x, y };
                    const RECT zoomOut = PeqGainZoomOutRect(g_peqWindow);
                    const RECT zoomIn = PeqGainZoomInRect(g_peqWindow);
                    if (!PtInRect(&zoomOut, candidate) &&
                        !PtInRect(&zoomIn, candidate) &&
                        PeqHitTest(g_peqWindow, candidate) < 0)
                    {
                        point = candidate;
                        foundBlank = true;
                        break;
                    }
                }
            }
            SendMessageW(g_peqWindow, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(point.x, point.y));
            const bool added = g_peqEditorBands.size() == initialBands + 1 && PeqSelectionIsValid();
            const int addedIndex = added ? g_peqSelectedBand : -1;
            const double previousFrequency = added
                ? g_peqEditorBands[addedIndex].frequencyHz : 0.0;
            const double previousGain = added
                ? g_peqEditorBands[addedIndex].gainDb : 0.0;
            POINT dragStart = point;
            if (added)
            {
                dragStart.x = PeqFrequencyToX(
                    g_peqEditorBands[addedIndex].frequencyHz, graph);
                dragStart.y = PeqGainToY(PeqHandleResponseDb(
                    addedIndex, PeqStoredPreampDb()), graph);
            }
            POINT draggedPoint{ std::min<LONG>(dragStart.x + 42, graph.right - 10),
                std::min<LONG>(dragStart.y + 24, graph.bottom - 10) };
            SendMessageW(g_peqWindow, WM_LBUTTONDOWN, MK_LBUTTON,
                MAKELPARAM(dragStart.x, dragStart.y));
            SendMessageW(g_peqWindow, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(draggedPoint.x, draggedPoint.y));
            SendMessageW(g_peqWindow, WM_LBUTTONUP, 0, MAKELPARAM(draggedPoint.x, draggedPoint.y));
            const bool dragSelectionValid = added && PeqSelectionIsValid() &&
                g_peqSelectedBand == addedIndex;
            const bool dragged = dragSelectionValid &&
                g_peqEditorBands[addedIndex].frequencyHz != previousFrequency &&
                g_peqEditorBands[addedIndex].gainDb != previousGain;
            const double previousQ = dragSelectionValid
                ? g_peqEditorBands[addedIndex].q : 0.0;
            POINT screenPoint = draggedPoint;
            if (PeqSelectionIsValid())
            {
                screenPoint.x = PeqFrequencyToX(
                    g_peqEditorBands[g_peqSelectedBand].frequencyHz, graph);
                screenPoint.y = PeqGainToY(PeqHandleResponseDb(
                    g_peqSelectedBand, PeqStoredPreampDb()), graph);
            }
            const bool qSelectionValid = PeqSelectionIsValid();
            const bool qPointInside = PtInRect(&graph, screenPoint) != FALSE;
            ClientToScreen(g_peqWindow, &screenPoint);
            SendMessageW(g_peqWindow, WM_MOUSEWHEEL,
                MAKEWPARAM(MK_CONTROL, WHEEL_DELTA),
                MAKELPARAM(screenPoint.x, screenPoint.y));
            const bool qChanged = dragSelectionValid &&
                g_peqEditorBands[addedIndex].q > previousQ;
            const double previousRange = g_peqViewDbRange;
            SendMessageW(g_peqWindow, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA),
                MAKELPARAM(screenPoint.x, screenPoint.y));
            const bool directGainZoomed = g_peqViewDbRange < previousRange;
            const RECT zoomOut = PeqGainZoomOutRect(g_peqWindow);
            const RECT zoomIn = PeqGainZoomInRect(g_peqWindow);
            const bool zoomControlsPresent = zoomOut.right > zoomOut.left &&
                zoomOut.bottom > zoomOut.top && zoomIn.right > zoomIn.left &&
                zoomIn.bottom > zoomIn.top;
            const double rangeBeforeButton = g_peqViewDbRange;
            SendMessageW(g_peqWindow, WM_LBUTTONDOWN, MK_LBUTTON,
                MAKELPARAM((zoomOut.left + zoomOut.right) / 2,
                    (zoomOut.top + zoomOut.bottom) / 2));
            const bool zoomOutWorked = g_peqViewDbRange > rangeBeforeButton;
            const RECT gainZoomDirty = PeqGainZoomDirtyRect(g_peqWindow);
            const bool axisLabelsInvalidated = gainZoomDirty.left < graph.left &&
                gainZoomDirty.top == graph.top && gainZoomDirty.bottom == graph.bottom;
            const double rangeAfterOut = g_peqViewDbRange;
            SendMessageW(g_peqWindow, WM_LBUTTONDOWN, MK_LBUTTON,
                MAKELPARAM((zoomIn.left + zoomIn.right) / 2,
                    (zoomIn.top + zoomIn.bottom) / 2));
            const bool zoomInWorked = g_peqViewDbRange < rangeAfterOut;
            SendMessageW(g_peqWindow, WM_COMMAND,
                MAKEWPARAM(kPeqDeleteId, BN_CLICKED), 0);
            const bool deleted = g_peqEditorBands.size() == initialBands;
            interactionValid = defaultGainRange && comboTextValid && rendered && performanceValid &&
                foundBlank && added && dragged && directGainZoomed && qChanged &&
                zoomControlsPresent && zoomOutWorked && axisLabelsInvalidated &&
                zoomInWorked && deleted;
            if (!defaultGainRange) interactionFailure = 42;
            else if (!comboTextValid) interactionFailure = 31;
            else if (!rendered) interactionFailure = 32;
            else if (!performanceValid) interactionFailure = 33;
            else if (!foundBlank || !added) interactionFailure = 34;
            else if (!dragged) interactionFailure = 35;
            else if (!directGainZoomed) interactionFailure = 36;
            else if (!qSelectionValid) interactionFailure = 43;
            else if (!qPointInside) interactionFailure = 44;
            else if (!qChanged) interactionFailure = 37;
            else if (!zoomControlsPresent) interactionFailure = 38;
            else if (!zoomOutWorked || !zoomInWorked) interactionFailure = 39;
            else if (!axisLabelsInvalidated) interactionFailure = 40;
            else if (!deleted) interactionFailure = 41;
        }
        const bool valid = window && g_peqWindow && g_peqEnabled && g_peqType &&
            g_peqFrequency && g_peqGain && g_peqQ && g_peqLiveApply &&
            g_peqDevice && interactionValid;
        if (g_peqWindow) DestroyWindow(g_peqWindow);
        if (window) DestroyWindow(window);
        if (SUCCEEDED(co)) CoUninitialize();
        return valid ? 0 : interactionFailure;
    }
    if (g_uiPreview)
    {
        OpenPeqEditor(window);
    }
    else if (g_virtualBassUiPreview)
    {
        OpenVirtualBassEditor(window);
    }
    else
    {
        ShowWindow(window, show);
        SizeWindowForCurrentDpi(window, 980, MainWindowLogicalHeight());
        PostMessageW(window, kApplyInitialDpiSizeMessage, 0, 0);
        UpdateWindow(window);
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return static_cast<int>(message.wParam);
}
