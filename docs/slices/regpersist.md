status: wip

# Slice regpersist: the registry overlay survives the process and reaches children

Problem: the registry overlay (D13) lived only in the host process, so a program's registry writes vanished when it exited
and a child started with `NtCreateUserProcess` (a new host process on the same machine folder) did not see them.

Design: docs/DISPATCHER.md "Registry persistence". The overlay is one file, `<machine>\registry-overlay.bin` (the existing
`SaveOverlay` format), loaded after `registry::Initialize`, saved on `NtFlushKey`, in `GuestExit` and before
`NtCreateUserProcess` creates the child; atomic (temp file + rename) under a lock file; read-only folders log one FIXME.
