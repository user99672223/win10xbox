status: wip

# Slice wt-ntdll-rest: Wine's ntdll `exception`, `info`, `pipe`, `sync`, `threadpool`, `virtual`, `wow64` units through the layer

Order: exception, info, pipe, sync, threadpool, virtual, wow64. Each unit joins `wine-pe/tests/ntdll.allowlist` (0
failures) or `wine-pe/tests/ntdll.parity` (Windows fails some assertions itself; the native failing lines are recorded
here) only when measured on both Windows jobs. Start: docs/slices/wt-ntdll.md (exception/info/pipe/sync/threadpool/
virtual crash with an access violation, wow64 completes with 137 failures), harness of docs/slices/wt-ntdll-file.md
(parity gate) and the registry slice merged in.

## Measurements

(first diagnostic run pending)
