status: wip

# Slice: -realgit (Git for Windows through the layer)

Goal: MinGit 2.47.1 x64 (`mingw64/bin/git.exe`, pinned by SHA-256, `tools/ci/fetch_mingit.ps1`) runs on the Wine PE
builtins over the NT syscall dispatcher: `git --version`, then `init`, `add`, `status`, `commit`, `log --oneline` in a
temporary repository (`integration.real_git*`, registered with the Lua tests under `if(MSVC OR WIN10XBOX_REAL_PROGRAMS)`).

First attempt (run 39848918482): both tests died with `EXCEPTION_WINE_STUB` (`api-ms-win-core-crt-l2-1-0!_initterm_e`
called from sechost's DllMain) because the tests ran on the shared machine folder, where the runner's native
advapi32/sechost are mapped. The git tests now run on the builtin-only folder, which `prepare_machine.cmake` fills with
the `guest/mingit` tree.
