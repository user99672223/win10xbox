status: ready

Ready at 015b717 (code head; this commit is doc-only): ci https://github.com/user99672223/win10xbox/actions/runs/39950439416 (linux-cross 87 of 87, windows Debug and RelWithDebInfo green, gate: nothing skipped), wine-pe https://github.com/user99672223/win10xbox/actions/runs/39950439433 green.

# Slice: wt-misc (small Wine conformance suites)

Eight suites run through one script, `wine-pe/tests/misc.sh --dll <dll>` (modelled on advapi32.sh: exe
`<dll>_test.exe`, units from the suite's Makefile.in SOURCES, link libraries from its IMPORTS plus the standard set,
baseline `wine-pe/tests/<dll>.baseline`), registered as `integration.winetest_<dll>` (label wine_conformance,
linux-cross only, MSVC registers nothing) in tests/winetest/CMakeLists.txt. The per-unit `.log`/`.diag` files and the
`<dll>.tsv` rows are in the linux-cross-bin artifact (`winetest/<dll>/`). Whole added job time: about 160 s
(psapi 3 s, cabinet 3 s, xmllite 6 s, mpr 2 s, setupapi 32 s, dbghelp 34 s, wintrust 11 s, crypt32 67 s), no unit needs a
long cap (20 s per unit), none timed out.

Baseline file format and gate: see wine-pe/tests/misc.sh (`run <max>`: must complete with at most max failures;
`crash`/`timeout`: must not complete, completing prints IMPROVED; at least one assertion must execute per suite).
Numbers below are measured on the linux-cross job of ci run 39930191602 (head 4507c6b), the `WINETEST <dll>:<unit>`
lines of its log. "Test failed: C" style truncations are Wine's own: the test prints a wide string with `%s`.

## What changed outside the scripts

- core/dispatch/unix_calls.cpp: a `crypt32` unix library table (Wine's build without a security library: process
  attach/detach succeed, the PFX import/export and the host root-certificate enumeration answer STATUS_NOT_IMPLEMENTED).
  Without it crypt32's DllMain fails (it needs `__wine_init_unix_call`, `NtQueryVirtualMemory(MemoryWineLoadUnixLib)`
  answered STATUS_DLL_NOT_FOUND), so crypt32, wintrust and setupapi (its tests import both) died with 0xC0000142 at
  load; ci run 39893860706 measured that. The gated test for it is `integration.winetest_crypt32` (and setupapi, wintrust).
- wine-pe/dlls.txt: cabinet psapi xmllite propsys dbghelp msxml3 setupapi urlmon wininet iertutil crypt32 ncrypt
  wintrust, plus cryptnet, cryptui, imagehlp (delay imports of crypt32 and wintrust). bcrypt, mpr and cfgmgr32 were
  already listed by MAIN. propsys belongs to -wt-media and is only listed. wine-pe run 39886857255 built them all (userenv included; see below why it is no longer listed).
- tests/winetest/CMakeLists.txt on MAIN had the kernel32 and shell32 blocks interleaved (an unresolved merge: one
  `add_test(... kernel32.sh all` followed by the shell32 block, two `set_tests_properties` heads, two TIMEOUT lines);
  this branch repairs it (each block whole again).

## Registered suites (baseline = measured)

| suite | unit | status | executed | failures | first failing assertion(s) |
|---|---|---|---|---|---|
| psapi | psapi_main | run | 3161 | 2125-2129 | psapi_main.c:258 GetModuleInformation failed: 6; :260 pcs-6464: image size was 0 (x128 per module/process) |
| cabinet | extract, fdi | run | 273, 158 | 0, 0 | none |
| xmllite | reader, writer | run | 2145, 2746 | 0, 0 | none |
| mpr | mpr | run | 335 | 0 | none |
| dbghelp | dbghelp | run | 869 | 16 | dbghelp.c:135 StackWalk64() failed: 126; :142 got bad return address 0; :170 didn't find stack_walk_thread frame |
| dbghelp | path | run | 5383 | 164 | path.c:1192 Mismatch in cb count, got 0 (expected 1) |
| dbghelp | minidump | crash | 0 | - | no output; access violation reading 0xf0 inside ntdll right after start |
| setupapi | devinst, dialog, diskspace, parser, setupcab, stringtable | run | 32, 20, 228, 1046, 134, 72 | 0 each | none |
| setupapi | install | run | 1078 | 6 | install.c:713 Sta.., :717 got e.., :2714 Failed to set hook |
| setupapi | misc | run | 114 | 4 | misc.c:421 Expected ERROR_FILE_NOT_FOUND, got 3; :571 Expected at least one GPU; :593 Expected at least one monitor |
| setupapi | query | run | 95 | 1 | query.c:304 |
| wintrust | register | run | 40 | 0 | none |
| wintrust | asn | run | 65 | 65 | asn.c:49, :59, :82 (every assertion: CryptEncodeObjectEx/CryptDecodeObjectEx results) |
| wintrust | crypt | run | 322 | 22 | crypt.c:406/:428 Expected success, got FALSE with -2146893799 (0x80090029) |
| wintrust | softpub | crash | 0 | - | softpub.c:313 Tests skipped: missing pfnObjectTrust, then a call through a NULL pointer (ip 0) |
| crypt32 | base64, oid, str | run | 3723, 175, 1598 | 0 each | none |
| crypt32 | main | run | 104 | 4 | main.c:340, :353 I_Crypt... |
| crypt32 | crl | run | 140 | 7 | crl.c:741 expected 1 matching CRLs, got 0; :742; :823 |
| crypt32 | message | crash (non-deterministic) | 134 or 0 | 49 or - | message.c:84 Expected 1, got -1; :148; :156 expected CRYPT_E_HASH_VALUE, got 0x57. Completes with 49 failures in two runs (39930191602, 39939144450), crashed after message.c:1257 CryptEncryptMessage failed 00000057 in one (39946638580); the crash entry records the worse outcome and prints IMPROVED when it completes |
| crypt32 | object | run | 16 | 6 | object.c:282, :289, :299 |
| crypt32 | sip | run | 48 | 11 | sip.c:331, :337 Expected a function pointer, :346 |
| crypt32 | cert | crash | 0 | - | cert.c:184 Expected failure; :195 Expected CRYPT_E_EXISTS, got 0x78; then a NULL dereference in crypt32 |
| crypt32 | chain | crash | 0 | - | chain.c:95 CertCreateCertificateChainEngine failed: 1 |
| crypt32 | ctl | crash | 0 | - | ctl.c:154 CertCreateCTLContext failed: 0xd |
| crypt32 | encode | crash | 0 | - | encode.c:8400 CryptAcquireContextA failed; :8247 Expected NTE_NO_KEY, got 0x57 |
| crypt32 | msg | crash | 0 | - | no output; access violation in crypt32 |
| crypt32 | protectdata | crash | 0 | - | protectdata.c:68 Encrypting without entropy; then a call through a NULL pointer |
| crypt32 | store | crash | 0 | - | store.c:312 CertGetCertificateContextProperty failed: 0x57; :313 wrong cert |

The crypt32 crashes follow failed setup calls (CryptAcquireContextA, CertCreateCertificateChainEngine,
CertCreateCTLContext): the tests dereference the NULL result. Lead for whoever continues: `rsaenh` (Wine's RSA/AES CSP,
which CryptAcquireContext and crypt32 use) is not in wine-pe/dlls.txt and not built.

## Not registered

- userenv, bcrypt, ncrypt: every unit dies, so no assertion is counted (the gate needs at least one) and a `crash`
  baseline can only ever print IMPROVED. Measured on ci run 39893860706 (their baselines are still in wine-pe/tests,
  `misc.sh --dll <dll>` runs them by hand):
  - userenv: userenv.c:147/150/153/156 CreateEnvironmentBlock returns FALSE (the registry Environment keys do not exist,
    NtOpenKeyEx -> 0xC0000034), then `get_env` dereferences the garbage block: access violation, exit 0xC0000005.
  - bcrypt: 21353 failures before the crash, the first bcrypt.c:769/:777/:782 "got 0xc0000002" (STATUS_NOT_IMPLEMENTED:
    the asymmetric and cipher operations need bcrypt's unix library, which wants gnutls), crash after bcrypt.c:1012.
  - ncrypt: ncrypt.c:311/:317 got 0x8009002d (NTE_FAIL), null handle; crash after ncrypt.c:644 (a call through NULL).
- msxml3: needs widl for xmlparser.idl -> xmlparser.h, which linux-cross does not have (the script skips with 77).
- urlmon, wininet: network. iertutil: no tests.

## userenv left out of wine-pe/dlls.txt

With userenv.dll in the machine, kernel32's environ unit (wine_conformance.kernel32.environ and
integration.winetest_kernel32, baseline `environ run 10`) runs `GetUserProfileDirectory` and fails one assertion more
(11 failures: environ.c:74 "Failed to get user profile dir, error 1332", ERROR_NONE_MAPPED), on both of this branch's
first two linux-cross runs (39893860706, 39930191602), while MAIN (run 39931018449) passes with 10. userenv is not
registered anyway (see above), so it is not built until the layer answers GetUserProfileDirectory (token user SID ->
registry ProfileList). Its measurement above was taken with the DLL built.

## Last run before the final push (ci 39939144450, head 4712b7b)

All my suites passed except dbghelp, whose `dbghelp` unit gave 17 failures (16 on runs 39893860706 and 39930191602, the
same first failing assertions dbghelp.c:135/:142/:170): the count is not deterministic, so the baseline is now
`dbghelp run 17` (the larger observed value is the budget, as for psapi). Not from this slice:
- `kernel32:atom status=crashed exit=1 time=0s` (REGRESSION: baseline is 'completes'). Its whole log is one line,
  `wine client error:0: recvmsg: Connection reset by peer`: the runner's own Wine (the emulator that runs win10xbox.exe on
  linux-cross) lost its wineserver connection before the guest started; the unit completed with 1049715 assertions on the
  two earlier runs. A runner flake, not a layer result. MAIN's own ci runs 39943789316, 39944578261 and 39945314963 were
  cancelled by newer merges before linux-cross finished, so main could not be compared.
- host-uwp (msix) failed with "guest(s) missing from the .msix: guest_msctf_stub.exe", a guest MAIN added.
The Windows jobs of ci and wine-pe were green.

Result of that final push (ci 39946638580, head 9f16b24): linux-cross red with exactly one failing test,
`integration.winetest_crypt32`, because `crypt32:message` crashed (exit 5, after message.c:1257 CryptEncryptMessage failed
00000057, then a call through a bad pointer) where it completed with 49 failures on run 39930191602 and 39939144450. The
outcome flips between runs, so the unit's honest baseline is `message crash 0` (a `crash` entry only prints IMPROVED when
the unit completes; `run 49` fails on every crash). Everything else in linux-cross passed (psapi, cabinet, xmllite, mpr,
setupapi, dbghelp with 17, wintrust, kernel32 including atom and environ, shell32, msvcrt); the Windows jobs and wine-pe
are green. Applied in the final push: `message crash 0`.

The final push merges main (edfdcbd + later) and sets `dbghelp run 17`; its run is reported to the coordinator.
