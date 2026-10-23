// Internal interfaces of core/user (the win32k surface). Not a public header:
// only core/user/*.cpp and its unit tests include it. docs/GUI.md is the design.
#pragma once
#include "win10xbox/user.h"
#include "win10xbox/trace.h"
#include "../dispatch/internal.h"   // ThreadState, CallInGuestView, PebField, st::, teb::/peb:: offsets
#include "winexbox/driver.h"
#include "ntuser_abi.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

WIN10XBOX_EXTERN_CHANNEL(user);   // window manager / messages (WIN10XBOX_DEBUG=+user)
WIN10XBOX_EXTERN_CHANNEL(gdi);    // GDI objects / drawing (+gdi)

namespace win10xbox::user::detail {

// ---- The lock ---------------------------------------------------------------
// One lock for the whole module, like wineserver's: windows, classes, queues,
// GDI handle table and surfaces. Held for the duration of a syscall, released
// while a thread blocks in GetMessage / MsgWaitForMultipleObjects and while a
// user-mode callback runs (a window procedure makes syscalls of its own).
SRWLOCK& UserLock();
class LockGuard {
public:
    LockGuard() { AcquireSRWLockExclusive(&UserLock()); }
    ~LockGuard() { ReleaseSRWLockExclusive(&UserLock()); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
};
// The opposite: drops the (held) lock for its lifetime. Pointers into module
// state are stale afterwards: re-resolve handles once it ends.
class UnlockScope {
public:
    UnlockScope() { ReleaseSRWLockExclusive(&UserLock()); }
    ~UnlockScope() { AcquireSRWLockExclusive(&UserLock()); }
    UnlockScope(const UnlockScope&) = delete;
    UnlockScope& operator=(const UnlockScope&) = delete;
};

// ---- Rectangles and regions ---------------------------------------------------
inline bool RectEmpty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }
inline RECT RectIntersect(const RECT& a, const RECT& b) {
    RECT r{a.left > b.left ? a.left : b.left, a.top > b.top ? a.top : b.top,
           a.right < b.right ? a.right : b.right, a.bottom < b.bottom ? a.bottom : b.bottom};
    if (RectEmpty(r)) r = RECT{0, 0, 0, 0};
    return r;
}
inline RECT RectUnion(const RECT& a, const RECT& b) {
    if (RectEmpty(a)) return b;
    if (RectEmpty(b)) return a;
    return RECT{a.left < b.left ? a.left : b.left, a.top < b.top ? a.top : b.top,
                a.right > b.right ? a.right : b.right, a.bottom > b.bottom ? a.bottom : b.bottom};
}
inline RECT RectOffset(RECT r, int dx, int dy) { r.left += dx; r.right += dx; r.top += dy; r.bottom += dy; return r; }
inline bool PtInRectangle(const RECT& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// A region: non-overlapping rectangles (rgn.cpp-free, see gdi_objects.cpp).
struct Rgn {
    std::vector<RECT> rects;
    bool Empty() const { return rects.empty(); }
    RECT Box() const;
    bool Contains(int x, int y) const;
    void Set(const RECT& r) { rects.clear(); if (!RectEmpty(r)) rects.push_back(r); }
    void Offset(int dx, int dy) { for (RECT& r : rects) r = RectOffset(r, dx, dy); }
    int Complexity() const { return rects.empty() ? NULLREGION : rects.size() == 1 ? SIMPLEREGION : COMPLEXREGION; }
};
// RGN_AND / RGN_OR / RGN_XOR / RGN_DIFF / RGN_COPY
Rgn CombineRgns(const Rgn& a, const Rgn& b, int mode);

struct State;

// ---- GDI objects ---------------------------------------------------------------
struct GdiObject {
    DWORD type = 0;          // abi::kObj*
    HGDIOBJ handle = nullptr;
    bool system = false;     // stock / system objects cannot be deleted
    bool delete_pending = false;   // deleted while selected into a DC
    std::uint32_t selected = 0;    // number of DCs that have it selected
    virtual ~GdiObject() = default;
};
struct BitmapObj;
struct BrushObj : GdiObject {
    UINT style = BS_SOLID;
    COLORREF color = 0;
    ULONG_PTR hatch = 0;
    HGDIOBJ pattern = nullptr;   // BS_PATTERN: a private copy of the bitmap
};
struct PenObj : GdiObject {
    UINT style = PS_SOLID;
    int width = 0;
    COLORREF color = 0;
    bool ext = false;
};
struct FontObj : GdiObject {
    LOGFONTW lf{};
};
struct PaletteObj : GdiObject {
    std::vector<PALETTEENTRY> entries;
};
struct RegionObj : GdiObject {
    Rgn rgn;
};
struct BitmapObj : GdiObject {
    int width = 0, height = 0;
    int bpp = 32;                // what GetObject reports
    int planes = 1;
    // Pixel storage. Plain bitmaps: `pixels` (BGRA top-down, always 32 bpp internally whatever `bpp` says).
    // DIB sections: `dib_bits` in guest-visible memory in the DIB's own format.
    std::vector<std::uint32_t> pixels;
    bool dib = false;
    void* dib_bits = nullptr;
    bool dib_owned = false;      // we allocated dib_bits (VirtualAlloc)
    int dib_stride = 0;          // bytes per row, positive
    bool dib_bottom_up = false;
    BITMAPINFOHEADER dib_header{};
    DWORD dib_masks[3] = {};
    std::vector<RGBQUAD> dib_colors;
    HANDLE dib_section = nullptr;
    DWORD dib_offset = 0;
    SIZE dimension{};            // SetBitmapDimensionEx
    HDC selected_dc = nullptr;
};

// Where drawing lands: 32 bpp BGRA rows.
struct PixelTarget {
    std::uint8_t* bits = nullptr;
    int width = 0, height = 0;
    std::ptrdiff_t stride = 0;   // may be negative (bottom-up DIB sections)
    std::uint32_t* Row(int y) const { return reinterpret_cast<std::uint32_t*>(bits + stride * y); }
};

enum class DcKind : std::uint8_t { Screen, Window, Memory, Info };
struct DcState {   // what SaveDC remembers
    abi::DcAttr attr{};
    HGDIOBJ brush = nullptr, pen = nullptr, font = nullptr, bitmap = nullptr, palette = nullptr;
    bool has_clip = false;
    Rgn clip;
};
struct DcObj : GdiObject {
    abi::DcAttr* attr = nullptr;      // gdi32 reads/writes it through the handle entry's UserPointer
    DcKind kind = DcKind::Memory;
    HWND hwnd = nullptr;              // window DC: the window
    bool window_origin = false;       // DCX_WINDOW: device (0,0) is the window's top-left, not the client's
    bool cache = false;               // handed out by GetDCEx (released by ReleaseDC)
    bool own_dc = false;              // CS_OWNDC / CS_CLASSDC: lives as long as the window
    bool paint_dc = false;            // BeginPaint
    HGDIOBJ brush = nullptr, pen = nullptr, font = nullptr, bitmap = nullptr, palette = nullptr;
    bool has_clip = false;            // app clip (SelectClipRgn / IntersectClipRect), device coords
    Rgn clip;
    bool has_paint_clip = false;      // BeginPaint / GetDCEx(DCX_INTERSECTRGN): device coords
    RECT paint_clip{};
    std::vector<DcState> saved;
    bool drew = false;                // something was drawn since the last flush
    XFORM world{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};   // GM_ADVANCED world transform (only translation honoured)
    RECT bounds{};                    // SetBoundsRect accumulation (device coords)
    bool bounds_enabled = false;
};

// The process-wide handle table at PEB.GdiSharedHandleTable. Separated from
// the rest of the module state so tests can exercise the encoding alone.
class GdiHandleTable {
public:
    // `entries` must have room for abi::kGdiMaxHandles entries, zeroed.
    explicit GdiHandleTable(abi::GdiHandleEntry* entries) : entries_(entries) {}
    // Wine's encoding: LOWORD = index, HIWORD = Unique (ext type | stock bit | generation).
    HGDIOBJ Alloc(GdiObject* obj, DWORD type, void* user_pointer = nullptr);
    GdiObject* Lookup(HGDIOBJ handle, DWORD type = 0) const;   // type 0: any
    GdiObject* Free(HGDIOBJ handle);                            // returns the object, does not delete it
    void MarkStock(HGDIOBJ handle);                              // sets the stock bit; returns nothing
    HGDIOBJ FullHandle(HGDIOBJ handle) const;                   // accepts a LOWORD-only handle
    abi::GdiHandleEntry* Entries() const { return entries_; }
    std::uint32_t Live() const { return live_; }
    static HGDIOBJ EntryHandle(std::uint32_t index, USHORT unique) {
        return reinterpret_cast<HGDIOBJ>(static_cast<ULONG_PTR>(index | (static_cast<DWORD>(unique) << abi::kHandleTypeShift)));
    }
private:
    abi::GdiHandleEntry* entries_;
    std::vector<GdiObject*> objects_ = std::vector<GdiObject*>(abi::kGdiMaxHandles, nullptr);
    std::vector<std::uint32_t> free_;
    std::uint32_t next_unused_ = abi::kFirstGdiHandle;
    std::uint32_t live_ = 0;
};

// ---- Classes -----------------------------------------------------------------
struct WinProc {
    WNDPROC a = nullptr, w = nullptr;
    bool Empty() const { return !a && !w; }
};
struct ClassObj {
    ATOM atom = 0;
    std::wstring name;            // as registered (case kept), compared case-insensitively
    std::size_t name_offset = 0;  // characters of version prefix
    HINSTANCE instance = nullptr;
    bool local = true;
    bool builtin = false;
    UINT style = 0;
    WinProc proc;
    int cls_extra = 0, win_extra = 0;
    std::vector<std::uint8_t> extra;
    HICON icon = nullptr, icon_sm = nullptr;
    HCURSOR cursor = nullptr;
    HBRUSH brush = nullptr;
    abi::ClientMenuName menu{};
    std::uint32_t windows = 0;     // live windows of this class
    HDC class_dc = nullptr;        // CS_CLASSDC
};

// ---- Windows -------------------------------------------------------------------
constexpr UINT kWinNeedSize = 0x1;        // WIN_NEED_SIZE: send WM_SIZE/WM_MOVE at first show
constexpr UINT kWinDestroying = 0x2;
constexpr UINT kWinHasBeenShown = 0x4;
constexpr UINT kWinIsDesktop = 0x8;
constexpr UINT kWinIsMsgParent = 0x10;

struct WindowObj {
    HWND handle = nullptr;
    ClassObj* cls = nullptr;
    HWND parent = nullptr;            // desktop for top-level windows
    HWND owner = nullptr;
    std::vector<HWND> children;       // z-order, front() is the top
    DWORD style = 0, ex_style = 0;
    ULONG_PTR id = 0;                 // GWLP_ID (child) / menu (top-level)
    HINSTANCE instance = nullptr;
    LONG_PTR userdata = 0;
    std::wstring text;
    WinProc proc;
    bool unicode = true;
    LONG_PTR dlg_info = 0, mdi_info = 0;
    std::vector<std::uint8_t> extra;
    RECT window{}, client{};          // relative to the parent's client area
    RECT normal{};                    // restored position (parent coordinates)
    DWORD tid = 0;
    struct UserThread* thread = nullptr;
    // painting
    RECT update{};                    // client coordinates, bounding box of the update region
    bool erase_pending = false;
    bool nc_pending = false;
    bool internal_paint = false;
    UINT flags = 0;
    UINT dpi_context = abi::kDpiUnaware;
    // surface (top-level windows only); surface rect = window rect at allocation time
    std::shared_ptr<winexbox::ISurface> surface;
    RECT dirty{};                     // surface coordinates, drawn but not flushed
    bool driver_created = false;
    std::vector<std::pair<std::wstring, HANDLE>> props;   // "#atom" for atom-named props
    HICON icon = nullptr, icon_sm = nullptr;
    HDC own_dc = nullptr;
    HWND last_active_popup = nullptr;
    WORD fnid = 0;
};

// ---- Message queues ------------------------------------------------------------
struct QueuedMsg {
    MSG msg{};
    ULONG_PTR extra = 0;
    bool hardware = false;
    // Hardware input (win_message.cpp). A raw mouse message is what the driver reported: the base WM_* message,
    // the screen position in `msg.pt`, MK_* flags (+ wheel delta / X button in the high word) in wParam and the
    // top-level window (or capture window) it was routed to in `msg.hwnd`. The thread that dequeues it finishes
    // the job as Wine's process_mouse_message does (hit test, activation, WM_SETCURSOR). Key messages are final.
    bool raw = false;
    std::uint32_t seq = 0;        // identifies a raw message across the lock being dropped
    BYTE vk = 0;                  // key messages: the physical key (VK_LSHIFT, ...) for the keyboard state
};
struct Timer {
    HWND hwnd = nullptr;
    UINT_PTR id = 0;
    UINT msg = WM_TIMER;
    UINT timeout = 0;
    ULONGLONG next = 0;
    TIMERPROC proc = nullptr;
};
struct SentMsg {                      // cross-thread SendMessage
    HWND hwnd; UINT msg; WPARAM wparam; LPARAM lparam; bool ansi;
    LRESULT result = 0;
    bool done = false;
    bool abandoned = false;           // the sender gave up (timeout): receiver frees it
    HANDLE done_event = nullptr;
    DWORD sender_tid = 0;
};

// ---- Per-thread state ----------------------------------------------------------
// Lives behind TEB.Win32ClientInfo[61]; the first bytes of Win32ClientInfo are
// user32's `struct ntuser_thread_info` (abi::ThreadInfo), written by the window
// manager (message_time, message_pos, top_window, dpi_context, ...).
struct CallbackFrame {
    std::vector<std::uint8_t> result;   // what NtCallbackReturn copied
    NTSTATUS status = 0;
    bool returned = false;
};
struct UserThread {
    DWORD tid = 0;
    std::vector<CallbackFrame> callbacks;   // nesting stack, index = depth
    std::uint32_t callback_depth = 0;
    abi::ThreadInfo* info = nullptr;        // TEB.Win32ClientInfo of this thread
    // queue
    std::deque<QueuedMsg> posted;
    std::deque<QueuedMsg> input;
    std::deque<SentMsg*> sent;              // incoming cross-thread sends
    std::vector<Timer> timers;
    bool quit = false;
    int quit_code = 0;
    HANDLE wake = nullptr;                  // auto-reset: something arrived for this thread
    DWORD changed_bits = 0;                 // QS_* since the last GetQueueStatus
    // focus
    HWND focus = nullptr, active = nullptr, capture = nullptr;
    bool exiting = false;
    std::uint32_t send_depth = 0;           // InSendMessage
    // hooks (win_hook.cpp): the hook procedure being called on this thread (CallNextHookEx continues from it)
    HHOOK cur_hook = nullptr;
    bool cur_hook_unicode = true;
    std::uint32_t hook_depth = 0;
};
// A SetWindowsHookEx hook (win_hook.cpp). tid 0: every thread of the process. `owner` installed it.
struct HookObj {
    HHOOK handle = nullptr;
    int id = 0;
    HOOKPROC proc = nullptr;
    DWORD tid = 0, owner = 0;
    bool unicode = true;
    bool removed = false;     // unhooked while a hook call was running: swept when the last call returns
};
// The calling thread's record, created on first use. Call with the lock held.
UserThread& CurrentUserThread();
// NULL when this thread never touched the module.
UserThread* FindUserThread();
UserThread* FindUserThreadById(DWORD tid);   // lock held
abi::ThreadInfo* CurrentThreadInfo();

// ---- Module state (allocated under the lock on first use) ---------------------------
struct RetainedLayer { std::shared_ptr<winexbox::ISurface> surface; RECT screen; };
struct State {
    // GDI
    abi::GdiHandleEntry* gdi_entries = nullptr;
    GdiHandleTable* gdi = nullptr;
    std::vector<abi::DcAttr*> dc_attr_free;
    HGDIOBJ stock[24] = {};
    HBRUSH sys_brush[31] = {};
    HPEN sys_pen[31] = {};
    // classes / atoms
    std::vector<ClassObj*> classes;
    std::vector<std::wstring> atoms;          // index + 0xC000 = atom
    WinProc client_procs[abi::ProcCount];
    HINSTANCE user32 = nullptr;
    bool builtins_registered = false;
    // windows
    std::vector<WindowObj*> windows;          // slot = index
    std::vector<WORD> generations;
    std::vector<std::uint32_t> free_slots;
    HWND desktop = nullptr;
    HWND msg_parent = nullptr;
    HWND foreground = nullptr;
    // threads
    std::vector<UserThread*> threads;
    // process
    UINT process_dpi_context = abi::kDpiUnaware;
    bool process_dpi_set = false;
    UINT_PTR next_timer_id = 0x7fff;
    // hooks, oldest first (the chain runs newest first); live hooks per WH_ id (index id - WH_MINHOOK)
    std::vector<HookObj*> hooks;
    int hook_count[16] = {};
    std::uint32_t hook_calls_active = 0;
    ULONG_PTR next_hook = 0;
    POINT cursor{};
    BYTE key_state[256] = {};                 // async key state (GetAsyncKeyState): 0x80 down, 0x01 toggled; follows the hardware
    BYTE input_key_state[256] = {};           // the state as of the last input message an application took (GetKeyState, TranslateMessage)
    std::uint32_t input_seq = 0;
    DWORD last_input_time = 0;
    // display
    int screen_w = 0, screen_h = 0;
    bool sink_installed = false;
    // last painted frame, for screenshots: the top-level layers at the most recent flush, bottom first
    std::vector<RetainedLayer> retained;
    bool retained_valid = false;
};
// Creates the state, the GDI table (installed at PEB.GdiSharedHandleTable),
// the stock objects and the system colour brushes on first call. Lock held.
State& S();
// Without creating anything (null before the first syscall). Lock held.
State* StateIfAny();
// Installs PEB.GdiSharedHandleTable for a guest that is about to start (process
// Launch); idempotent. Takes the lock itself.
void InstallProcessTables();

winexbox::IWinexboxDriver* Driver();          // the host's display driver (may be null)
void BumpStat(std::uint64_t Stats::*field, std::int64_t delta = 1);

// ---- Errors --------------------------------------------------------------------------
// The guest-visible last error of the current syscall (the dispatcher restores
// the TEB value around every syscall; this overrides it).
void SetLastErr(DWORD error);
void NoteUnimplemented(const char* name);

// ---- User-mode callbacks (KeUserModeCallback, in-process) ----------------------
// Calls PEB.KernelCallbackTable[id](args, len) -- a user32 entry point -- with
// the thread in the GUEST TLS view (the procedure is guest code and makes
// syscalls, which nest normally). The callback ends with NtCallbackReturn
// (emulated here: the result bytes are copied into the thread's CallbackFrame
// before the procedure's stack frame is gone); *ret_ptr / *ret_len then point at
// that copy, valid until the next callback at the same nesting depth on this
// thread. Returns the status NtCallbackReturn was given, or STATUS_NOT_IMPLEMENTED
// when user32 has not installed its table yet. Call with the user lock
// RELEASED.
NTSTATUS UserModeCallback(ULONG id, const void* args, ULONG len, void** ret_ptr, ULONG* ret_len);
NTSTATUS NTAPI User_NtCallbackReturn(void* result, ULONG size, NTSTATUS status);

// ---- Shared helpers across the files (lock held unless stated) -----------------------
// gdi_objects.cpp
GdiObject* GdiGet(HGDIOBJ h, DWORD type = 0);
template <class T> T* GdiGetT(HGDIOBJ h, DWORD type) { return static_cast<T*>(GdiGet(h, type)); }
HGDIOBJ GdiAlloc(GdiObject* obj, DWORD type, void* user_pointer = nullptr);
bool GdiDelete(HGDIOBJ h);                     // NtGdiDeleteObjectApp semantics
void GdiReleaseSelected(HGDIOBJ h);            // a DC stopped using it (finishes a pending delete)
HBRUSH CreateSolidBrushObj(COLORREF color, bool system);
HBRUSH SysColorBrush(int index);
COLORREF SysColor(int index);
HPEN SysColorPen(int index);
std::uint32_t BitmapPixel(const BitmapObj& bmp, int x, int y);   // BGRA
bool BitmapTarget(BitmapObj& bmp, PixelTarget* out);              // 32 bpp storage only
void SetBitmapPixel(BitmapObj& bmp, int x, int y, std::uint32_t bgra);
void SetSysColor(int index, COLORREF c);
void CreateGdiTable(State& s);
// A DIB described by a BITMAPINFO (application memory), read pixel by pixel.
struct DibView {
    int width = 0, height = 0, bpp = 0, stride = 0;
    bool bottom_up = false;
    const std::uint8_t* bits = nullptr;
    DWORD masks[3] = {};
    std::vector<RGBQUAD> colors;
};
bool MakeDibView(const BITMAPINFO* bmi, UINT coloruse, const void* bits, DibView* out);
std::uint32_t DibViewPixel(const DibView& v, int x, int y);   // y counts from the top of the image
// gdi_dc.cpp
DcObj* DcGet(HDC hdc);
HDC CreateDcObj(DcKind kind, HWND hwnd);
void FreeDcObj(HDC hdc);                       // no checks
// What a DC draws on, in device coordinates: `origin` maps device (0,0) into the target.
struct DrawTarget {
    PixelTarget px;
    POINT origin{};
    Rgn clip;                                  // device coordinates
    HWND top = nullptr;                        // window DC: the top-level window that owns the surface
};
bool ResolveDrawTarget(DcObj* dc, DrawTarget* out);
// The text cell of the DC's selected font: the built-in 8x16 face, scaled by a whole factor for
// LOGFONT heights of 32 and up (font8x16.h).
struct TextCell { int scale, width, height, ascent, descent; };
TextCell TextCellOf(const DcObj* dc);
POINT LogicalToDevice(const DcObj* dc, int x, int y);
int LogicalToDeviceDx(const DcObj* dc, int dx);
int LogicalToDeviceDy(const DcObj* dc, int dy);
void NoteDrawn(DcObj* dc, const DrawTarget& t, RECT device_rect);   // dirty tracking
// gdi_draw.cpp
void FillTargetRect(const DrawTarget& t, RECT device_rect, std::uint32_t bgra);
void DrawGlyphRun(const DrawTarget& t, RECT limit, int x, int y, const std::wstring& text, std::uint32_t fg);
std::uint32_t ColorToPixel(COLORREF c);
COLORREF PixelToColor(std::uint32_t px);

// win_class.cpp
ClassObj* FindClass(HINSTANCE instance, const WCHAR* name, std::size_t len);   // len in chars
ClassObj* FindClassByAtom(ATOM atom);
void RegisterBuiltinClasses();
ATOM AddAtom(const WCHAR* name, std::size_t len);
std::wstring AtomName(ATOM atom);
ATOM FindAtom(const WCHAR* name, std::size_t len);
ULONG_PTR GetClassLongSize(HWND hwnd, INT offset, UINT size, bool ansi);
WinProc ResolveWinProc(WNDPROC func, bool ansi);   // fills the other half for known builtin procs

// win_window.cpp
// HWND = (0x20 + slot) | (generation << 16), generation 1..0x7fff (win_window.cpp).
HWND EncodeHwnd(std::uint32_t slot, WORD generation);
bool DecodeHwnd(HWND hwnd, std::uint32_t* slot, WORD* generation);
WindowObj* Win(HWND hwnd);                     // full or short handle; null if invalid
WindowObj* WinChecked(HWND hwnd);              // sets ERROR_INVALID_WINDOW_HANDLE when invalid
HWND DesktopWindow();                          // creates it on first use
HWND MessageParentWindow();
HWND FullHandle(HWND hwnd);
bool IsTopLevel(const WindowObj* w);
HWND TopLevelOf(HWND hwnd);
POINT ClientOriginScreen(const WindowObj* w);  // screen coords of the client's (0,0)
RECT WindowRectScreen(const WindowObj* w);
RECT ClientRectScreen(const WindowObj* w);
bool IsWindowReallyVisible(const WindowObj* w); // window and every ancestor WS_VISIBLE
HWND WindowFromScreenPoint(POINT pt);
HWND ShallowWindowFromPoint(POINT pt);          // Wine's shallow_window_from_point: the top-level window at pt, else the desktop
// Every window containing screen point `pt` below `scope`, deepest child first, then siblings in z-order, then
// `scope` itself last (the order of Wine's get_window_children_from_point request).
void WindowsFromPoint(HWND scope, POINT pt, std::vector<HWND>* out);
BOOL SetWindowPosInternal(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags);
BOOL ShowWindowInternal(HWND hwnd, int cmd);
BOOL DestroyWindowInternal(HWND hwnd);
HWND SetFocusInternal(HWND hwnd);
bool SetActiveWindowInternal(HWND hwnd, bool mouse);
void DestroyThreadWindows(UserThread* t);
LRESULT DefWindowProcInternal(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool ansi);
void SetWindowTextInternal(WindowObj* w, const WCHAR* text);
void AdjustRectForStyle(RECT* rect, DWORD style, bool menu, DWORD ex_style);   // window from client
// The non-client frame: thickness of the border (sizing frame plus the border line) around the caption and client,
// the caption height (the caption's separator line included), and the rectangle of a caption button
// (HTCLOSE / HTMAXBUTTON / HTMINBUTTON) in window coordinates; false when the window has no such button.
int FrameInset(const WindowObj* w);
int CaptionHeightOf(const WindowObj* w);
bool CaptionButtonRect(const WindowObj* w, int hittest, RECT* out);
void ForEachWindowPreorder(HWND root, const std::function<bool(WindowObj*)>& fn);

// win_message.cpp
// Sends `msg` to `hwnd` and waits for the result: same thread -> the window
// procedure through the NtUserCallWinProc callback (lock dropped around it);
// other thread -> queued, the lock dropped while waiting. Lock held on entry
// and exit. `ansi` describes wparam/lparam (strings).
LRESULT SendMessageInternal(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool ansi = false);
LRESULT CallWindowProcInternal(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool ansi);
bool PostMessageInternal(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
bool PostThreadMessageInternal(UserThread* t, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool hardware = false);
void WakeThread(UserThread* t, DWORD qs_bits);
void KillWindowTimers(HWND hwnd);
void InstallEventSink();                       // driver -> input messages (lock held)

// win_hook.cpp
bool IsHooked(int id);                         // any live hook of this WH_ id (lock held)
// Runs the hook chain `id` for the calling thread, newest hook first, until one does not call CallNextHookEx.
// `lparam_size` > 0: lparam points at that many bytes, copied for the hook procedure and copied back after.
// Returns the first hook's result (0 when there is none). Lock held on entry and exit, dropped around the call.
LRESULT CallHooks(int id, int code, WPARAM wparam, LPARAM lparam, std::size_t lparam_size);
// CallWindowProcInternal wrapped in WH_CALLWNDPROC / WH_CALLWNDPROCRET.
LRESULT CallWindowProcHooked(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool ansi, bool sent_by_this_thread);
void RemoveThreadHooks(DWORD tid);             // the thread exits

// win_paint.cpp
void InvalidateWindow(HWND hwnd, const RECT* client_rect, UINT rdw_flags);
bool UpdateWindowNow(HWND hwnd, UINT rdw_flags);   // RDW_UPDATENOW / RDW_ERASENOW
HWND FindPaintWindow(UserThread* t);          // a window of `t` that needs WM_PAINT
void EnsureSurface(WindowObj* w);              // top-level: (re)allocate on show / resize
void ReleaseSurface(WindowObj* w);
void FlushWindow(WindowObj* top);              // dirty pixels -> driver, FrameSerial
void FlushAllDirty();
void NoteScreenChanged();                      // FrameSerial++ (window shown/moved/destroyed)
HDC GetDCInternal(HWND hwnd, HRGN clip, DWORD flags);
int ReleaseDCInternal(HWND hwnd, HDC hdc);
void SendEraseMessages(HWND hwnd, HDC hdc_in, bool* erased);   // WM_NCPAINT + WM_ERASEBKGND as needed
void PaintNonClient(HWND hwnd);
HDC BeginPaintInternal(HWND hwnd, PAINTSTRUCT* ps);
BOOL EndPaintInternal(HWND hwnd, const PAINTSTRUCT* ps);

// win_sysparams.cpp
int SystemMetric(int index);
void ScreenRect(RECT* out);
void WorkAreaRect(RECT* out);
bool SysParamsInfo(UINT action, UINT val, void* ptr, UINT winini);

}  // namespace win10xbox::user::detail
