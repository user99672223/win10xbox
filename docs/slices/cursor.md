status: wip

# Slice: cursor

The mouse cursor is drawn. `core/user/win_cursor.cpp` keeps the cursor and icon objects that Wine's PE user32 hands over
through `NtUserSetCursorIconData`, tracks the cursor shape per thread and the show count, and `ComposeFrame` /
`ComposeLastPaintedFrame` (so host-win32's screenshots and host-uwp's presenter) draw the pointer after the windows.
`NtUserDrawIconEx` draws the same objects into a DC. Design: docs/GUI.md rule 9b.

(Filled in when ci.yml is green on all three jobs.)
