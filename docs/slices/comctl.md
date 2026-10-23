status: wip

# Slice: comctl32 (Wine builtin) controls through the layer

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
