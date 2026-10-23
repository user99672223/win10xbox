// The binary contract between Wine's PE user32.dll / gdi32.dll and the win32u
// syscalls core/user implements: constants, call codes and structure layouts
// as Wine 11.9 defines them in include/ntuser.h and include/ntgdi.h (both
// "not compatible with Windows" by Wine's own comment -- the DLLs are Wine's,
// so this is the contract we implement). Values only; no Wine code. Every
// layout that user32/gdi32 reads straight out of memory has a static_assert.
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace win10xbox::user::abi {

// ---- KernelCallbackTable indices (ALL_USER32_CALLBACKS order) -------------------
enum : ULONG {
    CbCallDispatchCallback, CbCallEnumDisplayMonitor, CbCallSendAsyncCallback, CbCallWinEventHook,
    CbCallWinProc, CbCallWindowsHook, CbCopyImage, CbDrawNonClientButton, CbDrawScrollBar, CbDrawText,
    CbFreeCachedClipboardData, CbImmProcessKey, CbImmTranslateMessage, CbInitBuiltinClasses, CbLoadDriver,
    CbLoadImage, CbLoadSysMenu, CbPostDDEMessage, CbRenderSynthesizedFormat, CbUnpackDDEMessage,
};

// ---- TEB.Win32ClientInfo: struct ntuser_thread_info ------------------------------
struct InputMessageSource { std::uint32_t device_type, origin_id; };
struct ThreadInfo {
    DWORD message_time;
    DWORD message_pos;
    UINT64 message_extra;
    InputMessageSource msg_source;
    WORD recursion_count;
    UINT receive_flags;
    UINT top_window;    // desktop window (HWND as 32 bits)
    UINT msg_window;    // HWND_MESSAGE parent
    UINT dpi_context;
    UINT default_imc;
    UINT64 client_imm;
    UINT64 wmchar_data;
};
static_assert(offsetof(ThreadInfo, message_extra) == 8);
static_assert(offsetof(ThreadInfo, recursion_count) == 24);
static_assert(offsetof(ThreadInfo, top_window) == 32);
static_assert(offsetof(ThreadInfo, dpi_context) == 40);
static_assert(offsetof(ThreadInfo, client_imm) == 48);
static_assert(sizeof(ThreadInfo) == 64);

// ---- struct win_proc_params (NtUserCallWinProc callback / dispatch params) -------
enum WmCharMapping : int {
    WmCharMapPostMessage, WmCharMapSendMessage, WmCharMapSendMessageTimeout, WmCharMapRecvMessage,
    WmCharMapDispatchMessage, WmCharMapCallWindowProc, WmCharMapIsDialogMessage, WmCharMapNoMapping = 7,
};
struct WinProcParams {
    WNDPROC func;
    HWND hwnd;
    UINT msg;
    WPARAM wparam;
    LPARAM lparam;
    BOOL ansi;
    BOOL ansi_dst;
    int mapping;   // enum wm_char_mapping
    UINT dpi_context;
    WNDPROC procA;
    WNDPROC procW;
};
static_assert(offsetof(WinProcParams, msg) == 16);
static_assert(offsetof(WinProcParams, wparam) == 24);
static_assert(offsetof(WinProcParams, ansi) == 40);
static_assert(offsetof(WinProcParams, dpi_context) == 52);
static_assert(offsetof(WinProcParams, procA) == 56);
static_assert(sizeof(WinProcParams) == 72);

// ---- DPI awareness contexts ---------------------------------------------------------
constexpr UINT MakeDpiContext(UINT awareness, UINT version, UINT dpi, UINT flags) {
    return awareness | (version << 4) | (dpi << 8) | flags;
}
constexpr UINT kDpiFlagGdiScaled = 0x40000000;
constexpr UINT kDpiFlagProcess = 0x80000000;
constexpr UINT kDpiUnaware = MakeDpiContext(0, 1, 96, 0);           // 0x6010
constexpr UINT kDpiSystemAware = MakeDpiContext(1, 1, 96, 0);       // system dpi is 96
constexpr UINT kDpiPerMonitorAware = MakeDpiContext(2, 1, 0, 0);
constexpr UINT kDpiPerMonitorAwareV2 = MakeDpiContext(2, 2, 0, 0);

// ---- NtUserMessageCall codes ----------------------------------------------------------
enum : DWORD {
    McScrollBarWndProc = 0x029a, McPopupMenuWndProc = 0x029c, McDesktopWindowProc = 0x029d,
    McDefWindowProc = 0x029e, McCallWindowProc = 0x02ab, McSendMessage = 0x02b1, McSendMessageTimeout = 0x02b3,
    McSendNotifyMessage = 0x02b7, McSendMessageCallback = 0x02b8, McClipboardWindowProc = 0x0300,
    McGetDispatchParams = 0x3001, McSpyGetMsgName = 0x3002, McSpyEnter = 0x0303, McSpyExit = 0x0304,
    McImeDriverCall = 0x0305, McSystemTrayCall = 0x0306, McDragDropCall = 0x0307, McPostDdeCall = 0x0308,
    McWintabDriverCall = 0x0309,
};
struct SendMessageTimeoutParams { UINT flags; UINT timeout; DWORD_PTR result; };
struct SendMessageCallbackParams { SENDASYNCPROC callback; ULONG_PTR data; };
struct SendAsyncParams { SENDASYNCPROC callback; HWND hwnd; UINT msg; ULONG_PTR data; LRESULT result; };

// ---- NtUserInitializeClientPfnArrays ---------------------------------------------------
enum ClientProc : int {
    ProcScrollBar, ProcMessage, ProcMenu, ProcDesktop, ProcDefWnd, ProcIconTitle, ProcUnknown, ProcButton,
    ProcCombo, ProcComboLBox, ProcDialog, ProcEdit, ProcListBox, ProcMDIClient, ProcStatic, ProcIme, ProcGhost,
    ProcCount
};
constexpr int kClientWorkers = 11;

// ---- NtUserCallNoParam / OneParam / TwoParam / Hwnd / HwndParam codes -------------------
enum : ULONG {
    NoParamGetDesktopWindow, NoParamGetDialogBaseUnits, NoParamGetLastInputTime, NoParamGetProgmanWindow,
    NoParamGetShellWindow, NoParamGetTaskmanWindow, NoParamDisplayModeChanged, NoParamExitingThread,
    NoParamThreadDetach,
};
enum : ULONG {
    OneParamCreateCursorIcon, OneParamEnableDC, OneParamEnableThunkLock, OneParamGetIconParam,
    OneParamGetMenuItemCount, OneParamGetPrimaryMonitorRect, OneParamGetSysColor, OneParamGetSysColorBrush,
    OneParamGetSysColorPen, OneParamGetSystemMetrics, OneParamGetVirtualScreenRect, OneParamSetKeyboardAutoRepeat,
    OneParamSetThreadDpiAwarenessContext, OneParamD3DKMTOpenAdapterFromGdiDisplayName,
    OneParamGetAsyncKeyboardState, OneParamGetDeskPattern,
};
enum : ULONG {
    TwoParamGetDialogProc, TwoParamGetMenuInfo, TwoParamGetMonitorInfo, TwoParamGetSystemMetricsForDpi,
    TwoParamMonitorFromRect, TwoParamSetIconParam, TwoParamSetIMECompositionRect, TwoParamAdjustWindowRect,
    TwoParamGetVirtualScreenRect, TwoParamAllocWinProc,
};
struct AdjustWindowRectParams { DWORD style; DWORD ex_style; BOOL menu; UINT dpi; };
enum : DWORD {
    HwndActivateOtherWindow, HwndGetDialogInfo, HwndGetDpiForWindow, HwndGetLastActivePopup,
    HwndGetMDIClientInfo, HwndGetParent, HwndGetWindowDpiAwarenessContext, HwndGetWindowInputContext,
    HwndGetWindowSysSubMenu, HwndGetWindowTextLength, HwndIsWindow, HwndIsWindowEnabled, HwndIsWindowUnicode,
    HwndIsWindowVisible, HwndSetForegroundWindowInternal, HwndGetFullWindowHandle, HwndIsCurrentProcessWindow,
    HwndIsCurrentThreadWindow,
};
enum : DWORD {
    HwndParamClientToScreen, HwndParamGetChildRect, HwndParamGetClassLongA, HwndParamGetClassLongW,
    HwndParamGetClassLongPtrA, HwndParamGetClassLongPtrW, HwndParamGetClassWord, HwndParamGetScrollInfo,
    HwndParamGetWindowInfo, HwndParamGetWindowLongA, HwndParamGetWindowLongW, HwndParamGetWindowLongPtrA,
    HwndParamGetWindowLongPtrW, HwndParamGetWindowRect, HwndParamGetClientRect, HwndParamGetPresentRect,
    HwndParamGetWindowRelative, HwndParamGetWindowThread, HwndParamGetWindowWord, HwndParamIsChild,
    HwndParamMapWindowPoints, HwndParamMirrorRgn, HwndParamMonitorFromWindow, HwndParamScreenToClient,
    HwndParamSetDialogInfo, HwndParamSetMDIClientInfo, HwndParamSendHardwareInput, HwndParamExposeWindowSurface,
    HwndParamGetWinMonitorDpi, HwndParamSetRawWindowPos, HwndParamGetPrivateData, HwndParamSetPrivateData,
};
struct GetWindowRectsParams { RECT* rect; UINT dpi; };
struct MapWindowPointsParams { HWND hwnd_to; POINT* points; UINT count; UINT dpi; };
struct GetPrivateDataParams { UINT offset; UINT size; };
struct SetPrivateDataParams { UINT offset; UINT size; LONG64 value; };

// NtUserQueryWindow / NtUserGetThreadState classes
enum : int {
    QwWindowProcess, QwWindowProcess2, QwWindowThread, QwWindowActiveWindow, QwWindowFocusWindow, QwWindowIsHung,
    QwWindowClientBase, QwWindowIsForegroundThread, QwWindowDefaultImeWindow, QwWindowDefaultInputContext,
};
enum : int {
    TsFocusWindow, TsActiveWindow, TsCaptureWindow, TsDefaultImeWindow, TsDefaultInputContext, TsInputState,
    TsCursor, TsExtraInfo, TsInSendMessage, TsMessageTime, TsIsForeground,
};

// struct cursoricon_frame / cursoricon_desc (NtUserSetCursorIconData; "not compatible with Windows")
struct CursorIconFrame {
    UINT width, height;   // frame-specific size
    HBITMAP color;        // colour bitmap
    HBITMAP alpha;        // pre-multiplied alpha bitmap of 32 bpp icons
    HBITMAP mask;         // mask bitmap (followed by the colour half for 1 bpp icons)
    POINT hotspot;
};
static_assert(sizeof(CursorIconFrame) == 40);
struct CursorIconDesc {
    UINT flags, num_steps, num_frames, delay;
    CursorIconFrame* frames;
    DWORD* frame_seq;
    DWORD* frame_rates;
    HRSRC rsrc;
};
static_assert(sizeof(CursorIconDesc) == 48);
struct DispatchCallbackParams { UINT64 callback; };
struct FreeIconParams { DispatchCallbackParams dispatch; UINT64 param; };
static_assert(sizeof(FreeIconParams) == 16);

// struct client_menu_name
struct ClientMenuName { char* nameA; WCHAR* nameW; UNICODE_STRING* nameUS; };

// WINDOWPOS flags Windows uses but does not define in the SDK headers
constexpr UINT kSwpNoClientSize = 0x0800;
constexpr UINT kSwpNoClientMove = 0x1000;
// GetDCEx: use the class style (CS_OWNDC...) -- undocumented, not in every SDK
constexpr DWORD kDcxUseStyle = 0x00010000;
// The one monitor handle (HMONITOR as 32 bits).
inline HMONITOR PrimaryMonitor() { return reinterpret_cast<HMONITOR>(static_cast<ULONG_PTR>(0x10001)); }

// Wine's internal WM_WINE_* messages and WM_SYSTIMER
constexpr UINT kWmSysTimer = 0x0118;

// ---- GDI: the shared handle table ----------------------------------------------------
#pragma pack(push, 8)
struct GdiHandleEntry {
    UINT64 Object;
    ULONG Owner;           // ProcessId:16, Lock:1, Count:15
    USHORT Unique;         // ExtType:7 | StockFlag:1 | Generation:8 (bit fields spelled out below)
    UCHAR Type;            // ExtType & 0x1f; 0 marks a free entry
    UCHAR Flags;
    UINT64 UserPointer;
};
#pragma pack(pop)
static_assert(offsetof(GdiHandleEntry, Owner) == 8);
static_assert(offsetof(GdiHandleEntry, Unique) == 12);
static_assert(offsetof(GdiHandleEntry, Type) == 14);
static_assert(offsetof(GdiHandleEntry, UserPointer) == 16);
static_assert(sizeof(GdiHandleEntry) == 24);

constexpr USHORT MakeUnique(UCHAR ext_type, bool stock, UCHAR generation) {
    return static_cast<USHORT>((ext_type & 0x7f) | (stock ? 0x80 : 0) | (generation << 8));
}
constexpr UCHAR UniqueExtType(USHORT unique) { return static_cast<UCHAR>(unique & 0x7f); }
constexpr UCHAR UniqueGeneration(USHORT unique) { return static_cast<UCHAR>(unique >> 8); }

constexpr std::uint32_t kGdiMaxHandles = 0x10000;
constexpr std::uint32_t kFirstGdiHandle = 32;   // FIRST_GDI_HANDLE (gdi32/objects.c, win32u/gdiobj.c)

constexpr DWORD kObjDc = 0x010000, kObjEnhMetaDc = 0x210000, kObjRegion = 0x040000, kObjSurf = 0x050000,
                kObjMetafile = 0x260000, kObjEnhMetafile = 0x460000, kObjMetaDc = 0x660000, kObjPal = 0x080000,
                kObjBitmap = 0x090000, kObjFont = 0x0a0000, kObjBrush = 0x100000, kObjPen = 0x300000,
                kObjExtPen = 0x500000, kObjMemDc = 0x410000;
constexpr DWORD kHandleTypeShift = 16;
constexpr DWORD kHandleTypeMask = 0x007f0000;
constexpr DWORD kHandleStockObject = 0x00800000;

// NtGdiGetDCDword / NtGdiGetAndSetDCDword / NtGdiGetDCPoint codes
enum : UINT { DcSetMapMode = 8, DcSetBkColor = 100, DcSetBkMode, DcSetTextColor, DcSetDCBrushColor,
              DcSetDCPenColor, DcSetGraphicsMode, DcSetROP2, DcSetTextAlign };
enum : UINT { DcGetArcDirection, DcGetBkColor, DcGetBkMode, DcGetDCBrushColor, DcGetDCPenColor,
              DcGetGraphicsMode, DcGetLayout, DcGetPolyFillMode, DcGetROP2, DcGetTextColor, DcIsMemDC,
              DcHasOpenGL };
enum : UINT { DcGetBrushOrgEx, DcGetCurrentPosition, DcGetDCOrg };
// NtGdiPolyPolyDraw functions
enum : UINT { PolyPolygon = 1, PolyPolyline, PolyBezier, PolyBezierTo, PolylineTo, PolyPolygonRgn };

// struct DC_ATTR: gdi32 reads and writes it directly through the handle entry's UserPointer.
// struct char_width_info (ntgdi.h): NtGdiGetCharWidthInfo.
struct CharWidthInfo { INT lsb, rsb, unk; };

struct DcAttr {
    UINT hdc;
    LONG disabled;
    int save_level;
    COLORREF background_color;
    COLORREF brush_color;
    COLORREF pen_color;
    COLORREF text_color;
    POINT cur_pos;
    INT graphics_mode;
    INT arc_direction;
    DWORD layout;
    WORD text_align;
    WORD background_mode;
    WORD poly_fill_mode;
    WORD rop_mode;
    WORD rel_abs_mode;
    WORD stretch_blt_mode;
    INT map_mode;
    INT char_extra;
    DWORD mapper_flags;
    RECT vis_rect;
    FLOAT miter_limit;
    POINT brush_org;
    POINT wnd_org;
    SIZE wnd_ext;
    POINT vport_org;
    SIZE vport_ext;
    SIZE virtual_res;
    SIZE virtual_size;
    UINT font_code_page;
    RECTL emf_bounds;
    UINT64 emf;
    UINT64 abort_proc;
    UINT64 print;
};
static_assert(offsetof(DcAttr, background_color) == 12);
static_assert(offsetof(DcAttr, cur_pos) == 28);
static_assert(offsetof(DcAttr, text_align) == 48);
static_assert(offsetof(DcAttr, map_mode) == 60);
static_assert(offsetof(DcAttr, vis_rect) == 72);
static_assert(offsetof(DcAttr, wnd_org) == 100);
static_assert(offsetof(DcAttr, vport_ext) == 124);
static_assert(offsetof(DcAttr, emf) == 168);
static_assert(sizeof(DcAttr) == 192);

}  // namespace win10xbox::user::abi
