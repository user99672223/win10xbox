status: wip

# Slice: wt-media-atl

Follow-up of wt-media (docs/slices/wt-media.md). Goal: COM class registration works for Wine's builtins in the guest, so the
propsys and windowscodecs suites stop failing with REGDB_E_CLASSNOTREG (0x80040154).

## Cause (verified in the Wine 11.9 sources)

* `DllRegisterServer` of propsys and windowscodecs is `__wine_register_resources` (libs/winecrt0/register.c): it enumerates the
  `WINE_REGISTRY` resources (generated from the dll's .idl files) and feeds them to the ATL registrar:
  `LoadLibraryW(L"atl100.dll")` + `AtlCreateRegistrar`. Without atl100.dll it returns E_NOINTERFACE (what wt-media measured).
* atl100's imports (uuid ole32 oleaut32 user32 gdi32 advapi32 shlwapi) are all in wine-pe/dlls.txt already.
* The registry overlay of core/registry is in memory in the host process; `SaveOverlay`/`LoadOverlay` are not called anywhere
  but the unit test. A separate `regdll.exe` process (wt-media's step) therefore cannot leave keys for the unit that follows.
  media.sh now registers inside the test process, from a constructor in the generated test list, before `main`, and prints
  `media-reg: <dll> DllRegisterServer=0x........ HKCR\CLSID\{...}\InprocServer32 open=<LONG>`.

## Changes

* wine-pe/dlls.txt: `atl100`.
* wine-pe/tests/media.sh: the regdll.exe step is replaced by the in-process registration above.

(measurements below)
