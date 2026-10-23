// Every win32u syscall core/user implements, with the signature Wine's
// include/ntuser.h / include/ntgdi.h give it (the dispatcher calls these with
// the guest's own argument registers and stack slots, and hands RAX back
// whole). W10X_USER_SYSCALLS is the registration list (user.cpp, UserImpls);
// a name not in it stays Todo in the dispatcher.
#pragma once
#include "ntuser_abi.h"

namespace win10xbox::user::sys {

// ---- win_class.cpp ----
NTSTATUS WINAPI NtUserInitializeClientPfnArrays(const WNDPROC* procsA, const WNDPROC* procsW, const WNDPROC* workers,
                                                HINSTANCE user_module);
ATOM WINAPI NtUserRegisterClassExWOW(const WNDCLASSEXW* wc, UNICODE_STRING* name, UNICODE_STRING* version,
                                     abi::ClientMenuName* menu_name, DWORD fnid, DWORD flags, DWORD* wow);
BOOL WINAPI NtUserUnregisterClass(UNICODE_STRING* name, HINSTANCE instance, abi::ClientMenuName* menu_name);
ATOM WINAPI NtUserGetClassInfoEx(HINSTANCE instance, UNICODE_STRING* name, WNDCLASSEXW* wc,
                                 abi::ClientMenuName* menu_name, BOOL ansi);
INT WINAPI NtUserGetClassName(HWND hwnd, BOOL real, UNICODE_STRING* name);
ULONG WINAPI NtUserGetAtomName(ATOM atom, UNICODE_STRING* name);
ATOM WINAPI NtUserRegisterWindowMessage(UNICODE_STRING* name);
DWORD WINAPI NtUserSetClassLong(HWND hwnd, INT offset, LONG newval, BOOL ansi);
ULONG_PTR WINAPI NtUserSetClassLongPtr(HWND hwnd, INT offset, LONG_PTR newval, BOOL ansi);
WORD WINAPI NtUserSetClassWord(HWND hwnd, INT offset, WORD newval);

// ---- win_window.cpp ----
HWND WINAPI NtUserCreateWindowEx(DWORD ex_style, UNICODE_STRING* class_name, UNICODE_STRING* version,
                                 UNICODE_STRING* window_name, DWORD style, INT x, INT y, INT cx, INT cy, HWND parent,
                                 HMENU menu, HINSTANCE class_instance, void* params, DWORD flags, HINSTANCE instance,
                                 const WCHAR* class_ptr, BOOL ansi);
BOOL WINAPI NtUserDestroyWindow(HWND hwnd);
BOOL WINAPI NtUserShowWindow(HWND hwnd, INT cmd);
BOOL WINAPI NtUserShowWindowAsync(HWND hwnd, INT cmd);
BOOL WINAPI NtUserSetWindowPos(HWND hwnd, HWND after, INT x, INT y, INT cx, INT cy, UINT flags);
BOOL WINAPI NtUserMoveWindow(HWND hwnd, INT x, INT y, INT cx, INT cy, BOOL repaint);
LONG WINAPI NtUserSetWindowLong(HWND hwnd, INT offset, LONG newval, BOOL ansi);
LONG_PTR WINAPI NtUserSetWindowLongPtr(HWND hwnd, INT offset, LONG_PTR newval, BOOL ansi);
WORD WINAPI NtUserSetWindowWord(HWND hwnd, INT offset, WORD newval);
ULONG_PTR WINAPI NtUserCallHwnd(HWND hwnd, DWORD code);
ULONG_PTR WINAPI NtUserCallHwndParam(HWND hwnd, DWORD_PTR param, DWORD code);
ULONG_PTR WINAPI NtUserCallNoParam(ULONG code);
ULONG_PTR WINAPI NtUserCallOneParam(ULONG_PTR arg, ULONG code);
ULONG_PTR WINAPI NtUserCallTwoParam(ULONG_PTR arg1, ULONG_PTR arg2, ULONG code);
HWND WINAPI NtUserGetAncestor(HWND hwnd, UINT type);
HWND WINAPI NtUserSetParent(HWND hwnd, HWND parent);
HWND WINAPI NtUserFindWindowEx(HWND parent, HWND child, UNICODE_STRING* cls, UNICODE_STRING* title, ULONG unk);
NTSTATUS WINAPI NtUserBuildHwndList(HDESK desktop, HWND hwnd, BOOL children, BOOL non_immersive, ULONG thread_id,
                                    ULONG count, HWND* buffer, ULONG* size);
HANDLE WINAPI NtUserQueryWindow(HWND hwnd, int cls);
HWND WINAPI NtUserWindowFromPoint(LONG x, LONG y);
HWND WINAPI NtUserChildWindowFromPointEx(HWND parent, LONG x, LONG y, UINT flags);
HWND WINAPI NtUserRealChildWindowFromPoint(HWND parent, LONG x, LONG y);
HWND WINAPI NtUserSetFocus(HWND hwnd);
HWND WINAPI NtUserSetActiveWindow(HWND hwnd);
HWND WINAPI NtUserGetForegroundWindow();
BOOL WINAPI NtUserSetForegroundWindow(HWND hwnd);
HWND WINAPI NtUserSetCapture(HWND hwnd);
BOOL WINAPI NtUserReleaseCapture();
BOOL WINAPI NtUserEnableWindow(HWND hwnd, BOOL enable);
INT WINAPI NtUserInternalGetWindowText(HWND hwnd, WCHAR* text, INT count);
BOOL WINAPI NtUserSetProp(HWND hwnd, const WCHAR* str, HANDLE handle);
HANDLE WINAPI NtUserGetProp(HWND hwnd, const WCHAR* str);
HANDLE WINAPI NtUserRemoveProp(HWND hwnd, const WCHAR* str);
BOOL WINAPI NtUserGetWindowPlacement(HWND hwnd, WINDOWPLACEMENT* placement);
BOOL WINAPI NtUserSetWindowPlacement(HWND hwnd, const WINDOWPLACEMENT* placement);
BOOL WINAPI NtUserSetWindowFNID(HWND hwnd, WORD fnid);
BOOL WINAPI NtUserGetGUIThreadInfo(DWORD id, GUITHREADINFO* info);
BOOL WINAPI NtUserFlashWindowEx(FLASHWINFO* info);
BOOL WINAPI NtUserIsChildWindowDpiMessageEnabled(HWND hwnd);
BOOL WINAPI NtUserGetTitleBarInfo(HWND hwnd, TITLEBARINFO* info);
BOOL WINAPI NtUserSetMenu(HWND hwnd, HMENU menu);
HMENU WINAPI NtUserGetSystemMenu(HWND hwnd, BOOL revert);
BOOL WINAPI NtUserDrawMenuBar(HWND hwnd);
DWORD WINAPI NtUserGetWindowContextHelpId(HWND hwnd);
BOOL WINAPI NtUserSetWindowContextHelpId(HWND hwnd, DWORD id);
void WINAPI NtUserNotifyWinEvent(DWORD event, HWND hwnd, LONG object_id, LONG child_id);
BOOL WINAPI NtUserShowOwnedPopups(HWND owner, BOOL show);
HICON WINAPI NtUserInternalGetWindowIcon(HWND hwnd, UINT type);

// ---- win_menu.cpp ----
HMENU WINAPI NtUserCreateMenu();
HMENU WINAPI NtUserCreatePopupMenu();
BOOL WINAPI NtUserDestroyMenu(HMENU handle);
BOOL WINAPI NtUserRemoveMenu(HMENU handle, UINT id, UINT flags);
BOOL WINAPI NtUserDeleteMenu(HMENU handle, UINT id, UINT flags);
DWORD WINAPI NtUserCheckMenuItem(HMENU handle, UINT id, UINT flags);
BOOL WINAPI NtUserEnableMenuItem(HMENU handle, UINT id, UINT flags);
BOOL WINAPI NtUserSetMenuDefaultItem(HMENU handle, UINT item, UINT bypos);
BOOL WINAPI NtUserHiliteMenuItem(HWND hwnd, HMENU handle, UINT item, UINT hilite);
BOOL WINAPI NtUserGetMenuItemRect(HWND hwnd, HMENU handle, UINT item, RECT* rect);
INT WINAPI NtUserMenuItemFromPoint(HWND hwnd, HMENU handle, int x, int y);
BOOL WINAPI NtUserGetMenuBarInfo(HWND hwnd, LONG id, LONG item, MENUBARINFO* info);
UINT WINAPI NtUserThunkedMenuItemInfo(HMENU handle, UINT pos, UINT flags, UINT method, MENUITEMINFOW* info,
                                      UNICODE_STRING* str);
BOOL WINAPI NtUserThunkedMenuInfo(HMENU handle, const MENUINFO* info);
BOOL WINAPI NtUserTrackPopupMenuEx(HMENU handle, UINT flags, INT x, INT y, HWND hwnd, TPMPARAMS* params);
BOOL WINAPI NtUserEndMenu();

// ---- win_message.cpp ----
BOOL WINAPI NtUserGetMessage(MSG* msg, HWND hwnd, UINT first, UINT last);
BOOL WINAPI NtUserPeekMessage(MSG* msg, HWND hwnd, UINT first, UINT last, UINT flags);
BOOL WINAPI NtUserPostMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
BOOL WINAPI NtUserPostThreadMessage(DWORD thread, UINT msg, WPARAM wparam, LPARAM lparam);
BOOL WINAPI NtUserPostQuitMessage(INT exit_code);
LRESULT WINAPI NtUserMessageCall(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, void* result_info, DWORD type,
                                 BOOL ansi);
LRESULT WINAPI NtUserDispatchMessage(const MSG* msg);
BOOL WINAPI NtUserTranslateMessage(const MSG* msg, UINT flags);
UINT_PTR WINAPI NtUserSetTimer(HWND hwnd, UINT_PTR id, UINT timeout, TIMERPROC proc, ULONG tolerance);
BOOL WINAPI NtUserKillTimer(HWND hwnd, UINT_PTR id);
UINT_PTR WINAPI NtUserSetSystemTimer(HWND hwnd, UINT_PTR id, UINT timeout);
BOOL WINAPI NtUserKillSystemTimer(HWND hwnd, UINT_PTR id);
ULONG_PTR WINAPI NtUserGetThreadState(int cls);
DWORD WINAPI NtUserGetQueueStatus(UINT flags);
BOOL WINAPI NtUserWaitMessage();
DWORD WINAPI NtUserMsgWaitForMultipleObjectsEx(DWORD count, const HANDLE* handles, DWORD timeout, DWORD mask,
                                               DWORD flags);
DWORD WINAPI NtUserWaitForInputIdle(HANDLE process, DWORD timeout, BOOL wow);
SHORT WINAPI NtUserGetKeyState(INT vkey);
SHORT WINAPI NtUserGetAsyncKeyState(INT key);
BOOL WINAPI NtUserGetKeyboardState(BYTE* state);
BOOL WINAPI NtUserSetKeyboardState(BYTE* state);
BOOL WINAPI NtUserGetCursorPos(POINT* pt);
BOOL WINAPI NtUserSetCursorPos(INT x, INT y);
HCURSOR WINAPI NtUserSetCursor(HCURSOR cursor);
HCURSOR WINAPI NtUserGetCursor();
INT WINAPI NtUserShowCursor(BOOL show);
BOOL WINAPI NtUserClipCursor(const RECT* rect);
BOOL WINAPI NtUserGetClipCursor(RECT* rect);
HKL WINAPI NtUserGetKeyboardLayout(DWORD thread_id);
UINT WINAPI NtUserGetKeyboardLayoutList(INT size, HKL* layouts);
BOOL WINAPI NtUserGetKeyboardLayoutName(WCHAR* name);
HKL WINAPI NtUserActivateKeyboardLayout(HKL layout, UINT flags);
UINT WINAPI NtUserMapVirtualKeyEx(UINT code, UINT type, HKL layout);
INT WINAPI NtUserToUnicodeEx(UINT virt, UINT scan, const BYTE* state, WCHAR* str, int size, UINT flags, HKL layout);
WORD WINAPI NtUserVkKeyScanEx(WCHAR chr, HKL layout);
INT WINAPI NtUserGetKeyNameText(LONG lparam, WCHAR* buffer, INT size);
UINT WINAPI NtUserSendInput(UINT count, INPUT* inputs, int size);
BOOL WINAPI NtUserTrackMouseEvent(TRACKMOUSEEVENT* info);
UINT WINAPI NtUserGetDoubleClickTime();
BOOL WINAPI NtUserMessageBeep(UINT type);
BOOL WINAPI NtUserCreateCaret(HWND hwnd, HBITMAP bitmap, int width, int height);
BOOL WINAPI NtUserDestroyCaret();
BOOL WINAPI NtUserShowCaret(HWND hwnd);
BOOL WINAPI NtUserHideCaret(HWND hwnd);
BOOL WINAPI NtUserSetCaretPos(INT x, INT y);
BOOL WINAPI NtUserGetCaretPos(POINT* point);
UINT WINAPI NtUserGetCaretBlinkTime();
HWND WINAPI NtUserGetOpenClipboardWindow();

// ---- win_paint.cpp ----
HDC WINAPI NtUserBeginPaint(HWND hwnd, PAINTSTRUCT* ps);
BOOL WINAPI NtUserEndPaint(HWND hwnd, const PAINTSTRUCT* ps);
HDC WINAPI NtUserGetDC(HWND hwnd);
HDC WINAPI NtUserGetDCEx(HWND hwnd, HRGN clip_rgn, DWORD flags);
HDC WINAPI NtUserGetWindowDC(HWND hwnd);
INT WINAPI NtUserReleaseDC(HWND hwnd, HDC hdc);
BOOL WINAPI NtUserInvalidateRect(HWND hwnd, const RECT* rect, BOOL erase);
BOOL WINAPI NtUserInvalidateRgn(HWND hwnd, HRGN hrgn, BOOL erase);
BOOL WINAPI NtUserValidateRect(HWND hwnd, const RECT* rect);
BOOL WINAPI NtUserValidateRgn(HWND hwnd, HRGN hrgn);
BOOL WINAPI NtUserRedrawWindow(HWND hwnd, const RECT* rect, HRGN hrgn, UINT flags);
BOOL WINAPI NtUserGetUpdateRect(HWND hwnd, RECT* rect, BOOL erase);
INT WINAPI NtUserGetUpdateRgn(HWND hwnd, HRGN hrgn, BOOL erase);
INT WINAPI NtUserExcludeUpdateRgn(HDC hdc, HWND hwnd);
HWND WINAPI NtUserWindowFromDC(HDC hdc);
BOOL WINAPI NtUserLockWindowUpdate(HWND hwnd);
INT WINAPI NtUserScrollWindowEx(HWND hwnd, INT dx, INT dy, const RECT* rect, const RECT* clip_rect, HRGN update_rgn,
                                RECT* update_rect, UINT flags);

// ---- win_sysparams.cpp ----
ULONG WINAPI NtUserGetSystemDpiForProcess(HANDLE process);
ULONG WINAPI NtUserGetProcessDpiAwarenessContext(HANDLE process);
BOOL WINAPI NtUserSetProcessDpiAwarenessContext(ULONG awareness, ULONG unknown);
BOOL WINAPI NtUserSystemParametersInfo(UINT action, UINT val, void* ptr, UINT winini);
BOOL WINAPI NtUserSystemParametersInfoForDpi(UINT action, UINT val, PVOID ptr, UINT winini, UINT dpi);
BOOL WINAPI NtUserEnumDisplayMonitors(HDC hdc, RECT* rect, MONITORENUMPROC proc, LPARAM lp);
BOOL WINAPI NtUserGetDpiForMonitor(HMONITOR monitor, UINT type, UINT* x, UINT* y);
BOOL WINAPI NtUserEnumDisplaySettings(UNICODE_STRING* device, DWORD mode, DEVMODEW* dev_mode, DWORD flags);
NTSTATUS WINAPI NtUserEnumDisplayDevices(UNICODE_STRING* device, DWORD index, DISPLAY_DEVICEW* info, DWORD flags);
LONG WINAPI NtUserGetDisplayConfigBufferSizes(UINT32 flags, UINT32* num_path_info, UINT32* num_mode_info);
BOOL WINAPI NtUserGetProcessDefaultLayout(ULONG* layout);
BOOL WINAPI NtUserSetProcessDefaultLayout(ULONG layout);
BOOL WINAPI NtUserSetSysColors(INT count, const INT* colors, const COLORREF* values);
BOOL WINAPI NtUserIsMouseInPointerEnabled();

// ---- gdi_objects.cpp ----
HBRUSH WINAPI NtGdiCreateSolidBrush(COLORREF color, HBRUSH brush);
HBRUSH WINAPI NtGdiCreateHatchBrushInternal(INT style, COLORREF color, BOOL pen);
HBRUSH WINAPI NtGdiCreatePatternBrushInternal(HBITMAP hbitmap, BOOL pen, BOOL is_8x8);
HPEN WINAPI NtGdiCreatePen(INT style, INT width, COLORREF color, HBRUSH brush);
HPEN WINAPI NtGdiExtCreatePen(DWORD style, DWORD width, ULONG brush_style, ULONG color, ULONG_PTR client_hatch,
                              ULONG_PTR hatch, DWORD style_count, const DWORD* style_bits, ULONG dib_size,
                              BOOL old_style, HBRUSH brush);
HFONT WINAPI NtGdiHfontCreate(const void* logfont, ULONG unk2, ULONG unk3, ULONG unk4, void* data);
HBITMAP WINAPI NtGdiCreateBitmap(INT width, INT height, UINT planes, UINT bpp, const void* bits);
HBITMAP WINAPI NtGdiCreateCompatibleBitmap(HDC hdc, INT width, INT height);
HBITMAP WINAPI NtGdiCreateDIBSection(HDC hdc, HANDLE section, DWORD offset, const BITMAPINFO* bmi, UINT usage,
                                     UINT header_size, ULONG flags, ULONG_PTR color_space, void** bits);
HBITMAP WINAPI NtGdiCreateDIBitmapInternal(HDC hdc, INT width, INT height, DWORD init, const void* bits,
                                           const BITMAPINFO* data, UINT coloruse, UINT max_info, UINT max_bits,
                                           ULONG flags, HANDLE xform);
BOOL WINAPI NtGdiDeleteObjectApp(HGDIOBJ obj);
INT WINAPI NtGdiExtGetObjectW(HGDIOBJ handle, INT count, void* buffer);
BOOL WINAPI NtGdiUnrealizeObject(HGDIOBJ obj);
LONG WINAPI NtGdiGetBitmapBits(HBITMAP bitmap, LONG count, void* bits);
LONG WINAPI NtGdiSetBitmapBits(HBITMAP hbitmap, LONG count, const void* bits);
BOOL WINAPI NtGdiGetBitmapDimension(HBITMAP bitmap, SIZE* size);
BOOL WINAPI NtGdiSetBitmapDimension(HBITMAP hbitmap, INT x, INT y, SIZE* prev_size);
INT WINAPI NtGdiGetDIBitsInternal(HDC hdc, HBITMAP hbitmap, UINT startscan, UINT lines, void* bits, BITMAPINFO* info,
                                  UINT coloruse, UINT max_bits, UINT max_info);
// Not a Wine 11.9 win32u syscall (gdi32's SetDIBits goes through other entry points); kept as the internal
// helper CreateDIBitmap's CBM_INIT path uses, and therefore not in W10X_USER_SYSCALLS.
INT WINAPI NtGdiSetDIBitsInternal(HDC hdc, HBITMAP hbitmap, UINT startscan, UINT lines, const void* bits,
                                  const BITMAPINFO* info, UINT coloruse, UINT max_bits, UINT max_info);
HRGN WINAPI NtGdiCreateRectRgn(INT left, INT top, INT right, INT bottom);
HRGN WINAPI NtGdiExtCreateRegion(const XFORM* xform, DWORD count, const RGNDATA* data);
INT WINAPI NtGdiCombineRgn(HRGN dest, HRGN src1, HRGN src2, INT mode);
INT WINAPI NtGdiGetRgnBox(HRGN hrgn, RECT* rect);
BOOL WINAPI NtGdiSetRectRgn(HRGN hrgn, INT left, INT top, INT right, INT bottom);
INT WINAPI NtGdiOffsetRgn(HRGN hrgn, INT x, INT y);
BOOL WINAPI NtGdiPtInRegion(HRGN hrgn, INT x, INT y);
BOOL WINAPI NtGdiRectInRegion(HRGN hrgn, const RECT* rect);
BOOL WINAPI NtGdiEqualRgn(HRGN hrgn1, HRGN hrgn2);
DWORD WINAPI NtGdiGetRegionData(HRGN hrgn, DWORD count, RGNDATA* data);
HPALETTE WINAPI NtGdiCreatePaletteInternal(const LOGPALETTE* palette, UINT count);
LONG WINAPI NtGdiDoPalette(HGDIOBJ handle, WORD start, WORD count, void* entries, DWORD func, BOOL inbound);
UINT WINAPI NtGdiGetNearestPaletteIndex(HPALETTE hpalette, COLORREF color);
COLORREF WINAPI NtGdiGetNearestColor(HDC hdc, COLORREF color);
HPALETTE WINAPI NtUserSelectPalette(HDC hdc, HPALETTE palette, WORD force_background);
UINT WINAPI NtUserRealizePalette(HDC hdc);
UINT WINAPI NtGdiGetSystemPaletteUse(HDC hdc);

// ---- gdi_dc.cpp ----
HDC WINAPI NtGdiCreateCompatibleDC(HDC hdc);
HDC WINAPI NtGdiOpenDCW(UNICODE_STRING* device, const DEVMODEW* devmode, UNICODE_STRING* output, ULONG type,
                        BOOL is_display, HANDLE hspool, void* driver_info, void* pdev);
INT WINAPI NtGdiGetDeviceCaps(HDC hdc, INT cap);
BOOL WINAPI NtGdiGetAndSetDCDword(HDC hdc, UINT method, DWORD value, DWORD* result);
BOOL WINAPI NtGdiGetDCDword(HDC hdc, UINT method, DWORD* result);
BOOL WINAPI NtGdiGetDCPoint(HDC hdc, UINT method, POINT* result);
INT WINAPI NtGdiSaveDC(HDC hdc);
BOOL WINAPI NtGdiRestoreDC(HDC hdc, INT level);
HGDIOBJ WINAPI NtGdiSelectBrush(HDC hdc, HGDIOBJ handle);
HGDIOBJ WINAPI NtGdiSelectPen(HDC hdc, HGDIOBJ handle);
HGDIOBJ WINAPI NtGdiSelectFont(HDC hdc, HGDIOBJ handle);
HGDIOBJ WINAPI NtGdiSelectBitmap(HDC hdc, HGDIOBJ handle);
HANDLE WINAPI NtGdiGetDCObject(HDC hdc, UINT type);
INT WINAPI NtGdiIntersectClipRect(HDC hdc, INT left, INT top, INT right, INT bottom);
INT WINAPI NtGdiExcludeClipRect(HDC hdc, INT left, INT top, INT right, INT bottom);
INT WINAPI NtGdiOffsetClipRgn(HDC hdc, INT x, INT y);
INT WINAPI NtGdiExtSelectClipRgn(HDC hdc, HRGN region, INT mode);
INT WINAPI NtGdiGetAppClipBox(HDC hdc, RECT* rect);
INT WINAPI NtGdiGetRandomRgn(HDC hdc, HRGN region, INT code);
BOOL WINAPI NtGdiPtVisible(HDC hdc, INT x, INT y);
BOOL WINAPI NtGdiRectVisible(HDC hdc, const RECT* rect);
BOOL WINAPI NtGdiSetBrushOrg(HDC hdc, INT x, INT y, POINT* prev_org);
BOOL WINAPI NtGdiComputeXformCoefficients(HDC hdc);
BOOL WINAPI NtGdiTransformPoints(HDC hdc, const POINT* points_in, POINT* points_out, INT count, UINT mode);
BOOL WINAPI NtGdiGetTransform(HDC hdc, DWORD which, XFORM* xform);
BOOL WINAPI NtGdiModifyWorldTransform(HDC hdc, const XFORM* xform, DWORD mode);
DWORD WINAPI NtGdiSetLayout(HDC hdc, LONG wox, DWORD layout);
BOOL WINAPI NtGdiSetVirtualResolution(HDC hdc, DWORD horz_res, DWORD vert_res, DWORD horz_size, DWORD vert_size);
BOOL WINAPI NtGdiScaleViewportExtEx(HDC hdc, INT x_num, INT x_denom, INT y_num, INT y_denom, SIZE* size);
BOOL WINAPI NtGdiScaleWindowExtEx(HDC hdc, INT x_num, INT x_denom, INT y_num, INT y_denom, SIZE* size);
BOOL WINAPI NtGdiFlush();
UINT WINAPI NtGdiSetBoundsRect(HDC hdc, const RECT* rect, UINT flags);
UINT WINAPI NtGdiGetBoundsRect(HDC hdc, RECT* rect, UINT flags);
BOOL WINAPI NtGdiGetTextMetricsW(HDC hdc, TEXTMETRICW* metrics, ULONG flags);
BOOL WINAPI NtGdiGetTextExtentExW(HDC hdc, const WCHAR* str, INT count, INT max_ext, INT* nfit, INT* dxs, SIZE* size,
                                  UINT flags);
INT WINAPI NtGdiGetTextFaceW(HDC hdc, INT count, WCHAR* name, BOOL alias_name);
BOOL WINAPI NtGdiGetCharWidthW(HDC hdc, UINT first_char, UINT last_char, WCHAR* chars, ULONG flags, void* buffer);
BOOL WINAPI NtGdiGetCharWidthInfo(HDC hdc, abi::CharWidthInfo* info);
BOOL WINAPI NtGdiGetCharABCWidthsW(HDC hdc, UINT first, UINT last, WCHAR* chars, ULONG flags, void* buffer);
UINT WINAPI NtGdiGetTextCharsetInfo(HDC hdc, FONTSIGNATURE* fs, DWORD flags);
DWORD WINAPI NtGdiGetFontData(HDC hdc, DWORD table, DWORD offset, void* buffer, DWORD length);
UINT WINAPI NtGdiGetOutlineTextMetricsInternalW(HDC hdc, UINT cbData, OUTLINETEXTMETRICW* otm, ULONG opts);
BOOL WINAPI NtGdiGetRealizationInfo(HDC hdc, void* info);
DWORD WINAPI NtGdiGetGlyphIndicesW(HDC hdc, const WCHAR* str, INT count, WORD* indices, DWORD flags);
BOOL WINAPI NtGdiFontIsLinked(HDC hdc);
DWORD WINAPI NtGdiGetKerningPairs(HDC hdc, DWORD count, KERNINGPAIR* kern_pair);
BOOL WINAPI NtGdiSetTextJustification(HDC hdc, INT extra, INT breaks);

// ---- gdi_draw.cpp ----
BOOL WINAPI NtGdiPatBlt(HDC hdc, INT left, INT top, INT width, INT height, DWORD rop);
BOOL WINAPI NtGdiBitBlt(HDC hdc_dst, INT x_dst, INT y_dst, INT width, INT height, HDC hdc_src, INT x_src, INT y_src,
                        DWORD rop, DWORD bk_color, FLONG fl);
BOOL WINAPI NtGdiStretchBlt(HDC hdc, INT x_dst, INT y_dst, INT width_dst, INT height_dst, HDC hdc_src, INT x_src,
                            INT y_src, INT width_src, INT height_src, DWORD rop, COLORREF bk_color);
COLORREF WINAPI NtGdiGetPixel(HDC hdc, INT x, INT y);
COLORREF WINAPI NtGdiSetPixel(HDC hdc, INT x, INT y, COLORREF color);
BOOL WINAPI NtGdiRectangle(HDC hdc, INT left, INT top, INT right, INT bottom);
BOOL WINAPI NtGdiEllipse(HDC hdc, INT left, INT top, INT right, INT bottom);
BOOL WINAPI NtGdiMoveTo(HDC hdc, INT x, INT y, POINT* pt);
BOOL WINAPI NtGdiLineTo(HDC hdc, INT x, INT y);
ULONG WINAPI NtGdiPolyPolyDraw(HDC hdc, const POINT* points, const ULONG* counts, DWORD count, UINT function);
BOOL WINAPI NtGdiFillRgn(HDC hdc, HRGN hrgn, HBRUSH hbrush);
BOOL WINAPI NtGdiFrameRgn(HDC hdc, HRGN hrgn, HBRUSH brush, INT width, INT height);
BOOL WINAPI NtGdiInvertRgn(HDC hdc, HRGN hrgn);
BOOL WINAPI NtGdiExtTextOutW(HDC hdc, INT x, INT y, UINT flags, const RECT* rect, const WCHAR* str, UINT count,
                             const INT* dx, DWORD cp);
INT WINAPI NtGdiSetDIBitsToDeviceInternal(HDC hdc, INT x_dst, INT y_dst, DWORD cx, DWORD cy, INT x_src, INT y_src,
                                          UINT startscan, UINT lines, const void* bits, const BITMAPINFO* bmi,
                                          UINT coloruse, UINT max_bits, UINT max_info, BOOL xform_coords, HANDLE xform);
INT WINAPI NtGdiStretchDIBitsInternal(HDC hdc, INT x_dst, INT y_dst, INT width_dst, INT height_dst, INT x_src,
                                      INT y_src, INT width_src, INT height_src, const void* bits, const BITMAPINFO* bmi,
                                      UINT coloruse, DWORD rop, UINT max_info, UINT max_bits, HANDLE xform);

}  // namespace win10xbox::user::sys

// The registration list: every name above.
#define W10X_USER_SYSCALLS(X) \
    X(NtUserInitializeClientPfnArrays) X(NtUserRegisterClassExWOW) X(NtUserUnregisterClass) X(NtUserGetClassInfoEx) \
    X(NtUserGetClassName) X(NtUserGetAtomName) X(NtUserRegisterWindowMessage) X(NtUserSetClassLong) \
    X(NtUserSetClassLongPtr) X(NtUserSetClassWord) \
    X(NtUserCreateWindowEx) X(NtUserDestroyWindow) X(NtUserShowWindow) X(NtUserShowWindowAsync) \
    X(NtUserSetWindowPos) X(NtUserMoveWindow) X(NtUserSetWindowLong) X(NtUserSetWindowLongPtr) \
    X(NtUserSetWindowWord) X(NtUserCallHwnd) X(NtUserCallHwndParam) X(NtUserCallNoParam) X(NtUserCallOneParam) \
    X(NtUserCallTwoParam) X(NtUserGetAncestor) X(NtUserSetParent) X(NtUserFindWindowEx) X(NtUserBuildHwndList) \
    X(NtUserQueryWindow) X(NtUserWindowFromPoint) X(NtUserChildWindowFromPointEx) X(NtUserRealChildWindowFromPoint) \
    X(NtUserSetFocus) X(NtUserSetActiveWindow) X(NtUserGetForegroundWindow) X(NtUserSetForegroundWindow) \
    X(NtUserSetCapture) X(NtUserReleaseCapture) X(NtUserEnableWindow) X(NtUserInternalGetWindowText) \
    X(NtUserSetProp) X(NtUserGetProp) X(NtUserRemoveProp) X(NtUserGetWindowPlacement) X(NtUserSetWindowPlacement) \
    X(NtUserSetWindowFNID) X(NtUserGetGUIThreadInfo) X(NtUserFlashWindowEx) X(NtUserIsChildWindowDpiMessageEnabled) \
    X(NtUserGetTitleBarInfo) X(NtUserSetMenu) X(NtUserGetSystemMenu) X(NtUserDrawMenuBar) \
    X(NtUserCreateMenu) X(NtUserCreatePopupMenu) X(NtUserDestroyMenu) X(NtUserRemoveMenu) X(NtUserDeleteMenu) \
    X(NtUserCheckMenuItem) X(NtUserEnableMenuItem) X(NtUserSetMenuDefaultItem) X(NtUserHiliteMenuItem) \
    X(NtUserGetMenuItemRect) X(NtUserMenuItemFromPoint) X(NtUserGetMenuBarInfo) X(NtUserThunkedMenuItemInfo) \
    X(NtUserThunkedMenuInfo) X(NtUserTrackPopupMenuEx) X(NtUserEndMenu) \
    \
    X(NtUserGetWindowContextHelpId) X(NtUserSetWindowContextHelpId) X(NtUserNotifyWinEvent) \
    X(NtUserShowOwnedPopups) X(NtUserInternalGetWindowIcon) \
    X(NtUserGetMessage) X(NtUserPeekMessage) X(NtUserPostMessage) X(NtUserPostThreadMessage) \
    X(NtUserPostQuitMessage) X(NtUserMessageCall) X(NtUserDispatchMessage) X(NtUserTranslateMessage) \
    X(NtUserSetTimer) X(NtUserKillTimer) X(NtUserSetSystemTimer) X(NtUserKillSystemTimer) X(NtUserGetThreadState) \
    X(NtUserGetQueueStatus) X(NtUserWaitMessage) X(NtUserMsgWaitForMultipleObjectsEx) X(NtUserWaitForInputIdle) \
    X(NtUserGetKeyState) X(NtUserGetAsyncKeyState) X(NtUserGetKeyboardState) X(NtUserSetKeyboardState) \
    X(NtUserGetCursorPos) X(NtUserSetCursorPos) X(NtUserSetCursor) X(NtUserGetCursor) X(NtUserShowCursor) \
    X(NtUserClipCursor) X(NtUserGetClipCursor) X(NtUserGetKeyboardLayout) X(NtUserGetKeyboardLayoutList) \
    X(NtUserGetKeyboardLayoutName) X(NtUserActivateKeyboardLayout) X(NtUserMapVirtualKeyEx) X(NtUserToUnicodeEx) \
    X(NtUserVkKeyScanEx) X(NtUserGetKeyNameText) X(NtUserSendInput) X(NtUserTrackMouseEvent) \
    X(NtUserGetDoubleClickTime) X(NtUserMessageBeep) X(NtUserCreateCaret) X(NtUserDestroyCaret) X(NtUserShowCaret) \
    X(NtUserHideCaret) X(NtUserSetCaretPos) X(NtUserGetCaretPos) X(NtUserGetCaretBlinkTime) \
    X(NtUserGetOpenClipboardWindow) \
    X(NtUserBeginPaint) X(NtUserEndPaint) X(NtUserGetDC) X(NtUserGetDCEx) X(NtUserGetWindowDC) X(NtUserReleaseDC) \
    X(NtUserInvalidateRect) X(NtUserInvalidateRgn) X(NtUserValidateRect) X(NtUserValidateRgn) X(NtUserRedrawWindow) \
    X(NtUserGetUpdateRect) X(NtUserGetUpdateRgn) X(NtUserExcludeUpdateRgn) X(NtUserWindowFromDC) \
    X(NtUserLockWindowUpdate) X(NtUserScrollWindowEx) \
    X(NtUserGetSystemDpiForProcess) X(NtUserGetProcessDpiAwarenessContext) X(NtUserSetProcessDpiAwarenessContext) \
    X(NtUserSystemParametersInfo) X(NtUserSystemParametersInfoForDpi) X(NtUserEnumDisplayMonitors) \
    X(NtUserGetDpiForMonitor) X(NtUserEnumDisplaySettings) X(NtUserEnumDisplayDevices) \
    X(NtUserGetDisplayConfigBufferSizes) \
    X(NtUserGetProcessDefaultLayout) \
    X(NtUserSetProcessDefaultLayout) X(NtUserSetSysColors) X(NtUserIsMouseInPointerEnabled) \
    \
    X(NtGdiCreateSolidBrush) X(NtGdiCreateHatchBrushInternal) X(NtGdiCreatePatternBrushInternal) X(NtGdiCreatePen) \
    X(NtGdiExtCreatePen) X(NtGdiHfontCreate) X(NtGdiCreateBitmap) X(NtGdiCreateCompatibleBitmap) \
    X(NtGdiCreateDIBSection) X(NtGdiCreateDIBitmapInternal) X(NtGdiDeleteObjectApp) X(NtGdiExtGetObjectW) \
    X(NtGdiUnrealizeObject) X(NtGdiGetBitmapBits) X(NtGdiSetBitmapBits) X(NtGdiGetBitmapDimension) \
    X(NtGdiSetBitmapDimension) X(NtGdiGetDIBitsInternal) X(NtGdiCreateRectRgn) \
    X(NtGdiExtCreateRegion) X(NtGdiCombineRgn) \
    X(NtGdiGetRgnBox) X(NtGdiSetRectRgn) X(NtGdiOffsetRgn) X(NtGdiPtInRegion) X(NtGdiRectInRegion) X(NtGdiEqualRgn) \
    X(NtGdiGetRegionData) X(NtGdiCreatePaletteInternal) X(NtGdiDoPalette) X(NtGdiGetNearestPaletteIndex) \
    X(NtGdiGetNearestColor) X(NtUserSelectPalette) X(NtUserRealizePalette) X(NtGdiGetSystemPaletteUse) \
    X(NtGdiCreateCompatibleDC) X(NtGdiOpenDCW) X(NtGdiGetDeviceCaps) X(NtGdiGetAndSetDCDword) X(NtGdiGetDCDword) \
    X(NtGdiGetDCPoint) X(NtGdiSaveDC) X(NtGdiRestoreDC) X(NtGdiSelectBrush) X(NtGdiSelectPen) X(NtGdiSelectFont) \
    X(NtGdiSelectBitmap) X(NtGdiGetDCObject) X(NtGdiIntersectClipRect) X(NtGdiExcludeClipRect) \
    X(NtGdiOffsetClipRgn) X(NtGdiExtSelectClipRgn) X(NtGdiGetAppClipBox) X(NtGdiGetRandomRgn) X(NtGdiPtVisible) \
    X(NtGdiRectVisible) X(NtGdiSetBrushOrg) X(NtGdiComputeXformCoefficients) X(NtGdiTransformPoints) \
    X(NtGdiGetTransform) X(NtGdiModifyWorldTransform) X(NtGdiSetLayout) X(NtGdiSetVirtualResolution) \
    X(NtGdiScaleViewportExtEx) X(NtGdiScaleWindowExtEx) X(NtGdiFlush) X(NtGdiSetBoundsRect) X(NtGdiGetBoundsRect) \
    X(NtGdiGetTextMetricsW) X(NtGdiGetTextExtentExW) X(NtGdiGetTextFaceW) X(NtGdiGetCharWidthW) \
    X(NtGdiGetCharWidthInfo) X(NtGdiGetCharABCWidthsW) X(NtGdiGetTextCharsetInfo) X(NtGdiGetFontData) X(NtGdiGetOutlineTextMetricsInternalW) \
    X(NtGdiGetRealizationInfo) X(NtGdiGetGlyphIndicesW) X(NtGdiFontIsLinked) X(NtGdiGetKerningPairs) \
    X(NtGdiSetTextJustification) \
    X(NtGdiPatBlt) X(NtGdiBitBlt) X(NtGdiStretchBlt) \
    X(NtGdiGetPixel) X(NtGdiSetPixel) X(NtGdiRectangle) X(NtGdiEllipse) X(NtGdiMoveTo) \
    X(NtGdiLineTo) X(NtGdiPolyPolyDraw) X(NtGdiFillRgn) X(NtGdiFrameRgn) X(NtGdiInvertRgn) X(NtGdiExtTextOutW) \
    X(NtGdiSetDIBitsToDeviceInternal) X(NtGdiStretchDIBitsInternal)
