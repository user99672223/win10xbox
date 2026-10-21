| changed | category | name | baseline-win32 | laptop-uwp | xbox-uwp |
|---|---|---|---|---|---|
| yes | A | loadlibrary-out-of-package | OK | OK | MISSING |
| yes | B | load:d3d9 | OK | OK | MISSING |
| yes | B | load:gdiplus | OK | OK | MISSING |
| yes | B | load:opengl32 | OK | OK | MISSING |
| yes | F | read:drive-c-root | OK | DENIED | DENIED |
| yes | F | read:known-documents | OK | DENIED | DENIED |
| yes | F | read:package-install | SKIPPED | OK | OK |
| yes | F | read:package-localstate | SKIPPED | OK | OK |
| yes | F | read:programdata | OK | DENIED | DENIED |
| yes | F | read:system32 | OK | OK | MISSING |
| yes | F | read:userprofile | OK | DENIED | DENIED |
| yes | F | write:drive-c-root | DENIED | DENIED | INFO |
| yes | F | write:known-documents | OK | DENIED | DENIED |
| yes | F | write:package-install | SKIPPED | DENIED | DENIED |
| yes | F | write:package-localstate | SKIPPED | OK | OK |
| yes | F | write:programdata | OK | DENIED | DENIED |
| yes | F | write:system32 | DENIED | DENIED | INFO |
| yes | F | write:userprofile | OK | DENIED | DENIED |
| yes | F | write:windows-dir | DENIED | DENIED | INFO |
| yes | G | write:HKCU | OK | DENIED | DENIED |
| yes | I | bridge:d3d9on12 | OK | OK | NOTIMPL |
| yes | I | bridge:glon12 | OK | OK | MISSING |
| yes | I | bridge:vulkan | OK | OK | MISSING |
| yes | L | createprocess-notepad | OK | WARN | MISSING |
| yes | L | package-identity | INFO | OK | OK |
| yes | M | firmware-acpi | OK | DENIED | DENIED |
| yes | M | firmware-enum | OK | DENIED | DENIED |
| yes | M | firmware-smbios | OK | DENIED | DENIED |

## SUMMARY
Keys: 243 total, 28 changed (status differs or absent).

### Status counts per environment

| status | baseline-win32 | laptop-uwp | xbox-uwp |
|---|---|---|---|
| OK | 219 | 211 | 203 |
| WARN | 0 | 1 | 0 |
| INFO | 15 | 14 | 17 |
| DENIED | 5 | 17 | 14 |
| NOTIMPL | 0 | 0 | 1 |
| MISSING | 0 | 0 | 8 |
| SKIPPED | 4 | 0 | 0 |
| records | 243 | 243 | 243 |
| walls vs baseline | - | 11 | 20 |

### Regressions vs baseline 'baseline-win32'
OK in the baseline but DENIED, MISSING, NOTIMPL, CRASH or absent in a later report:

**[A] dynamic code exec**
- `loadlibrary-out-of-package`: xbox-uwp=MISSING

**[B] DLL inventory**
- `load:d3d9`: xbox-uwp=MISSING
- `load:gdiplus`: xbox-uwp=MISSING
- `load:opengl32`: xbox-uwp=MISSING

**[F] filesystem**
- `read:drive-c-root`: laptop-uwp=DENIED, xbox-uwp=DENIED
- `read:known-documents`: laptop-uwp=DENIED, xbox-uwp=DENIED
- `read:programdata`: laptop-uwp=DENIED, xbox-uwp=DENIED
- `read:system32`: xbox-uwp=MISSING
- `read:userprofile`: laptop-uwp=DENIED, xbox-uwp=DENIED
- `write:known-documents`: laptop-uwp=DENIED, xbox-uwp=DENIED
- `write:programdata`: laptop-uwp=DENIED, xbox-uwp=DENIED
- `write:userprofile`: laptop-uwp=DENIED, xbox-uwp=DENIED

**[G] registry**
- `write:HKCU`: laptop-uwp=DENIED, xbox-uwp=DENIED

**[I] graphics**
- `bridge:d3d9on12`: xbox-uwp=NOTIMPL
- `bridge:glon12`: xbox-uwp=MISSING
- `bridge:vulkan`: xbox-uwp=MISSING

**[L] processes**
- `createprocess-notepad`: xbox-uwp=MISSING

**[M] system/memory**
- `firmware-acpi`: laptop-uwp=DENIED, xbox-uwp=DENIED
- `firmware-enum`: laptop-uwp=DENIED, xbox-uwp=DENIED
- `firmware-smbios`: laptop-uwp=DENIED, xbox-uwp=DENIED

Total: 20 key(s) walled in at least one later environment.
