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

## Measured (ci run 39946633669, head 46b03a1, linux-cross; before = docs/slices/wt-media.md)

`media-reg: propsys.dll DllRegisterServer=0x00000000 HKCR\CLSID\{B8967F85-58AE-4F46-9FB2-5D7904798F4B}\InprocServer32 open=0`
and the same for `windowscodecs.dll` / `{CACAF262-9370-4615-A13B-9F5539DA4C0A}`: both return S_OK and the key is readable
through HKEY_CLASSES_ROOT in the same process (so the keys land in the overlay).

| suite:unit | before | after (executed, failures) | first failing line now |
|---|---|---|---|
| propsys:propsys | crash `propsys.c:2388 CoC` | ok, 1159 executed, 0 failures, 32 todo, 1 skipped | (`propsys.c:2397: IPersistSerializedPropStorage not supported`, skipped) |
| windowscodecs:bitmap | crash, 0x80040154 | ok, 647 executed, 12 failures | `bitmap.c:753: CreateIconIndirect failed`, `bitmap.c:760/766: ... 0x80070078` (user32 icons), then `bitmap.c:855` pixels 0 (x9) |
| windowscodecs:bmpformat | crash | ok, 298, 0 | |
| windowscodecs:converter | crash | ok, 20201, 0 (145 todo, 9 skipped) | |
| windowscodecs:ddsformat | 1 failure | ok, 3854, 0 (30 todo) | |
| windowscodecs:gifformat | 1 failure | ok, 843, 0 | |
| windowscodecs:icoformat | 7 failures | ok, 44, 0 (2 todo) | |
| windowscodecs:info | crash | ok, 1011, 0 (1 todo) | |
| windowscodecs:jpegformat | 1 failure | ok, 22, 0 | |
| windowscodecs:metadata | crash | ok, 6808, 0 (43 todo) | |
| windowscodecs:palette | crash | ok, 2882, 0 | |
| windowscodecs:pngformat | 1 failure | ok, 513, 0 (4 todo) | |
| windowscodecs:propertybag | crash | ok, 49, 0 | |
| windowscodecs:stream | crash | ok, 361, 0 | |
| windowscodecs:tiffformat | 1 failure | ok, 990, 0 (1 todo) | |
| windowscodecs:wmpformat | 1 failure (0x80040154) | ok, 1 executed, 1 failure | `wmpformat.c:92: WmpDecoder isn't available, skipping test` (a `win_skip`, which Wine's harness counts as a failure on Wine platforms; the JPEG-XR decoder needs jxrlib, not part of this build) |

The baselines propsys.baseline and windowscodecs.baseline hold these numbers (14 of 15 windowscodecs units and propsys
complete with 0 failures). mlang, gdiplus and dwrite are unchanged (same numbers as in wt-media.md; the gate holds).
Job time of windowscodecs' units: 54 s (3 to 6 s each), propsys 2 s.

## What still fails

* windowscodecs:bitmap: `CreateIconIndirect` fails in the guest (the user32/win32k icon path, not COM), so
  `CreateBitmapFromHICON` returns 0x80070078 (ERROR_CALL_NOT_IMPLEMENTED) and the HICON pixel checks read 0.
* windowscodecs:wmpformat: no WmpDecoder (jxrlib is not built).
* Out of scope and unchanged: gdiplus `image` (c0000090 NaN/MXCSR crash), gdiplus `brush`/`font`/`metafile` crashes, `graphics`
  timeout, dwrite and the font tests (no fonts staged).
* The registry overlay is per host process (`SaveOverlay`/`LoadOverlay` are not called by the host): registering classes once
  in a separate `regsvr32` process, as on an installed Windows, is not possible yet; media.sh registers in each unit's process.
