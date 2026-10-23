status: ready

# Slice: dialog

`DialogBoxParamW` from a DIALOG resource with Button, Static and Edit controls, through Wine's PE user32 and our
core/user. Gate: `integration.guest_dialog` (tests/guest/dialog.c + dialog.rc) on the builtin-only machine folder.

Run: https://github.com/user99672223/win10xbox/actions/runs/39883477245 (head 717e0c5; linux-cross, windows (Debug) and
windows (RelWithDebInfo) green, "Gate - nothing skipped": 49/49 on the Windows jobs, 64/64 on linux-cross).

## What runs through the layer
`C:\guest\guest_dialog.exe`, exit code 42. It creates the dialog with DialogBoxParamW from its DIALOG resource and
prints (the lines ctest matches, tests/integration/CMakeLists.txt):

    dialog: init edit=100 button=1
    dialog: text=hello dialog
    dialog: class=#32770 edit=Edit button=Button static=Static
    dialog: ok clicked
    dialog: result=7
    dialog: done

WM_INITDIALOG runs, GetDlgItem finds the controls, SetDlgItemTextW / GetDlgItemTextW round-trip, tab order works,
WM_CTLCOLORSTATIC is answered. The host injects a click (WIN10XBOX_TEST_INJECT_INPUT) on the OK button; the dialog ends
with EndDialog(7). The screenshot readback (WIN10XBOX_EXPECT_SCREENSHOT_PIXELS) checks the button frame (110,150) and
face (118,158), the static's colour (160,114) and the edit's white (240,136).

## Syscalls
Added (registered in core/user/user_syscalls.h): NtUserAlterWindowStyle (BM_SETSTYLE / focus changes),
NtUserCallMsgFilter (IsDialogMessage; no hooks exist, so it never eats a message), NtUserQueryInputContext (no IME: 0).
Changed: NtUserBuildHwndList (a window with children=FALSE lists that window and its following siblings, which
GetDlgItem / GetNextDlgTabItem rely on), NtGdiGetCharWidthW / NtGdiGetCharABCWidthsW (with the INDICES flag `last` is the
count: GetCharWidthI overran the caller's buffer). DLLs added to wine-pe/dlls.txt: none.

## Other
tests/guest/window_input2.c prints the other thread's message log before its "another thread's window" check. That
check failed twice in a row on linux-cross (Wine host) for head 0bfecc3 and passed on 717e0c5 after merging main; the
cause was not found, the log line is there to diagnose it if it comes back.

## What still fails
Nothing in this slice's gate. Dialog features beyond it (modal loop with nested dialogs, DS_* styles, combo/list box
controls, WM_CTLCOLOR for edit/button) are not exercised.
