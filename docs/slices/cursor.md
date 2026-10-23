status: ready

# Slice: cursor

The mouse cursor is drawn. `core/user/win_cursor.cpp` keeps the cursor and icon objects that Wine's PE user32 hands over
through `NtUserSetCursorIconData`, tracks the cursor shape per thread and the show count, and `ComposeFrame` /
`ComposeLastPaintedFrame` (so host-win32's screenshots and host-uwp's presenter) draw the pointer after the windows.
`NtUserDrawIconEx` draws the same objects into a DC. Design: docs/GUI.md rule 9b.

CI run (ci.yml, all three jobs green: linux-cross, windows Debug, windows RelWithDebInfo, "Gate - nothing skipped"
passed) for code commit 2376feb: https://github.com/user99672223/win10xbox/actions/runs/39948192106
(the commit after it only edits this file; its own ci.yml run is the head's).
host-uwp.yml (`workflow_dispatch` on the branch, since it runs on main only; `guest_cursor` is in HOST_UWP_GUESTS, the
GUI foreach and the package check list): https://github.com/user99672223/win10xbox/actions/runs/39951740095. The package builds
with `guest_cursor` in it (it is not among the missing guests). The job's "Verify package contents" step fails
for a reason that is on main and not in this change: it lists every `tests/guest/*.c`, and `guest_msctf_stub.exe` and
`guest_net.exe` are not built by host-uwp/CMakeLists.txt.

## What runs through the layer

`tests/guest/cursor.c` (gate `integration.guest_cursor`, builtin-only machine folder, exit 42). It loads Wine's own
`IDC_ARROW` and `IDC_IBEAM` (user32 reads the 32 bpp images from its resources and calls `NtUserSetCursorIconData`),
builds a monochrome AND/XOR cursor with `CreateCursor`, checks `GetIconInfo`, `GetCursorInfo`, `GetCursor`, `SetCursor`
and `ShowCursor`, and draws the icons into its window with `DrawIconEx` (normal size, stretched, monochrome) reading the
pixels back with `GetPixel`. The composed cursor is not painted by the guest, so the host script moves the pointer to four
places and the guest changes the cursor on each move (it recognises them by `GetCursorPos`); the script takes
a screenshot after each of the first three with the new inject command `shot FILE` and `run_guest.cmake` checks pixels of
every file (`WIN10XBOX_SCREENSHOT_2..4` / `WIN10XBOX_EXPECT_SCREENSHOT_PIXELS_2..4`; the final file is the usual
`WIN10XBOX_SCREENSHOT`, written after the guest exited). First lines, from the job's ctest-junit.xml:

    cursor: loaded arrow=0000000000010001 ibeam=0000000000010002 shared=1
    cursor: arrow icon=0 hot=0,0 mask=32x32x1 color=32x32x32
    cursor: ibeam icon=0 hot=3,9 mask=32x32x1 color=32x32x32
    cursor: mono icon=0 hot=4,4 mask=32x64x1 color=none
    cursor: DrawIconEx arrow=ok stretched=ok mono=ok
    cursor: P0 ibeam showing pos=250,250
    cursor: P1 SetCursor(arrow) returned ibeam
    cursor: P2 SetCursor(mono) returned arrow
    cursor: P3 ShowCursor(FALSE)=-1 flags=0
    cursor: done

Screenshot pixels checked (window colour ff4080c0 at (100,100)-(500,400)): I-beam at (250,250), hotspot (3,9): opaque white
at (248,242) and (250,242), fffdfdfd at (250,250), the transparent image pixel (257,250) is the window colour; arrow at
(320,250), hotspot (0,0): opaque black at (321,251), white at (322,254) and (323,255), transparent (340,255) is the window
colour, and the old I-beam spot is the window colour again; monochrome cursor at (390,250), hotspot (4,4): black at
(388,252), white at (396,252), the inverted window colour ffbf7f3f at (404,252), unchanged at (412,252); final file after
`ShowCursor(FALSE)` (pointer at (392,252)): none of the monochrome cursor's pixels, plus the DrawIconEx results in the
window. host-win32's screenshot therefore shows the arrow (a cursor image is in every frame while the show count is >= 0).

## Syscalls added (core/user/user_syscalls.h W10X_USER_SYSCALLS, docs/SYSCALLS.md regenerated)

`NtUserSetCursorIconData`, `NtUserFindExistingCursorIcon`, `NtUserDestroyCursor`, `NtUserGetIconInfo`,
`NtUserGetIconSize`, `NtUserGetCursorFrameInfo`, `NtUserGetCursorInfo`, `NtUserDrawIconEx`; `NtUserCallOneParam`
`CreateCursorIcon`. Changed: `NtUserSetCursor` / `NtUserGetCursor` / `TsCursor` are per thread (they used the driver's one
global id), `NtUserShowCursor` is a real count under the lock, `NtUserClipCursor` / `NtUserGetClipCursor` remember the
rectangle, the default `WM_SETCURSOR` sets the thread's cursor from the class cursor (non-client hits go back to the
built-in arrow), `NtGdiStretchDIBitsInternal` keeps the alpha byte of a 32 bpp source on a 32 bpp DIB section (user32's
`create_alpha_bitmap` needs it; every other destination still gets 0xff). host-win32: inject command `shot [FILE]`.
DLLs added to wine-pe/dlls.txt: none.

## What still fails or is not done

* Animated cursors (`.ani`, `NtUserSetCursorIconData` with `num_steps`) fail with `ERROR_CALL_NOT_IMPLEMENTED`; no frame
  stepping.
* Cursor clipping is only remembered and passed to the driver (which clamps the pointer); `ClipCursor` by a program is not
  tested by the gate.
* `DrawIconEx` onto a DC backed by a 1 bpp or palettised bitmap fails (FIXME logged); only a solid background brush is
  drawn; `DI_NOMIRROR` ignored.
* The builtin window classes (Button, Edit, ...) have no class cursor, and DefWindowProc picks no sizing cursors on the
  frame: a non-client hit shows the built-in arrow (12x18, drawn from the source of `win_cursor.cpp`, used too when no
  application cursor applies).
* host-uwp presents the composed frame with the pointer in it; its own system cursor over the CoreWindow is not hidden
  by this change.
* `CopyImage` of cursors, `NtUserSetIconParam` / `GetIconParam` are untested / not handled.
