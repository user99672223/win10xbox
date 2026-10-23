status: ready

# Slice: comctl32 (Wine builtin) controls through the layer

ci.yml run: https://github.com/user99672223/win10xbox/actions/runs/39882077562 (windows Debug, windows RelWithDebInfo and
linux-cross green, "Gate - nothing skipped" passed, `integration.guest_comctl` Passed in 3.39 s). It ran on commit 6110904;
the head after it only changes this document.

Gate: `integration.guest_comctl` (tests/guest/comctl.c) on the builtin-only machine folder. InitCommonControlsEx, then a
report-view ListView (2 columns, 3 items) with its header, a TreeView and a smooth progress bar (position 50 of 100) as
children of a 400x300 popup; the guest samples pixels (ListView background white, header button face, progress fill and
the part outside it) and the screenshot checks the same ones (WIN10XBOX_EXPECT_SCREENSHOT_PIXELS). Exit 42.

## What the first CI run (39848694058) showed

comctl32.dll maps and runs as a builtin (no new DLL needed); every control is created and answers its messages
(LVM_GETITEMCOUNT 3, TVM_GETCOUNT 2, PBM_GETPOS, ...). The pixel check failed: ListView, TreeView, progress bar and
status bar read back as one colour. Cause, from the relay trace: a window that does not clip its children erased and
painted over children that had already painted (the progress bar paints itself synchronously from PBM_SETPOS; the
ListView header is painted by `UpdateWindow(header)` inside the ListView's own WM_PAINT, which then calls
`ValidateRect(listview, header rect)` so that its erase leaves the header alone). core/user kept one bounding rectangle
as update region and could neither validate "everything but the header strip" nor re-paint covered children.

## core/user changes (win_paint.cpp)

* `ValidateRect`/`ValidateRgn` subtract the rectangle from the update rectangle whenever the result is still one
  rectangle (a strip along one edge), instead of only when it covers the whole update rectangle.
* A window that paints (`BeginPaint`) validates its painted area on every ancestor that does not clip children
  (win32k `validate_parents`), where representable.
* A window that does not clip children and paints or erases over its children re-invalidates (paint + erase) the
  children it covered, so they paint after it (win32k keeps them valid with a real region).

Not touched here: `NtUserSetScrollInfo`/`NtUserShowScrollBar` (slice -scroll) and `NtUserFindExistingCursorIcon` (returns
0 with ERROR_CALL_NOT_IMPLEMENTED; LoadCursor of the class cursors fails quietly).

## What runs through the layer

`C:\guest\guest_comctl.exe` (tests/guest/comctl.c, MSVC/llvm-mingw build, no manifest so the v5 classes) on the
builtin-only machine folder, against Wine 11.9's builtin `comctl32.dll`. Output from the job log:

```
comctl: init ok
comctl: created lv=1 tv=1 pb=1 hdr=1
comctl: lv count=3 cols=2 tv count=2 pb pos=50
comctl: pixels lv=00ffffff hdr=00f0f0f0 pb_in=0000a000 pb_out=00e6e6e6
comctl: label pixels drawn=154
comctl: done
```

exit 42, and the screenshot pixels (255,205)=white (ListView background), (285,114)=f0f0f0 (header, right of the two
columns), (125,245)=00a000 (progress fill), (275,245)=e6e6e6 (outside the fill) match. The progress bar uses
PBM_SETBARCOLOR/PBM_SETBKCOLOR so the check does not depend on the system colours; the header and ListView background use
the default system colours (COLOR_BTNFACE, COLOR_WINDOW).

## Syscalls added or changed

None added. `NtUserValidateRect`/`NtUserValidateRgn`/`NtUserRedrawWindow(RDW_VALIDATE)`, `NtUserBeginPaint` and the
erase path of `NtUserRedrawWindow`/`NtUserGetUpdateRect` changed behaviour (see above).

## DLLs added to wine-pe/dlls.txt

None (comctl32 was already listed; it needs no uxtheme: Wine loads it only when a manifest asks for v6).

## What still fails

* v6 controls (a manifest/activation context) and themed drawing: not tried; WC_BUTTONW from comctl32 v6 is user32's
  builtin button without one.
* `NtUserSetScrollInfo`, `NtUserShowScrollBar` (28 and 2 calls in the trace): still Todo here, return 0; the -scroll
  slice implements them. ListView/TreeView with more items than fit show no scroll bars until that lands.
* `NtUserFindExistingCursorIcon` (18 calls, from the cursors comctl32's classes load): Todo, returns 0, the class cursor
  stays NULL.
* Update regions are still bounding rectangles: a hole in the middle of a parent's region is not representable, so a
  parent covering an already painted child repaints the child afterwards (extra WM_PAINT, same final pixels).
