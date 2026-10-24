status: wip
owner: unassigned

# -crypto-rsaaes: CryptAcquireContext(PROV_RSA_AES) returns FALSE (error 0xcb) after the provider resolves

Found by `-d3dsample` round 3 (docs/slices/d3dsample.md). Not part of that slice.

## First error

`CryptAcquireContextW(&h, NULL, MS_ENH_RSA_AES_PROV, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)` returns FALSE, `GetLastError() == 0xcb`
(ERROR_ENVVAR_NOT_FOUND; probably a stale value, not the real failure), on the layer, on a machine folder whose registry now has the
provider keys. Log line from the round-3 probe (3046eba): `crypto: CryptAcquireContext(PROV_RSA_AES) failed, error 0x000000cb`.

Steps that DO work, in order (all from the +loaddll / registry trace of ci run 40133570491, Debug job):

1. `Software\Microsoft\Cryptography\Defaults\Provider Types\Type 024` resolves (added by `ApplyDefaultSoftwareKeys`,
   core/registry/registry.cpp, round 3; before it the open failed with c0000034).
2. `Defaults\Provider\Microsoft Enhanced RSA and AES Cryptographic Provider` resolves (Image Path `rsaenh.dll`, SigInFile 0, Type 24).
3. Wine's advapi32 (`dlls/advapi32/crypt.c`, `CRYPT_LoadProvider`) loads the DLL named by Image Path: `rsaenh.dll` is mapped from
   `Windows\builtin` (listed in `wine-pe/dlls.txt` since round 3), together with crypt32, ncrypt and bcrypt.
4. Then the call returns FALSE and all four DLLs are unmapped (see docs/slices/unload-slist.md for what that unmap leaves behind).

What is not known: which step inside `CRYPT_LoadProvider` fails (`GetProcAddress` of the provider's `CPAcquireContext` table, the
provider's DllMain, the `SigInFile`/signature handling, or the registry value type of one of the values we wrote, e.g. REG_DWORD
vs REG_BINARY for `SigInFile`/`Type`). Wine's `CRYPT_LoadProvider` sets no error of its own on every path, which is why 0xcb is stale.

## Reproducer

The Debug build of the d3dsample fixture compiles its shader with `D3DCOMPILE_DEBUG`, whose d3dcompiler_47 path needs
CryptAcquireContext + an MD5 hash. After the d3dsample round 4 the fixture is always built Release, so that path is no longer
exercised by CI. A fresh agent should build a small guest instead, `tests/guest/crypto_rsaaes.c`:
`CryptAcquireContextW(PROV_RSA_AES, CRYPT_VERIFYCONTEXT)`, then `CryptCreateHash(CALG_MD5)`, `CryptHashData`, `CryptGetHashParam`,
print the result and `GetLastError()`; register it as an integration test on the Windows jobs (machine-native-d3d is enough).

Native control: the same call succeeds on Windows (same binary, no layer): the provider is genuine Windows rsaenh there.

## Next steps

* `WIN10XBOX_WINEDEBUG=+loaddll,+reg,+crypt,+module` on the reproducer: find the first failing call after rsaenh is mapped.
* Compare the registry values with a real Windows hive (`reg query HKLM\SOFTWARE\Microsoft\Cryptography\Defaults\Provider /s`):
  value names, types (REG_DWORD for `Type` and `SigInFile`) and the `Image Path` form.
* If the provider DLL's DllMain or `CPAcquireContext` fails because of an unresolved import or a missing builtin, add it
  (`wine-pe/dlls.txt`) rather than weakening the sample.

Related, found on the same path: the MessageBox route of the sample (user32/gdi32full text path) calls
`TextShaping.dll!ShapingGetBreakingProperties`; the machine has no TextShaping builtin, so it is an unresolved import there.
Add a builtin (or a stub export) when a program reaches a MessageBox; not needed for the passing sample.
