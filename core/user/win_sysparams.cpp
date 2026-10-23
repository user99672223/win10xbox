// core/user system parameters: GetSystemMetrics (Windows 10 values at 96 DPI),
// SystemParametersInfo, DPI awareness (everything is 96 DPI and the process is
// DPI-unaware unless it says otherwise), and the monitor / display-mode
// queries, answered from the display driver's QueryDisplay().
#include "user_internal.h"
#include "user_syscalls.h"

#include <cstring>

namespace win10xbox::user {
namespace detail {
namespace {


// NONCLIENTMETRICS at 96 DPI (Windows 10).
constexpr int kBorderWidth = 1, kScrollWidth = 17, kCaptionWidth = 36, kCaptionHeight = 22, kSmCaptionWidth = 22,
              kSmCaptionHeight = 22, kMenuWidth = 19, kMenuHeight = 19, kPaddedBorder = 4;

LOGFONTW UiFont(LONG height = -12, LONG weight = FW_NORMAL) {
    LOGFONTW lf{};
    lf.lfHeight = height;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = DEFAULT_QUALITY;
    wcscpy(lf.lfFaceName, L"Segoe UI");
    return lf;
}

void FillNonClientMetrics(NONCLIENTMETRICSW* m) {
    m->iBorderWidth = kBorderWidth;
    m->iScrollWidth = kScrollWidth;
    m->iScrollHeight = kScrollWidth;
    m->iCaptionWidth = kCaptionWidth;
    m->iCaptionHeight = kCaptionHeight;
    m->lfCaptionFont = UiFont();
    m->iSmCaptionWidth = kSmCaptionWidth;
    m->iSmCaptionHeight = kSmCaptionHeight;
    m->lfSmCaptionFont = UiFont();
    m->iMenuWidth = kMenuWidth;
    m->iMenuHeight = kMenuHeight;
    m->lfMenuFont = UiFont();
    m->lfStatusFont = UiFont();
    m->lfMessageFont = UiFont();
    if (m->cbSize >= sizeof(NONCLIENTMETRICSW)) m->iPaddedBorderWidth = kPaddedBorder;
}

template <class T> bool Put(void* ptr, T v) {
    if (!ptr) return false;
    std::memcpy(ptr, &v, sizeof v);
    return true;
}

winexbox::DisplayMode CurrentMode() {
    winexbox::DisplayMode m{};
    if (winexbox::IWinexboxDriver* d = Driver()) {
        const winexbox::DisplayConfig c = d->QueryDisplay();
        if (!c.monitors.empty()) m = c.monitors[0].current;
    }
    if (!m.width) { m.width = 1920; m.height = 1080; m.bits_per_pixel = 32; m.refresh_hz = 60; }
    return m;
}

void FillDevMode(DEVMODEW* dm, const winexbox::DisplayMode& m) {
    const WORD size = dm->dmSize, extra = dm->dmDriverExtra;
    std::memset(reinterpret_cast<std::uint8_t*>(dm) + offsetof(DEVMODEW, dmSpecVersion), 0,
                size > offsetof(DEVMODEW, dmSpecVersion) ? size - offsetof(DEVMODEW, dmSpecVersion) : 0);
    wcscpy(dm->dmDeviceName, L"winexbox");
    dm->dmSize = size;
    dm->dmDriverExtra = extra;
    dm->dmSpecVersion = DM_SPECVERSION;
    dm->dmDriverVersion = DM_SPECVERSION;
    dm->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS | DM_POSITION |
                   DM_DISPLAYORIENTATION;
    dm->dmPelsWidth = m.width;
    dm->dmPelsHeight = m.height;
    dm->dmBitsPerPel = m.bits_per_pixel ? m.bits_per_pixel : 32;
    dm->dmDisplayFrequency = m.refresh_hz;
    dm->dmPosition = POINTL{0, 0};
    dm->dmDisplayOrientation = DMDO_DEFAULT;
}

}  // namespace

void ScreenRect(RECT* out) {
    State& s = S();
    if (!s.screen_w) {
        std::int32_t w = 0, h = 0;
        ScreenSize(&w, &h);
        s.screen_w = w ? w : 1920;
        s.screen_h = h ? h : 1080;
    }
    *out = RECT{0, 0, s.screen_w, s.screen_h};
}

void WorkAreaRect(RECT* out) {
    if (State* st = StateIfAny(); st && st->work_area_set) {
        *out = st->work_area;
        return;
    }
    // no taskbar: the work area is the whole monitor
    if (winexbox::IWinexboxDriver* d = Driver()) {
        const winexbox::DisplayConfig c = d->QueryDisplay();
        if (!c.monitors.empty()) {
            const winexbox::Rect& r = c.monitors[0].work;
            if (!r.Empty()) {
                *out = RECT{r.left, r.top, r.right, r.bottom};
                return;
            }
        }
    }
    ScreenRect(out);
}

int SystemMetric(int index) {
    RECT screen, work;
    ScreenRect(&screen);
    WorkAreaRect(&work);
    const int sw = screen.right, sh = screen.bottom;
    const int frame = kBorderWidth + 3;   // SM_CXFRAME: 4
    switch (index) {
    case SM_CXSCREEN: return sw;
    case SM_CYSCREEN: return sh;
    case SM_CXVSCROLL: case SM_CYHSCROLL: case SM_CYVTHUMB: case SM_CXHTHUMB: case SM_CYVSCROLL: case SM_CXHSCROLL:
        return kScrollWidth;
    case SM_CYCAPTION: return kCaptionHeight + 1;
    case SM_CXBORDER: case SM_CYBORDER: return 1;
    case SM_CXDLGFRAME: case SM_CYDLGFRAME: return 3;
    case SM_CXICON: case SM_CYICON: case SM_CXCURSOR: case SM_CYCURSOR: return 32;
    case SM_CYMENU: return kMenuHeight + 1;
    case SM_CXFULLSCREEN: return work.right - work.left;
    case SM_CYFULLSCREEN: return work.bottom - work.top - (kCaptionHeight + 1);
    case SM_CYKANJIWINDOW: return 0;
    case SM_MOUSEPRESENT: return 1;
    case SM_DEBUG: return 0;
    case SM_SWAPBUTTON: return 0;
    case SM_CXMIN: case SM_CXMINTRACK: return 136;
    case SM_CYMIN: case SM_CYMINTRACK: return 39;
    case SM_CXSIZE: return kCaptionWidth;
    case SM_CYSIZE: return kCaptionHeight;
    case SM_CXFRAME: case SM_CYFRAME: return frame;
    case SM_CXDOUBLECLK: case SM_CYDOUBLECLK: return 4;
    case SM_CXICONSPACING: case SM_CYICONSPACING: return 75;
    case SM_MENUDROPALIGNMENT: return 0;
    case SM_PENWINDOWS: return 0;
    case SM_DBCSENABLED: return 0;
    case SM_CMOUSEBUTTONS: return 5;
    case SM_SECURE: return 0;
    case SM_CXEDGE: case SM_CYEDGE: return 2;
    case SM_CXMINSPACING: return 160;
    case SM_CYMINSPACING: return 28;
    case SM_CXSMICON: case SM_CYSMICON: return 16;
    case SM_CYSMCAPTION: return kSmCaptionHeight + 1;
    case SM_CXSMSIZE: return kSmCaptionWidth;
    case SM_CYSMSIZE: return kSmCaptionHeight;
    case SM_CXMENUSIZE: return kMenuWidth;
    case SM_CYMENUSIZE: return kMenuHeight;
    case SM_ARRANGE: return ARW_HIDE;
    case SM_CXMINIMIZED: return 160;
    case SM_CYMINIMIZED: return 28;
    case SM_CXMAXTRACK: return sw + 4 + 2 * (frame + kPaddedBorder);
    case SM_CYMAXTRACK: return sh + 4 + 2 * (frame + kPaddedBorder);
    case SM_CXMAXIMIZED: return work.right - work.left + 2 * (frame + kPaddedBorder);
    case SM_CYMAXIMIZED: return work.bottom - work.top + 2 * (frame + kPaddedBorder);
    case SM_NETWORK: return 3;
    case SM_CLEANBOOT: return 0;
    case SM_CXDRAG: case SM_CYDRAG: return 4;
    case SM_SHOWSOUNDS: return 0;
    case SM_CXMENUCHECK: case SM_CYMENUCHECK: return 15;
    case SM_SLOWMACHINE: return 0;
    case SM_MIDEASTENABLED: return 0;
    case SM_MOUSEWHEELPRESENT: return 1;
    case SM_XVIRTUALSCREEN: case SM_YVIRTUALSCREEN: return 0;
    case SM_CXVIRTUALSCREEN: return sw;
    case SM_CYVIRTUALSCREEN: return sh;
    case SM_CMONITORS: return 1;
    case SM_SAMEDISPLAYFORMAT: return 1;
    case SM_IMMENABLED: return 0;
    case SM_CXFOCUSBORDER: case SM_CYFOCUSBORDER: return 1;
    case SM_TABLETPC: case SM_MEDIACENTER: case SM_STARTER: case SM_SERVERR2: return 0;
    case SM_MOUSEHORIZONTALWHEELPRESENT: return 1;
    case SM_CXPADDEDBORDER: return kPaddedBorder;
    case SM_DIGITIZER: case SM_MAXIMUMTOUCHES: return 0;
    case SM_REMOTESESSION: case SM_SHUTTINGDOWN: case SM_REMOTECONTROL: case SM_CARETBLINKINGENABLED: return 0;
    case 0x2003: /* SM_CONVERTIBLESLATEMODE */ return 0;
    case 0x2004: /* SM_SYSTEMDOCKED */ return 0;
    }
    FIXME(user, "GetSystemMetrics(%d)", index);
    return 0;
}

bool SysParamsInfo(UINT action, UINT val, void* ptr, UINT winini) {
    (void)winini;
    switch (action) {
    case SPI_GETWORKAREA: {
        RECT r;
        WorkAreaRect(&r);
        return Put(ptr, r);
    }
    case SPI_GETNONCLIENTMETRICS: {
        auto* m = static_cast<NONCLIENTMETRICSW*>(ptr);
        if (!m || (m->cbSize != sizeof(NONCLIENTMETRICSW) && m->cbSize != offsetof(NONCLIENTMETRICSW, iPaddedBorderWidth))) {
            SetLastErr(ERROR_INVALID_PARAMETER);
            return false;
        }
        FillNonClientMetrics(m);
        return true;
    }
    case SPI_GETICONTITLELOGFONT: return Put(ptr, UiFont());
    case SPI_GETICONMETRICS: {
        auto* m = static_cast<ICONMETRICSW*>(ptr);
        if (!m || m->cbSize != sizeof(ICONMETRICSW)) return false;
        m->iHorzSpacing = m->iVertSpacing = 75;
        m->iTitleWrap = TRUE;
        m->lfFont = UiFont();
        return true;
    }
    case SPI_GETMINIMIZEDMETRICS: {
        auto* m = static_cast<MINIMIZEDMETRICS*>(ptr);
        if (!m || m->cbSize != sizeof(MINIMIZEDMETRICS)) return false;
        m->iWidth = 154;
        m->iHorzGap = m->iVertGap = 0;
        m->iArrange = ARW_HIDE;
        return true;
    }
    case SPI_GETHIGHCONTRAST: {
        auto* hc = static_cast<HIGHCONTRASTW*>(ptr);
        if (!hc || hc->cbSize != sizeof(HIGHCONTRASTW)) return false;
        hc->dwFlags = 0;
        hc->lpszDefaultScheme = nullptr;
        return true;
    }
    case SPI_GETANIMATION: {
        auto* a = static_cast<ANIMATIONINFO*>(ptr);
        if (!a || a->cbSize != sizeof(ANIMATIONINFO)) return false;
        a->iMinAnimate = 0;
        return true;
    }
    case SPI_GETMOUSE: {
        const int v[3] = {6, 10, 1};
        if (!ptr) return false;
        std::memcpy(ptr, v, sizeof v);
        return true;
    }
    case SPI_GETFONTSMOOTHING: case SPI_GETDRAGFULLWINDOWS: case SPI_GETBEEP: case SPI_GETGRADIENTCAPTIONS:
    case SPI_GETCLIENTAREAANIMATION: case SPI_GETUIEFFECTS: case SPI_GETKEYBOARDCUES: case SPI_GETHOTTRACKING:
    case SPI_GETCOMBOBOXANIMATION: case SPI_GETLISTBOXSMOOTHSCROLLING: case SPI_GETMENUANIMATION:
    case SPI_GETTOOLTIPANIMATION: case SPI_GETSELECTIONFADE: case SPI_GETTOOLTIPFADE: case SPI_GETMENUFADE:
        return Put<BOOL>(ptr, action == SPI_GETKEYBOARDCUES ? FALSE : TRUE);
    case SPI_GETFLATMENU: case SPI_GETDROPSHADOW: case SPI_GETCURSORSHADOW: case SPI_GETSHOWSOUNDS:
    case SPI_GETKEYBOARDPREF: case SPI_GETSCREENREADER: case SPI_GETSNAPTODEFBUTTON: case SPI_GETSCREENSAVEACTIVE:
    case SPI_GETACTIVEWINDOWTRACKING: case SPI_GETMENUDROPALIGNMENT: case SPI_GETLOWPOWERACTIVE:
    case SPI_GETPOWEROFFACTIVE: case SPI_GETBLOCKSENDINPUTRESETS: case SPI_GETSCREENSAVERRUNNING:
    case SPI_GETMOUSEVANISH: case SPI_GETMOUSESONAR: case SPI_GETMOUSECLICKLOCK:
        return Put<BOOL>(ptr, FALSE);
    case SPI_GETFONTSMOOTHINGTYPE: return Put<UINT>(ptr, FE_FONTSMOOTHINGSTANDARD);
    case SPI_GETFONTSMOOTHINGCONTRAST: return Put<UINT>(ptr, 1400);
    case SPI_GETFONTSMOOTHINGORIENTATION: return Put<UINT>(ptr, FE_FONTSMOOTHINGORIENTATIONRGB);
    case SPI_GETKEYBOARDDELAY: return Put<INT>(ptr, 1);
    case SPI_GETKEYBOARDSPEED: return Put<DWORD>(ptr, 31);
    case SPI_GETMOUSEHOVERTIME: return Put<UINT>(ptr, 400);
    case SPI_GETMOUSEHOVERWIDTH: case SPI_GETMOUSEHOVERHEIGHT: return Put<UINT>(ptr, 4);
    case SPI_GETWHEELSCROLLLINES: case SPI_GETWHEELSCROLLCHARS: return Put<UINT>(ptr, 3);
    case SPI_GETMENUSHOWDELAY: return Put<INT>(ptr, 400);
    case SPI_GETBORDER: return Put<INT>(ptr, 1);
    case SPI_GETMOUSESPEED: return Put<INT>(ptr, 10);
    case SPI_GETFOCUSBORDERWIDTH: case SPI_GETFOCUSBORDERHEIGHT: case SPI_GETCARETWIDTH: return Put<UINT>(ptr, 1);
    case SPI_GETSCREENSAVETIMEOUT: return Put<INT>(ptr, 0);
    case SPI_GETFOREGROUNDLOCKTIMEOUT: return Put<DWORD>(ptr, 200000);
    case SPI_GETFOREGROUNDFLASHCOUNT: return Put<DWORD>(ptr, 3);
    case SPI_GETACTIVEWNDTRKTIMEOUT: return Put<DWORD>(ptr, 0);
    case SPI_GETMESSAGEDURATION: return Put<ULONG>(ptr, 5);
    case SPI_GETMOUSETRAILS: return Put<INT>(ptr, 0);
    case SPI_GETDEFAULTINPUTLANG: return Put<HKL>(ptr, reinterpret_cast<HKL>(static_cast<ULONG_PTR>(0x04090409)));
    case SPI_GETDESKWALLPAPER: {
        if (!ptr || !val) return false;
        static_cast<WCHAR*>(ptr)[0] = 0;
        return true;
    }
    case SPI_GETCARETBROWSING: return Put<BOOL>(ptr, FALSE);
    case SPI_GETAUDIODESCRIPTION: {
        auto* ad = static_cast<AUDIODESCRIPTION*>(ptr);
        if (!ad || ad->cbSize != sizeof(AUDIODESCRIPTION)) return false;
        ad->Enabled = FALSE;
        ad->Locale = 0x409;
        return true;
    }
    }
    if (action >= 0x1000 && (action & 1)) {
        // the SPI_SET* half of the boolean / dword range
        WARN(user, "SystemParametersInfo(SPI %#x, %u): setting accepted, not kept", action, val);
        return true;
    }
    switch (action) {
    case SPI_SETWORKAREA: {
        // explorer.exe's taskbar reserves its strip: windows maximize into the rest
        const auto* r = static_cast<const RECT*>(ptr);
        if (!r) {
            SetLastErr(ERROR_INVALID_PARAMETER);
            return false;
        }
        State& st = S();
        st.work_area = *r;
        st.work_area_set = true;
        return true;
    }
    case SPI_SETSCREENSAVEACTIVE: case SPI_SETDESKWALLPAPER: case SPI_SETMOUSE:
    case SPI_SETKEYBOARDSPEED: case SPI_SETKEYBOARDDELAY: case SPI_SETDOUBLECLICKTIME: case SPI_SETMOUSETRAILS:
    case SPI_SETBEEP: case SPI_SETMENUDROPALIGNMENT: case SPI_SETICONTITLEWRAP: case SPI_SETFASTTASKSWITCH:
    case SPI_SETDRAGFULLWINDOWS: case SPI_SETNONCLIENTMETRICS: case SPI_SETSTICKYKEYS: case SPI_SETFILTERKEYS:
    case SPI_SETTOGGLEKEYS: case SPI_SETPOWEROFFACTIVE: case SPI_SETLOWPOWERACTIVE: case SPI_SETSCREENSAVETIMEOUT:
        WARN(user, "SystemParametersInfo(SPI %#x, %u): setting accepted, not kept", action, val);
        return true;
    case SPI_GETSTICKYKEYS: {
        auto* sk = static_cast<STICKYKEYS*>(ptr);
        if (!sk || sk->cbSize != sizeof(STICKYKEYS)) return false;
        sk->dwFlags = SKF_AVAILABLE | SKF_HOTKEYACTIVE;
        return true;
    }
    case SPI_GETTOGGLEKEYS: {
        auto* tk = static_cast<TOGGLEKEYS*>(ptr);
        if (!tk || tk->cbSize != sizeof(TOGGLEKEYS)) return false;
        tk->dwFlags = TKF_AVAILABLE | TKF_HOTKEYACTIVE;
        return true;
    }
    case SPI_GETFILTERKEYS: {
        auto* fk = static_cast<FILTERKEYS*>(ptr);
        if (!fk || fk->cbSize != sizeof(FILTERKEYS)) return false;
        std::memset(reinterpret_cast<std::uint8_t*>(fk) + sizeof(UINT), 0, sizeof(*fk) - sizeof(UINT));
        fk->dwFlags = FKF_AVAILABLE | FKF_HOTKEYACTIVE;
        return true;
    }
    }
    FIXME(user, "SystemParametersInfo(action %#x, %u, %p) not implemented", action, val, ptr);
    SetLastErr(ERROR_INVALID_SPI_VALUE);
    return false;
}

}  // namespace detail

namespace sys {
using namespace detail;

ULONG WINAPI NtUserGetSystemDpiForProcess(HANDLE process) {
    (void)process;
    return 96;
}

ULONG WINAPI NtUserGetProcessDpiAwarenessContext(HANDLE process) {
    LockGuard lock;
    (void)process;
    return S().process_dpi_context;
}

BOOL WINAPI NtUserSetProcessDpiAwarenessContext(ULONG awareness, ULONG unknown) {
    LockGuard lock;
    (void)unknown;
    State& s = S();
    const UINT a = awareness & 0x0f;
    if (a > 2) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (s.process_dpi_set) {
        SetLastErr(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    s.process_dpi_set = true;
    s.process_dpi_context = awareness & ~abi::kDpiFlagProcess;
    TRACE(user, "process DPI awareness context %#lx (everything stays at 96 DPI)", awareness);
    return TRUE;
}

BOOL WINAPI NtUserSystemParametersInfo(UINT action, UINT val, void* ptr, UINT winini) {
    LockGuard lock;
    return SysParamsInfo(action, val, ptr, winini);
}

BOOL WINAPI NtUserSystemParametersInfoForDpi(UINT action, UINT val, PVOID ptr, UINT winini, UINT dpi) {
    LockGuard lock;
    if (dpi != 96) FIXME(user, "SystemParametersInfoForDpi(%u): answered at 96 DPI", dpi);
    switch (action) {
    case SPI_GETICONTITLELOGFONT: case SPI_GETICONMETRICS: case SPI_GETNONCLIENTMETRICS:
        return SysParamsInfo(action, val, ptr, winini);
    }
    SetLastErr(ERROR_INVALID_PARAMETER);
    return FALSE;
}

BOOL WINAPI NtUserEnumDisplayMonitors(HDC hdc, RECT* rect, MONITORENUMPROC proc, LPARAM lp) {
    struct Params { MONITORENUMPROC proc; HMONITOR monitor; HDC hdc; RECT rect; LPARAM lparam; } p{};
    {
        LockGuard lock;
        RECT screen;
        ScreenRect(&screen);
        if (hdc) {
            FIXME(user, "EnumDisplayMonitors with a DC: monitor rectangles not clipped to it");
        }
        RECT r = screen;
        if (rect) {
            r = RectIntersect(*rect, screen);
            if (RectEmpty(r)) return TRUE;
        }
        p = Params{proc, abi::PrimaryMonitor(), hdc, r, lp};
    }
    if (!proc) return FALSE;
    void* ret = nullptr;
    ULONG len = 0;
    UserModeCallback(abi::CbCallEnumDisplayMonitor, &p, sizeof p, &ret, &len);
    return TRUE;
}

BOOL WINAPI NtUserGetDpiForMonitor(HMONITOR monitor, UINT type, UINT* x, UINT* y) {
    (void)type;
    if (monitor != abi::PrimaryMonitor() || !x || !y) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *x = *y = 96;
    return TRUE;
}

BOOL WINAPI NtUserEnumDisplaySettings(UNICODE_STRING* device, DWORD mode, DEVMODEW* dev_mode, DWORD flags) {
    LockGuard lock;
    (void)flags;
    if (!dev_mode) return FALSE;
    if (device && device->Length && _wcsnicmp(device->Buffer, L"\\\\.\\DISPLAY1", device->Length / sizeof(WCHAR))) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS) {
        FillDevMode(dev_mode, CurrentMode());
        return TRUE;
    }
    std::vector<winexbox::DisplayMode> modes;
    if (winexbox::IWinexboxDriver* d = Driver()) {
        const winexbox::DisplayConfig c = d->QueryDisplay();
        if (!c.monitors.empty()) modes = c.monitors[0].modes;
    }
    if (modes.empty()) modes.push_back(CurrentMode());
    if (mode >= modes.size()) {
        SetLastErr(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    FillDevMode(dev_mode, modes[mode]);
    return TRUE;
}

NTSTATUS WINAPI NtUserEnumDisplayDevices(UNICODE_STRING* device, DWORD index, DISPLAY_DEVICEW* info, DWORD flags) {
    LockGuard lock;
    (void)flags;
    if (!info || info->cb < offsetof(DISPLAY_DEVICEW, DeviceKey)) return static_cast<NTSTATUS>(0xC000000D);
    if (index > 0) return static_cast<NTSTATUS>(0xC0000001);   // STATUS_UNSUCCESSFUL: no more devices
    const bool monitors = device && device->Length;
    if (monitors) {
        wcscpy(info->DeviceName, L"\\\\.\\DISPLAY1\\Monitor0");
        wcscpy(info->DeviceString, L"Generic Non-PnP Monitor");
        info->StateFlags = DISPLAY_DEVICE_ACTIVE | DISPLAY_DEVICE_ATTACHED;
        if (info->cb >= offsetof(DISPLAY_DEVICEW, DeviceKey) + sizeof info->DeviceID)
            wcscpy(info->DeviceID, L"MONITOR\\Default_Monitor");
    } else {
        wcscpy(info->DeviceName, L"\\\\.\\DISPLAY1");
        wcscpy(info->DeviceString, L"winexbox Display Adapter");
        info->StateFlags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE;
        if (info->cb >= offsetof(DISPLAY_DEVICEW, DeviceKey) + sizeof info->DeviceID)
            wcscpy(info->DeviceID, L"PCI\\VEN_1414&DEV_008C");
    }
    if (info->cb >= sizeof(DISPLAY_DEVICEW)) info->DeviceKey[0] = 0;
    return dispatch::detail::st::Success;
}

LONG WINAPI NtUserGetDisplayConfigBufferSizes(UINT32 flags, UINT32* num_path_info, UINT32* num_mode_info) {
    (void)flags;
    if (!num_path_info || !num_mode_info) return ERROR_INVALID_PARAMETER;
    *num_path_info = 1;
    *num_mode_info = 2;
    return ERROR_SUCCESS;
}

// The one adapter and its one monitor, as win32u's sysparams.c reports them: source 0 / target 0 on a fixed adapter LUID,
// the current mode of the display driver (the same mode EnumDisplaySettings answers).
namespace {
constexpr LUID kAdapterLuid = {1, 0};
constexpr UINT32 kSourceId = 0, kTargetId = 0;
}  // namespace

LONG WINAPI NtUserQueryDisplayConfig(UINT32 flags, UINT32* paths_count, DISPLAYCONFIG_PATH_INFO* paths, UINT32* modes_count,
                                     DISPLAYCONFIG_MODE_INFO* modes, DISPLAYCONFIG_TOPOLOGY_ID* topology_id) {
    constexpr UINT32 kRetrieveMask = QDC_ALL_PATHS | QDC_ONLY_ACTIVE_PATHS | QDC_DATABASE_CURRENT;
    const UINT32 retrieve = flags & kRetrieveMask;
    if (!paths_count || !modes_count || !*paths_count || !*modes_count) return ERROR_INVALID_PARAMETER;
    if (retrieve != QDC_ALL_PATHS && retrieve != QDC_ONLY_ACTIVE_PATHS && retrieve != QDC_DATABASE_CURRENT)
        return ERROR_INVALID_PARAMETER;
    if ((retrieve == QDC_DATABASE_CURRENT) != (topology_id != nullptr)) return ERROR_INVALID_PARAMETER;
    if (flags & ~kRetrieveMask) {   // QDC_VIRTUAL_MODE_AWARE (desktop-image modes) is not provided
        FIXME(user, "QueryDisplayConfig flags %#x not supported", flags);
        return ERROR_INVALID_PARAMETER;
    }
    const UINT32 need_modes = 2;   // source, target
    if (!paths || !modes) return ERROR_INVALID_PARAMETER;
    if (*paths_count < 1 || *modes_count < need_modes) return ERROR_INSUFFICIENT_BUFFER;

    winexbox::DisplayMode m;
    {
        LockGuard lock;
        m = CurrentMode();
    }
    if (topology_id) *topology_id = DISPLAYCONFIG_TOPOLOGY_INTERNAL;
    const UINT32 freq = m.refresh_hz ? m.refresh_hz : 60;
    const UINT32 bpp = m.bits_per_pixel ? m.bits_per_pixel : 32;

    std::memset(paths, 0, sizeof(paths[0]));
    std::memset(modes, 0, sizeof(modes[0]) * need_modes);
    DISPLAYCONFIG_MODE_INFO& src = modes[0];
    src.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    src.adapterId = kAdapterLuid;
    src.id = kSourceId;
    src.sourceMode.width = m.width;
    src.sourceMode.height = m.height;
    src.sourceMode.pixelFormat = bpp == 32 ? DISPLAYCONFIG_PIXELFORMAT_32BPP
                                 : bpp == 24 ? DISPLAYCONFIG_PIXELFORMAT_24BPP
                                 : bpp == 16 ? DISPLAYCONFIG_PIXELFORMAT_16BPP
                                 : bpp == 8  ? DISPLAYCONFIG_PIXELFORMAT_8BPP
                                             : DISPLAYCONFIG_PIXELFORMAT_NONGDI;
    src.sourceMode.position = POINTL{0, 0};

    DISPLAYCONFIG_MODE_INFO& tgt = modes[1];
    tgt.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
    tgt.adapterId = kAdapterLuid;
    tgt.id = kTargetId;
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO& sig = tgt.targetMode.targetVideoSignalInfo;
    sig.pixelRate = static_cast<UINT64>(freq) * m.width * m.height;
    sig.hSyncFreq = DISPLAYCONFIG_RATIONAL{freq * static_cast<UINT32>(m.width), 1};
    sig.vSyncFreq = DISPLAYCONFIG_RATIONAL{freq, 1};
    sig.activeSize = DISPLAYCONFIG_2DREGION{static_cast<UINT32>(m.width), static_cast<UINT32>(m.height)};
    sig.totalSize = (flags & QDC_DATABASE_CURRENT) ? DISPLAYCONFIG_2DREGION{0, 0}
                                                    : DISPLAYCONFIG_2DREGION{static_cast<UINT32>(m.width), static_cast<UINT32>(m.height)};
    sig.videoStandard = static_cast<decltype(sig.videoStandard)>(255);   // D3DKMDT_VSS_OTHER
    sig.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

    DISPLAYCONFIG_PATH_INFO& path = paths[0];
    path.flags = DISPLAYCONFIG_PATH_ACTIVE;
    path.sourceInfo.adapterId = kAdapterLuid;
    path.sourceInfo.id = kSourceId;
    path.sourceInfo.statusFlags = DISPLAYCONFIG_SOURCE_IN_USE;
    path.targetInfo.adapterId = kAdapterLuid;
    path.targetInfo.id = kTargetId;
    path.targetInfo.outputTechnology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL;
    path.targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
    path.targetInfo.scaling = DISPLAYCONFIG_SCALING_IDENTITY;
    path.targetInfo.refreshRate = DISPLAYCONFIG_RATIONAL{freq, 1};
    path.targetInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    path.targetInfo.targetAvailable = TRUE;
    path.targetInfo.statusFlags = DISPLAYCONFIG_TARGET_IN_USE;
    path.sourceInfo.modeInfoIdx = 0;
    path.targetInfo.modeInfoIdx = 1;
    *paths_count = 1;
    *modes_count = need_modes;
    return ERROR_SUCCESS;
}

NTSTATUS WINAPI NtUserDisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_HEADER* packet) {
    namespace st = dispatch::detail::st;
    constexpr NTSTATUS kInvalidParameter = static_cast<NTSTATUS>(0xC000000D), kUnsuccessful = static_cast<NTSTATUS>(0xC0000001);
    if (!packet || packet->size < sizeof(*packet)) return kUnsuccessful;
    auto our_adapter = [&] { return packet->adapterId.LowPart == kAdapterLuid.LowPart && packet->adapterId.HighPart == kAdapterLuid.HighPart; };
    switch (packet->type) {
    case DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME: {
        auto* p = reinterpret_cast<DISPLAYCONFIG_SOURCE_DEVICE_NAME*>(packet);
        if (packet->size < sizeof(*p)) return kInvalidParameter;
        if (!our_adapter() || packet->id != kSourceId) return kUnsuccessful;
        wcscpy(p->viewGdiDeviceName, L"\\\\.\\DISPLAY1");
        return st::Success;
    }
    case DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME: {
        auto* p = reinterpret_cast<DISPLAYCONFIG_TARGET_DEVICE_NAME*>(packet);
        if (packet->size < sizeof(*p)) return kInvalidParameter;
        std::memset(&p->flags, 0, sizeof(*p) - offsetof(DISPLAYCONFIG_TARGET_DEVICE_NAME, flags));
        if (!our_adapter() || packet->id != kTargetId) return kUnsuccessful;
        p->outputTechnology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL;
        wcscpy(p->monitorFriendlyDeviceName, L"Display1");
        wcscpy(p->monitorDevicePath, L"\\\\?\\DISPLAY#Default_Monitor#1&0&UID0#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}");
        return st::Success;
    }
    case DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME: {
        auto* p = reinterpret_cast<DISPLAYCONFIG_ADAPTER_NAME*>(packet);
        if (packet->size < sizeof(*p)) return kInvalidParameter;
        if (!our_adapter()) return kUnsuccessful;
        wcscpy(p->adapterDevicePath, L"\\\\?\\PCI#VEN_1414&DEV_008C#0#{5b45201d-f2f2-4f3b-85bb-30ff1f953599}");
        return st::Success;
    }
    default:
        return kInvalidParameter;   // the other packet types (preferred mode, colour, white level, ...) are not answered
    }
}

BOOL WINAPI NtUserGetProcessDefaultLayout(ULONG* layout) {
    if (!layout) return FALSE;
    *layout = 0;
    return TRUE;
}

BOOL WINAPI NtUserSetProcessDefaultLayout(ULONG layout) {
    if (layout & LAYOUT_RTL) FIXME(user, "right-to-left process layout not supported");
    return TRUE;
}

BOOL WINAPI NtUserSetSysColors(INT count, const INT* colors, const COLORREF* values) {
    LockGuard lock;
    if (!colors || !values || count < 0) return FALSE;
    for (INT i = 0; i < count; ++i) SetSysColor(colors[i], values[i]);
    return TRUE;
}

BOOL WINAPI NtUserIsMouseInPointerEnabled() { return FALSE; }

}  // namespace sys
}  // namespace win10xbox::user
