// Scroll bars (win_scroll.cpp): the NtUser* scroll bar syscalls and the hooks
// the default window procedure / the ScrollBar class procedure call.
#pragma once
#include "user_internal.h"

namespace win10xbox::user {
namespace sys {
INT WINAPI NtUserSetScrollInfo(HWND hwnd, INT bar, const SCROLLINFO* info, BOOL redraw);
BOOL WINAPI NtUserShowScrollBar(HWND hwnd, INT bar, BOOL show);
BOOL WINAPI NtUserEnableScrollBar(HWND hwnd, UINT bar, UINT flags);
BOOL WINAPI NtUserGetScrollBarInfo(HWND hwnd, LONG id, SCROLLBARINFO* info);
BOOL WINAPI NtUserScrollDC(HDC hdc, INT dx, INT dy, const RECT* scroll, const RECT* clip, HRGN update_rgn,
                           RECT* update_rect);
}  // namespace sys
namespace detail {
struct GetScrollInfoParams { int bar; SCROLLINFO* info; };   // include/ntuser.h get_scroll_info_params
// lock held for all of these
ULONG_PTR ScrollGetInfoCall(HWND hwnd, ULONG_PTR param);      // NtUserCallHwndParam(GetScrollInfo)
LRESULT ScrollBarWndProcInternal(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool ansi);
LRESULT ScrollNcHitTest(WindowObj* w, POINT screen_pt);        // HTVSCROLL / HTHSCROLL / 0
void ScrollDrawNc(HWND hwnd);                                  // both bars + the size corner (WM_NCPAINT)
bool ScrollNcLButtonDown(HWND hwnd, LPARAM screen_pt);         // true when a bar consumed the click
}  // namespace detail
}  // namespace win10xbox::user
