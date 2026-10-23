status: ready

# Slice: clipboard

Text clipboard round trip in-process through Wine's PE user32 clipboard code. `core/user/win_clipboard.cpp`
implements the NtUser clipboard syscalls: wineserver's clipboard (format list, owner, viewer, listeners, sequence
number, text synthesis) fused with win32u's per-process cache of the handles the application passed to
SetClipboardData. The user32 <-> win32u protocol is Wine's (`struct set_clipboard_params` /
`struct get_clipboard_params` of `include/ntuser.h`).

CI run (ci.yml, all three jobs green: windows Debug, windows RelWithDebInfo, linux-cross):
https://github.com/user99672223/win10xbox/actions/runs/39886738188
host-uwp.yml (guest_clipboard bundled in the package): https://github.com/user99672223/win10xbox/actions/runs/39886738210

## What runs through the layer

`tests/guest/clipboard.c` (gate `integration.guest_clipboard`, builtin-only machine folder, exit 42, "Gate - nothing
skipped" passed). It creates two windows and runs the user32 API against the emulated clipboard. First lines, from
the job's ctest-junit.xml:

    clipboard: window ok
    clipboard: set-unicode ok
    clipboard: sequence-number ok seq 0 -> ...
    clipboard: get-unicode ok
    clipboard: synthesized-text ok CF_TEXT=Hello Xbox clipboard
    clipboard: enum-formats ok count=4
    clipboard: registered-format ok seq 6
    clipboard: delayed-render ok calls=1
    clipboard: listener-update ok count=1
    clipboard: listener-removed ok
    clipboard: done

SetClipboardData(CF_UNICODETEXT) then GetClipboardData returns the same text; CF_TEXT, CF_OEMTEXT and CF_LOCALE are
synthesised when the clipboard is closed (EnumClipboardFormats lists four formats); the sequence number increases;
a registered format (RegisterClipboardFormatW) round-trips binary data; delayed rendering answers WM_RENDERFORMAT;
a second window registered with AddClipboardFormatListener receives WM_CLIPBOARDUPDATE exactly once per change and
none after RemoveClipboardFormatListener.

## Syscalls added (core/user/user_syscalls.h W10X_USER_SYSCALLS, docs/SYSCALLS.md regenerated)

NtUserOpenClipboard, NtUserCloseClipboard, NtUserEmptyClipboard, NtUserSetClipboardData, NtUserGetClipboardData,
NtUserCountClipboardFormats, NtUserIsClipboardFormatAvailable, NtUserEnumClipboardFormats,
NtUserGetUpdatedClipboardFormats, NtUserGetPriorityClipboardFormat, NtUserGetClipboardFormatName,
NtUserGetClipboardOwner, NtUserGetClipboardViewer, NtUserGetOpenClipboardWindow, NtUserGetClipboardSequenceNumber,
NtUserSetClipboardViewer, NtUserChangeClipboardChain, NtUserAddClipboardFormatListener,
NtUserRemoveClipboardFormatListener. NtUserSetClipboardData is listed as NTSTATUS-returning in dispatcher.cpp.

DLLs added to wine-pe/dlls.txt: none.

## What still fails / not done

- Bitmap, DIB and metafile format synthesis (CF_BITMAP <-> CF_DIB ...) is not implemented (logged once when it
  would apply). Text synthesis is done at GetClipboardData time here instead of through
  NtUserRenderSynthesizedFormat.
- The window-destroy / thread-exit hooks of the server are a sweep at the start of each clipboard syscall.
- No cross-process clipboard (there is one process).
- The first run of this slice (39847401022) failed only because the linux-cross job timed out installing Wine
  ("Install Wine + ninja" 12 minute action timeout); the second push needed host-uwp.yml to carry the new guest
  (host-uwp/CMakeLists.txt).
