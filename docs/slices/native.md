status: ready

# Slice: native GUI (`gui: native`, route B)

Gate: `integration.guest_window_native` (tests/guest/window.c on `machine-native-gui`, machine.json `gui: native`): the guest
loads the host's own user32/gdi32/gdi32full/win32u/msvcp_win/imm32, every NtUser*/NtGdi* syscall is forwarded by name to
the host's win32u.dll, and `user::ComposeFrame` composes the guest HWNDs with PrintWindow. Registered on Windows builds only.

Green ci.yml run on the head merged with main (b2f4356): https://github.com/user99672223/win10xbox/actions/runs/39931462411
(windows Debug, windows RelWithDebInfo, linux-cross all green; also green pre-merge on 11dec4f, run 39881185269).

Root causes found on the way to green (each was the first failure in the Windows job log):
1. The genuine user32 calls `KERNELBASE!WTSGetServiceSessionId`; the guest shares the host PEB, so the API-set map is
   Windows' own, and it routes imports to kernelbase that Wine keeps elsewhere or stubs. wine-pe patches 0003/0004 add
   WTSGetServiceSessionId, WTSIsServerContainer, CheckTokenMembershipEx, SetProcessValidCallTargets and forward the
   Global*/Local* family to kernel32.
2. `Native_CsrClientConnectToServer` dereferenced its 4th argument, which this user32 passes by value (0x248): it no
   longer touches arguments 4 and 5.
3. imm32 delay-loads msctf.dll; the genuine one does not load on Wine's builtins, so tests/guest/msctf_stub.c is a
   do-nothing msctf.dll staged into the native machine's System32 only (prepare_machine.cmake).
4. combase -> ole32 -> coml2 and advapi32 -> cryptbase were not built: wine-pe/dlls.txt.

Open: further host-win32k/Wine-builtin export gaps surface one per run (the stub exception names the DLL and function in
crash.txt); `GetPersistedRegistryLocationW` and `__uncaught_exceptions` are known unimplemented and not yet called.
