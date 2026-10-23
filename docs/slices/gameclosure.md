status: wip

# -gameclosure: DLL/import closure of a game through the layer

Handoff (implement agent, one push):
1. tools/game/dump_imports.py (python3, stdlib): import + delay-load tables of every x64 PE in a directory -> `<file>_imports.txt`
   (format in the file's docstring; sizes only, no hashes). docs/game/README.md: what the owner runs, commits, and keeps.
2. tests/guest/closure.c (guest_closure): for each list, LoadLibrary the module when it is a DLL shipped beside the lists,
   and LoadLibrary + GetProcAddress every `<dll> <symbol|#ordinal>`; prints `closure: MISSING DLL ...`, `closure: MISSING dll!sym (module)`
   and the summary; exit 42 closed, 43 otherwise (also when no list exists). Nothing skipped or faked.
3. tests/integration: target `closure_lists` writes the lists of the built guests (hello, xinput, net, wasapi, xaudio2,
   d3d11_tri) into bin/guest (prepare_machine stages `*_imports.txt`); `integration.guest_closure` on machine-native-d3d
   (native Windows builds with guest_d3d11_tri only, REQUIRE d3d11.dll) expects exit 42 and `0 missing dlls, 0 missing symbols`;
   `integration.guest_closure_builtin` is the same on the builtin folder, registered on linux-cross only (Wine). ci.yml runs
   plain ctest, so both are discovered; ci.yml is unchanged.
4. host-uwp: NOT added. guest_closure with no argument looks in C:\guest\closure, finds no list and exits 43, while host-uwp
   expects 42 from each packaged guest. (Packaging it with lists is a later step.)
Untested on Windows: integration.guest_closure (d3d machine, MSVC guests' lists), the MSVC build of closure.c.
Commands: `cmake --build build-mingw --target prepare_machine && ctest --test-dir build-mingw -R guest_closure`;
owner: `python3 tools/game/dump_imports.py "<game dir>" docs/game`.

## Measured closure (local, builtin folder, Wine 9 host; imports OF the DLL resolved through the layer)

wine-pe/out/builtin lists no d3d11, dxgi, xaudio2_9, winhttp, msvcp140 or vcruntime140 (xaudio2_9 is in dlls.txt but the
local artifact predates it; d3d11/dxgi are native in route B; winhttp, msvcp140, vcruntime140 are in no list: the VC
runtime is NOT staged). xinput1_3/1_4 are our own guest-dlls copies.

| DLL | imports checked | missing |
|---|---|---|
| ws2_32 | 77 | 0 |
| secur32 | 86 | 0 |
| crypt32 | 154 | 0 |
| bcrypt | 36 | 0 |
| dbghelp | 125 | 0 |
| ucrtbase | 160 | 0 |
| xinput1_4 (own) | 4 | 0 |
| wininet | 208 | 1: dhcpcsvc.dll (absent) `DhcpRequestParams` |
| d3d11, dxgi, xaudio2_9, winhttp, vcruntime140, msvcp140 | (not present) | DLL missing: a hand-written probe of one export each (`D3D11CreateDevice`, `CreateDXGIFactory1`, `XAudio2Create`, `WinHttpOpen`, `memcpy`, `?_Xbad_alloc@std@@YAXXZ`) reports all six DLLs missing |

Total of that run: 9 modules, 26 dlls, 857 symbols, 7 missing dlls, 7 missing symbols. The gate's own lists (5 mingw guests,
13 dlls, 248 symbols) resolve with 0 missing under Wine. Next: wine-pe/dlls.txt gets winhttp and dhcpcsvc (Wine-side), then
the owner's lists decide the rest (VC runtime from the redistributable, d3d11/dxgi native).

### Push 1 (ci-loop): Wine-side builtins
Added to wine-pe/dlls.txt: winhttp, jsproxy (winhttp delay import: InternetGetProxyInfo, InternetInitializeAutoProxyDll,
InternetDeInitializeAutoProxyDll), msvcp140, msvcp140_1, vcruntime140, vcruntime140_1, dhcpcsvc. All built locally with
`wine-pe/build.sh --only <dll>`. closure_lists now also dumps the imports of winhttp, wininet, msvcp140(_1), vcruntime140(_1),
dhcpcsvc from WIN10XBOX_BUILTIN_DIR (those present at configure time). Local guest_closure_builtin: 12 modules, 27 dlls,
826 symbols, 0 missing dlls, 0 missing symbols (before jsproxy: 1 missing dll, 3 missing symbols, all jsproxy).

### Push 2 (ci-loop)
Push 1 on CI: both closure tests failed with `closure: no *_imports.txt in C:\guest`: ci.yml runs `cmake --build` (ALL) then ctest, and
closure_lists was not part of ALL (only of the prepare_machine target, which the integration.prepare_machine test does not build).
closure_lists is now `ALL`.

### Status after two ci-loop pushes (blocked)
Runs for f108c41: ci https://github.com/user99672223/win10xbox/actions/runs/40023521598 (linux-cross success incl. guest_closure_builtin;
both windows jobs fail only on integration.guest_closure), wine-pe https://github.com/user99672223/win10xbox/actions/runs/40019588822 (success, abf02a2:
winhttp, jsproxy, msvcp140, msvcp140_1, vcruntime140, vcruntime140_1, dhcpcsvc all built).
Windows (Debug, machine-native-d3d) first error: `closure: MISSING DLL crypt32.dll (needed by winhttp.dll)`; summary
`closure: 13 modules, 19 dlls, 1157 symbols, 2 missing dlls, 15 missing symbols`. Missing: crypt32.dll (CertDuplicateCertificateContext,
CertFreeCertificateChain, CertFreeCertificateContext, CertGetCertificateChain, CertNameToStrW, CertVerifyCertificateChainPolicy,
CryptFindOIDInfo for winhttp; CertCreateCertificateContext, CertDuplicateCertificateChain, CertNameToStrA and the same chain calls for
wininet) and cryptui.dll (CryptUIDlgViewCertificateW, wininet). crypt32.dll is in the fetched builtin artifact (716800 bytes), so on that
folder LoadLibrary of crypt32 fails (not an absent file): unexplained, needs `WIN10XBOX_DEBUG` trace of the load on Windows. Everything
else (ws2_32, secur32, bcrypt, dbghelp, ucrtbase, d3d11, dxgi, VC runtime, dhcpcsvc, jsproxy) resolves. windows RelWithDebInfo printed no
closure lines at all (0.86 s, minidump record failures): unexplained.
Owner next: run guest_closure by hand on machine-native-d3d with a load trace for crypt32/cryptui; cryptui is a wintrust/wininet delay chain
that is not in the d3d machine's folder.

### Root-cause push (fresh agent)
crypt32 load failure: not a missing file and not an apiset. The job log's crash record for the Debug run shows two EXCEPTION_WINE_STUB
exceptions, `unresolved import: dll "kernelbase.dll" function "LoadStringBaseExW"`, raised under crypt32's DllMain
(crypt32+0x40a61, CertOIDToAlgId region) -> the genuine user32!LoadStringW (route B: native user32 in the folder) -> kernelbase!LoadStringBaseExW,
which Wine's kernelbase.spec has as `@ stub`. The stub raise is caught by Wine's __TRY in MODULE_InitDLL, DllMain fails, LoadLibrary(crypt32)
returns 0 (so winhttp and wininet report it, and cryptui.dll, which imports crypt32, fails the same way; cryptui is already in dlls.txt and in
the artifact). The builtin-only folder uses Wine's own user32, which never calls that export, hence linux-cross is green.
Fix: wine-pe/patches/0021-kernelbase-loadstringbaseexw.patch exports LoadStringBaseExW(instance, id, buffer, length, language) sharing one
loader with LoadStringW (same pattern as 0004 for CheckTokenMembershipEx and friends).
RelWithDebInfo: the same stub exception, not a crash in closure.c. Exit code -2147483392 is 0x80000100 (EXCEPTION_WINE_STUB) of the first
crypt32 load; no `closure:` line was printed because the host process ended on it. In the Debug host the same two exceptions are recovered
through the TEB-frame VEH (`2 unwinds to a frame`) and LoadLibrary returns 0; in the optimised host the recovery did not take place (why is
open: core/dispatch/teb_frames.cpp is the only code between). With the stub gone neither build raises it.
