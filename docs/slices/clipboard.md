status: wip

# Slice: clipboard

Text clipboard round trip in-process through Wine's PE user32 clipboard code: `core/user/win_clipboard.cpp`
implements the NtUser clipboard syscalls (server-side format list, owner/viewer/listeners, sequence number,
text synthesis, plus win32u's per-process handle cache). Gate: `integration.guest_clipboard`
(`tests/guest/clipboard.c`) on the builtin-only machine folder.
