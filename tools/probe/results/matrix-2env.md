| changed | category | name | baseline-win32 | laptop-uwp |
|---|---|---|---|---|
| yes | F | read:drive-c-root | OK | DENIED |
| yes | F | read:known-documents | OK | DENIED |
| yes | F | read:package-install | SKIPPED | OK |
| yes | F | read:package-localstate | SKIPPED | OK |
| yes | F | read:programdata | OK | DENIED |
| yes | F | read:userprofile | OK | DENIED |
| yes | F | write:known-documents | OK | DENIED |
| yes | F | write:package-install | SKIPPED | DENIED |
| yes | F | write:package-localstate | SKIPPED | OK |
| yes | F | write:programdata | OK | DENIED |
| yes | F | write:userprofile | OK | DENIED |
| yes | G | write:HKCU | OK | DENIED |
| yes | L | createprocess-notepad | OK | WARN |
| yes | L | package-identity | INFO | OK |
| yes | M | firmware-acpi | OK | DENIED |
| yes | M | firmware-enum | OK | DENIED |
| yes | M | firmware-smbios | OK | DENIED |

## SUMMARY
Keys: 243 total, 17 changed (status differs or absent).

### Status counts per environment

| status | baseline-win32 | laptop-uwp |
|---|---|---|
| OK | 219 | 211 |
| WARN | 0 | 1 |
| INFO | 15 | 14 |
| DENIED | 5 | 17 |
| SKIPPED | 4 | 0 |
| records | 243 | 243 |
| walls vs baseline | - | 11 |

### Regressions vs baseline 'baseline-win32'
OK in the baseline but DENIED, MISSING, NOTIMPL, CRASH or absent in a later report:

**[F] filesystem**
- `read:drive-c-root`: laptop-uwp=DENIED
- `read:known-documents`: laptop-uwp=DENIED
- `read:programdata`: laptop-uwp=DENIED
- `read:userprofile`: laptop-uwp=DENIED
- `write:known-documents`: laptop-uwp=DENIED
- `write:programdata`: laptop-uwp=DENIED
- `write:userprofile`: laptop-uwp=DENIED

**[G] registry**
- `write:HKCU`: laptop-uwp=DENIED

**[M] system/memory**
- `firmware-acpi`: laptop-uwp=DENIED
- `firmware-enum`: laptop-uwp=DENIED
- `firmware-smbios`: laptop-uwp=DENIED

Total: 11 key(s) walled in at least one later environment.
