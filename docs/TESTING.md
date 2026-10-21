# Testing

| Suite | Where | Runs on |
|-------|-------|---------|
| `unit.*` | `tests/unit` | linux-cross (Wine) + windows |
| `integration.*` | `tests/integration` | windows (real EXEs through host-win32) |
| Wine conformance subset | `tests/wine-tests` | windows (M3) |
| kmtest subset | `tests/kmtest` | windows (M4) |

Fidelity baseline numbers (Wine test subset pass counts) are recorded here
once M3 lands.
