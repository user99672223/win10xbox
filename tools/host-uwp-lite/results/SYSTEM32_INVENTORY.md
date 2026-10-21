# Xbox C:\Windows\System32 "readable by name" inventory

Captured by `host-uwp-lite` v1.0.0.2 running inside its own AppContainer on
`XBOX` (OS build `10.0.26100.9445`, SystemOS Dev Mode, Game bucket). Each name
below was opened with `CreateFile2(GENERIC_READ)` from the probed list in
`host-uwp-lite/stage.cmake` (same list the probe used, extended slightly). The
app did not enumerate `System32`; names were opened one at a time.

- 29 of 47 probed DLL names are readable by name.
- 18 of 47 return `ERROR_FILE_NOT_FOUND` (error 2).
- Only `ntdll.dll` was known-readable from the earlier capability probe; this
  inventory extends that result.

## Readable (29)

Each of these is openable at `C:\Windows\System32\<name>` from an AppContainer
app on Xbox:

```
kernelbase.dll (4 108 288 B)
ucrtbase.dll   (1 351 680 B)
msvcrt.dll       (684 032 B)
```

Plus 26 further names reported as `readable by name` by `smoke.cpp`'s
inventory step — see the raw list of `harness: inventory: ... readable by name`
lines in `trace.log` for the complete set and byte counts.

## NOT readable (18, all error 2 = ERROR_FILE_NOT_FOUND)

```
kernel32.dll
user32.dll
gdi32full.dll
imm32.dll
comctl32.dll
comdlg32.dll
shell32.dll
shlwapi.dll
ole32.dll
version.dll
winmm.dll
setupapi.dll
psapi.dll
apisetschema.dll
advapi32.dll
vcruntime140.dll
vcruntime140_1.dll
msvcp140.dll
```

These are **loadable** via `LoadLibraryExW` by name — the probe's
`B/load:kernel32`/`load:user32`/`load:advapi32` and the related
`exports:*` records all show OK, meaning the resolver reaches them through a
different path (likely SxS / activation-manifest / API set redirection, backed
by `C:\Program Files\WindowsApps` or an inbox store that is not
`C:\Windows\System32`). They are just not present as plain files under
`\Windows\System32` on the Xbox SystemOS Dev Mode volume.

## NLS files (System32)

- `l_intl.nls` and `c_10000.nls` were absent from `C:\Windows\System32`.
  Everything else in the staged NLS list (`c_1252.nls`, `locale.nls`,
  `norm*.nls`, `c_437..c_1258.nls`, `c_20127.nls`, `c_28591.nls`) opened
  cleanly, 25 of 27.
- Both were patched into `LocalState\machine\Windows\System32\` by hand before
  the second run (copies from the laptop's `C:\Windows\System32`); after the
  patch `nls.required_missing=l_intl.nls` still appears in `result.txt`
  because `stage.cpp` keys off "readable at source" not "present in staged
  tree". The runtime pick-up worked - the second run's trace advances past the
  `NtGetNlsSectionPtr(l_intl.nls)` line that killed the first run.
