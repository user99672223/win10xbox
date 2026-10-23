status: ready

# Slice regpersist: the registry overlay survives the process and reaches children

Run: https://github.com/user99672223/win10xbox/actions/runs/39956672547 (ci.yml green on linux-cross, windows (Debug) and
windows (RelWithDebInfo), head 2947159; the commit that sets this status changes only this file). wine-pe.yml on the
same head: https://github.com/user99672223/win10xbox/actions/runs/39956672615 (green). host-uwp.yml (main only, started with
workflow_dispatch): https://github.com/user99672223/win10xbox/actions/runs/39959702136 builds and bundles
`guest_regpersist.exe`; its only error is `guest(s) missing from the .msix: guest_msctf_stub.exe, guest_net.exe`, which main's
own run 39948347965 has too (not this slice's files).

Problem (found by the -wt-media-atl slice): the registry overlay (D13) lived only in the host process, so a program's registry
writes vanished when it exited, and a child started through `NtCreateUserProcess` (a new host process on the same machine
folder) did not see them.

## What runs through the layer

`tests/guest/regpersist.c` as `integration.guest_regpersist_write` and `integration.guest_regpersist_read` (builtin-only
machine folder, console DLL set, `DEPENDS` write -> read, three jobs, "Gate - nothing skipped" PASS: 81/81 ran on linux-cross,
89/89 on both Windows jobs). The passing tests' output is not in the job logs; these are the lines the tests require and
the guest prints (a missing one or any `regpersist: ... FAILED` fails the test):

    write run (exit 42):  regpersist: write ok / regpersist: child ok / regpersist: done
    read run, "read" argument, a new host process (exit 42):
      regpersist: sz ok, expand ok, multisz ok, dword ok, qword ok, binary ok, default ok, prespawn ok, subkey ok / regpersist: done

The write run writes `HKCU\Software\win10xbox\persist` (REG_SZ, REG_EXPAND_SZ, REG_MULTI_SZ, REG_DWORD, REG_QWORD,
REG_BINARY, the default value, a subkey value), `RegFlushKey`, then one more value that is NOT flushed (`prespawn`), and runs
itself with `CreateProcessW` as `child`; the child reads `sz` (flushed) and `prespawn` (saved by the layer just before it
started) and exits 7. The unit test `registry_overlay_persist_atomic_lock` (tests/unit/test_registry.cpp) covers: nothing
written when nothing changed, only the changed key in the file (the Initialize-seeded keys are not written back), flush
writes, no `.tmp` left, a save waits for another holder of the lock file, a second session loads the first one's writes,
a folder where the file cannot be replaced still answers `NtFlushKey` with success.

## Design (docs/DISPATCHER.md "Registry persistence")

One file, `<machine>\registry-overlay.bin` (the existing `SaveOverlay` format, unchanged), loaded by `process::Launch` right
after `registry::Initialize` (`registry::EnablePersistence`), saved by `registry::PersistOverlay` on `NtFlushKey`, in
`GuestExit` (before the other guest threads are killed) and in `Xl_NtCreateUserProcess` before the child host process is
created. Atomic: `registry-overlay.bin.tmp` is written, flushed and renamed over the old file; `registry-overlay.lock`
(no sharing) serialises load and save across processes. Last writer wins per save. Only differences from what `Initialize`
seeded are written, so a `machine.json` edit still shows and a process that changed nothing writes nothing (it does not clobber
another process's file). A read-only folder or an unreplaceable file logs one FIXME; nothing fails. No `machine.json` knob: the
UWP host's LocalState machine folder is writable (D15).

Test isolation (needed because the file now carries state between processes): `run_guest.cmake` deletes
`<machine>\registry-overlay.bin` before every guest run except `guest_regpersist_read` (`KEEP_REGISTRY_OVERLAY`), and
`run_wine_test.cmake`, `run_ntdll.cmake` and the shell runners of wine-pe/tests (advapi32, gdi32, kernel32, msvcrt, shell32)
delete it before every unit. The first push without that went red on linux-cross: `winetest_advapi32` ran its units one
after the other on one folder, the crashed `registry` unit's leftovers changed `security` and `crypt`.

## Syscalls changed

`NtFlushKey` (Emulate, registry): after the handle check it saves the overlay file; failing to write never changes the
answer. `NtTerminateProcess` of the current process (via `GuestExit`) and `NtCreateUserProcess` save as above. No syscall
added, no policy change (docs/SYSCALLS.md unchanged). DLLs added to wine-pe/dlls.txt: none.

## What still fails

* Cross-process write visibility without a flush: a write made after a process's last save reaches a running child (or any
  other process) only through a later flush or at the writer's exit, and the file is read only at a process's start, so
  a running process never sees another process's later writes (no change notification across processes).
* Concurrent writers do not merge: the whole overlay of the later save replaces the file (last writer wins), so two
  processes that both write lose the earlier one's changes made after the later one started.
* A read-only machine folder (or one where the file cannot be replaced) persists nothing, with one FIXME in the log.
* A host process that dies without `NtTerminateProcess` (a crash of the host itself) loses its writes since the last flush.
