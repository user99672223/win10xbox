# Status

## M0 - repo skeleton (tag `m0`)

Layout created per `docs/ARCHITECTURE.md`; the capability probe moved to
`tools/probe/` untouched. Eleven third-party projects pinned as shallow
submodules, Wine and ReactOS as pinned-tag fetches. CMake builds `core/`
(static lib with config + tracing), `host-win32` ("hello"), and the unit/
integration test harness with both MSVC and llvm-mingw. CI: `linux-cross`
(llvm-mingw + Wine smoke) and `windows` (MSVC Debug/RelWithDebInfo, sccache,
machine folder populated from the runner's System32 at test time).

## M1 - loader (tag `m1`)

`core/loader` manually maps x64 PE images (sections, relocations, protections,
host `.pdata` registration), resolves exports/forwarders, rewrites the guest
ntdll's syscall thunks to enter our dispatcher, and scans executable code with
Zydis to redirect `cpuid`/`rdtsc`/`rdtscp`/`syscall` to trampolines (inline
near-jump where the displaced bytes relocate safely, `int3`+VEH otherwise).
Validated under Wine on the real Wine-built `ntdll.dll`: all 258 syscall
thunks matched and were rewritten. Guest test programs (`tests/guest`) build
with the host toolchain; `test_loader` has 21 cases. PE/loader corner cases
and the two scanner strategies are documented in `docs/LOADER.md`. SEH-based
tests are MSVC-only (clang-mingw has no `__try`), so the mingw smoke build
stays green.

## M2 - dispatcher + subsystems (tag `m2`)

`core/dispatch` routes every guest syscall through a table generated from
Wine's syscall numbers (`tools/gen_syscalls.py`, 258 entries) plus a
hand-maintained policy (`core/dispatch/syscall_policy.inc`): 163 pass-through
to the host ntdll by name, 29 translate (paths/handles/timeouts), 45 emulate,
21 todo (loud `STATUS_NOT_IMPLEMENTED`). Per-channel relay tracing logs every
call with args and NTSTATUS. `SEC_IMAGE` sections are emulated via the loader;
63 process-wide `Rtl*` routines are forwarded to the host ntdll (heap, locks,
exception/unwind - see `DECISIONS.md` E2); the guest's eight Wine unix calls
are answered. Subsystems: VFS (machine-folder path translation + native/
builtin DLL routing), registry (own bounds-checked REGF reader/writer + write
overlay over supplied hives), virtual clock (QPC/TSC/system time +
KUSER_SHARED_DATA page), token (interactive medium-integrity user identity).
`process::Launch` boots Wine's PE ntdll inside the host, loads kernelbase/
kernel32/ucrtbase from the machine folder, and runs the program.

End-to-end under Wine: `win10xbox C:\guest\guest_hello.exe` prints
`hello from guest` and exits 42; `guest_cpuid` sees `AuthenticAMD` with the
hypervisor-present bit cleared (natively the box is Intel with the bit set),
proving the scanner patches are live. `SYSCALLS.md` is generated; the
per-syscall status table is the fidelity ledger going forward.

### CI gating and real-Windows status

- The whole tree builds and links under **MSVC** (windows-latest, Debug and
  RelWithDebInfo) and under **llvm-mingw**; `unit.core` (109 cases across
  trace/vfs/registry/clock/token/loader/dispatch) is green on both.
- The guest **end-to-end** tests (`integration.guest_hello`, `guest_cpuid`:
  boot a real PE through the full layer, check output and exit code) are a
  **hard gate on the Wine job** (`linux-cross`), which downloads the Wine PE
  builtins and stages the NLS data the emulated `NtGetNlsSectionPtr` serves.
- On a **real-Windows** host the guest currently boots through the entire
  Wine-ntdll init (loader, NLS/locale, process params) and runs to
  completion with the correct exit code, but its **stdio is not yet captured**
  (Wine's kernelbase console path over a real-Windows pipe). These two tests
  therefore run **non-blocking** on the Windows CI job (visible, not gating)
  until that is resolved. This is the lead M3 bring-up item, tracked here;
  it is a real-Windows-only wiring gap, not an architectural one (the same
  path is proven on the Wine gate).
